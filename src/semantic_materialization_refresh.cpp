#include "semantic_definition_exception.hpp"
#include "duckdb/catalog/catalog.hpp"
#include "duckdb/catalog/entry_lookup_info.hpp"
#include "duckdb/parser/expression/star_expression.hpp"
#include "duckdb/parser/query_node/set_operation_node.hpp"
#include "duckdb/common/exception/binder_exception.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/common/to_string.hpp"
#include "duckdb/common/types/uuid.hpp"
#include "duckdb/main/client_context.hpp"
#include "duckdb/parser/expression/constant_expression.hpp"
#include "duckdb/parser/expression/function_expression.hpp"
#include "duckdb/parser/expression/operator_expression.hpp"
#include "duckdb/parser/expression/subquery_expression.hpp"
#include "duckdb/parser/parsed_data/create_table_info.hpp"
#include "duckdb/parser/parser.hpp"
#include "duckdb/parser/statement/create_statement.hpp"
#include "duckdb/parser/tableref/basetableref.hpp"
#include "semantic_ast_helpers.hpp"
#include "semantic_aggregate_state.hpp"
#include "semantic_lowering.hpp"
#include "duckdb/main/attached_database.hpp"
#include "duckdb/main/database_manager.hpp"
#include "semantic_materialization_internal.hpp"
#include "semantic_materialization_storage.hpp"

namespace duckdb {

namespace {

//! Logical tables of a refresh: tracked tables at a snapshot, replaced tables (changed-row images), else as defined
class RefreshSources : public SemanticTableSources {
public:
	RefreshSources(const SemanticModel &model, const map<idx_t, SemanticSourceTable> &tracked,
	               const map<string, idx_t> &snapshots)
	    : model(model), tracked(tracked), snapshots(snapshots) {
	}

	unique_ptr<TableRef> Source(idx_t table) const override {
		auto replaced = replacements.find(table);
		if (replaced != replacements.end()) {
			return replaced->second->Copy();
		}
		auto source = tracked.find(table);
		if (source != tracked.end() && snapshots.count(source->second.catalog)) {
			return SemanticChangeTracker::AtSnapshot(source->second, snapshots.at(source->second.catalog));
		}
		return model.tables[table].source->Copy();
	}

	map<idx_t, unique_ptr<TableRef>> replacements;

private:
	const SemanticModel &model;
	const map<idx_t, SemanticSourceTable> &tracked;
	const map<string, idx_t> &snapshots;
};

string Literal(const string &value) {
	return KeywordHelper::WriteQuoted(value, '\'');
}

//! The rows among the changed rows `changes` (a temp table: rowid, snapshot_id, change_type, columns) as of after the
//! changes (new images) or before them (old images). DuckLake row ids are stable and never reused, and an update is
//! a deletion and an insertion of the same row id in one snapshot: a row's new image is its last insertion unless it
//! was deleted afterwards, its old image its first deletion unless it was inserted before
unique_ptr<TableRef> ChangedRowImages(const string &changes, bool new_images) {
	auto order =
	    new_images ? "snapshot_id DESC, change_type = 'insert' DESC" : "snapshot_id, change_type = 'delete' DESC";
	auto kind = new_images ? "insert" : "delete";
	auto sql = "SELECT * EXCLUDE (snapshot_id, change_type, __sv_rn) FROM (SELECT *, row_number() OVER (PARTITION BY "
	           "rowid ORDER BY " +
	           string(order) + ") AS __sv_rn FROM " + changes + ") WHERE __sv_rn = 1 AND change_type = '" + kind + "'";
	Parser parser;
	parser.ParseQuery(sql);
	auto select = unique_ptr_cast<SQLStatement, SelectStatement>(std::move(parser.statements[0]));
	return make_uniq<SubqueryRef>(std::move(select));
}

unique_ptr<ParsedExpression> Exists(unique_ptr<SelectNode> node) {
	auto subquery = make_uniq<SubqueryExpression>();
	subquery->subquery_type = SubqueryType::EXISTS;
	subquery->subquery = MakeSelectStatement(std::move(node));
	return std::move(subquery);
}

//! EXISTS (SELECT 1 FROM <temp table> k WHERE k.<column> = <value> AND ...)
unique_ptr<ParsedExpression> ExistsIn(const string &temp_table, const vector<string> &columns,
                                      vector<unique_ptr<ParsedExpression>> values, bool null_safe) {
	auto node = make_uniq<SelectNode>();
	node->select_list.push_back(make_uniq<ConstantExpression>(Value::INTEGER(1)));
	auto ref = make_uniq<BaseTableRef>();
	ref->table_name = temp_table;
	ref->alias = "__sv_k";
	node->from_table = std::move(ref);
	vector<unique_ptr<ParsedExpression>> conditions;
	for (idx_t c = 0; c < columns.size(); c++) {
		conditions.push_back(
		    Compare(null_safe ? ExpressionType::COMPARE_NOT_DISTINCT_FROM : ExpressionType::COMPARE_EQUAL,
		            ColumnRef(columns[c], "__sv_k"), std::move(values[c])));
	}
	node->where_clause = AndAll(std::move(conditions));
	return Exists(std::move(node));
}

string Join(const vector<string> &values, const string &separator) {
	return StringUtil::Join(values, separator);
}

struct RefreshSnapshots {
	map<string, SemanticCatalogChanges> changes;
	map<string, idx_t> current;
};

class MaterializationRefresh {
public:
	MaterializationRefresh(ClientContext &context, const SemanticModel &model,
	                       const SemanticMaterialization &materialization, Connection &connection)
	    : model(model), materialization(materialization), state(*materialization.state), tracker(context),
	      connection(connection), compiler(model, context), inventory(context, model, materialization) {
	}

	SemanticMaterializationRefreshResult Build() {
		auto snapshots = ReadChanges({});
		auto result = Result(snapshots.current, "FULL", "initial build");
		FullBuild(snapshots.current);
		WriteMetadata(connection, model, materialization, state, result.watermark, result.verified_at, result.outcome,
		              result.detail);
		return result;
	}

	SemanticMaterializationRefreshResult Refresh(SemanticRefreshMode mode) {
		map<string, idx_t> watermark;
		bool built;
		string unbuilt_reason;
		{
			lock_guard<mutex> guard(state.lock);
			watermark = state.watermark;
			built = state.built;
			unbuilt_reason = built ? string() : state.last_refresh_detail;
		}
		auto snapshots = ReadChanges(watermark);
		auto why_full = FullRefreshReason(mode, built, watermark, snapshots.changes, unbuilt_reason);
		if (mode == SemanticRefreshMode::INCREMENTAL && !why_full.empty()) {
			throw BinderException("Materialization \"%s\" cannot be refreshed incrementally: %s", materialization.name,
			                      why_full);
		}
		auto result = Result(snapshots.current, "FULL", why_full);
		if (!why_full.empty()) {
			FullBuild(snapshots.current);
		} else {
			string rebuilt;
			auto groups = IncrementalRefresh(mode, watermark, snapshots.current, snapshots.changes, rebuilt);
			result.outcome = groups.IsValid() ? "INCREMENTAL" : "NONE";
			result.detail = groups.IsValid() ? to_string(groups.GetIndex()) + " group(s) changed" : "no source changes";
			if (!rebuilt.empty()) {
				result.outcome = groups.IsValid() ? "INCREMENTAL" : "FULL";
				result.detail = groups.IsValid() ? result.detail + "; " + rebuilt : rebuilt;
			}
		}
		// Metadata changes with the tables. A refresh without changes has nothing to persist.
		if (!why_full.empty()) {
			WriteMetadata(connection, model, materialization, state, result.watermark, result.verified_at,
			              result.outcome, result.detail);
		} else if (result.outcome != "NONE") {
			UpdateMetadata(connection, materialization, state, result.watermark, result.verified_at, result.outcome,
			               result.detail);
		}
		return result;
	}

private:
	SemanticMaterializationRefreshResult Result(const map<string, idx_t> &current, string outcome,
	                                            string detail) const {
		return {inventory.complete ? current : map<string, idx_t>(), Timestamp::GetCurrentTimestamp(),
		        std::move(outcome), std::move(detail)};
	}

	//! One metadata query per source catalog, before row images are read at explicit snapshots.
	RefreshSnapshots ReadChanges(const map<string, idx_t> &watermark) {
		RefreshSnapshots result;
		if (inventory.complete) {
			for (auto &entry : inventory.by_catalog) {
				optional_idx since;
				auto previous = watermark.find(entry.first);
				if (previous != watermark.end()) {
					since = previous->second;
				}
				auto changes = tracker.CatalogChanges(entry.first, since);
				result.current.emplace(entry.first, changes.current);
				result.changes.emplace(entry.first, std::move(changes));
			}
		}
		return result;
	}

	//! Why the refresh must rebuild everything; empty when it can be incremental (runs in the refresh's transaction)
	string FullRefreshReason(SemanticRefreshMode mode, bool built, const map<string, idx_t> &watermark,
	                         const map<string, SemanticCatalogChanges> &changes, const string &unbuilt_reason) {
		if (mode == SemanticRefreshMode::FULL) {
			return "FULL refresh requested";
		}
		if (!built && !unbuilt_reason.empty()) {
			return unbuilt_reason;
		}
		if (!built || !inventory.Covers(watermark)) {
			return materialization.incremental ? "it was not built from tracked snapshots"
			                                   : materialization.incremental_reason;
		}
		for (auto &entry : changes) {
			auto previous = watermark.at(entry.first);
			if (previous == entry.second.current) {
				continue;
			}
			if (!entry.second.available) {
				return "the changes since snapshot " + to_string(previous) + " of " + entry.first +
				       " are no longer available";
			}
			for (auto &source : inventory.by_table) {
				if (source.second.catalog == entry.first && entry.second.SchemaChanged(source.second)) {
					return "the definition of " + source.second.catalog + "." + source.second.schema + "." +
					       source.second.table + " changed since the last refresh";
				}
			}
		}
		for (idx_t b = 0; b < materialization.blocks.size(); b++) {
			EntryLookupInfo lookup(CatalogType::TABLE_ENTRY, state.tables[b]);
			if (!Catalog::GetEntry(*connection.context, state.catalog, state.schema, lookup,
			                       OnEntryNotFound::RETURN_NULL)) {
				return "its table " + state.tables[b] + " is missing";
			}
		}
		// a materialization that cannot follow changes is rebuilt once its sources changed
		if (!materialization.incremental) {
			for (auto &source : inventory.by_table) {
				auto &catalog = changes.at(source.second.catalog);
				if (watermark.at(source.second.catalog) != catalog.current && catalog.RowChanges(source.second).Any()) {
					return materialization.incremental_reason;
				}
			}
		}
		return string();
	}

	unique_ptr<MaterializedQueryResult> Execute(const string &sql) {
		auto result = connection.Query(sql);
		if (result->HasError()) {
			result->ThrowError("semantic view materialization: ");
		}
		return result;
	}

	void Execute(unique_ptr<SQLStatement> statement) {
		auto result = connection.context->Query(std::move(statement), false);
		if (result->HasError()) {
			result->ThrowError("semantic view materialization: ");
		}
	}

	idx_t Count(const string &table) {
		return Execute("SELECT count(*) FROM " + table)->GetValue(0, 0).GetValue<idx_t>();
	}

	//! CREATE OR REPLACE [TEMP] TABLE <catalog>.<schema>.<table> AS <select>
	void CreateTableAs(const string &catalog, const string &schema, const string &table,
	                   unique_ptr<SelectStatement> select, bool temporary) {
		auto info = make_uniq<CreateTableInfo>(catalog, schema, table);
		info->on_conflict = OnCreateConflict::REPLACE_ON_CONFLICT;
		info->temporary = temporary;
		info->query = std::move(select);
		auto create = make_uniq<CreateStatement>();
		create->info = std::move(info);
		Execute(std::move(create));
	}

	void CreateTempTableAs(const string &table, unique_ptr<SelectStatement> select) {
		CreateTableAs(TEMP_CATALOG, DEFAULT_SCHEMA, table, std::move(select), true);
	}

	unique_ptr<SelectStatement> States(idx_t b, const SemanticCompileOptions &options) {
		auto &block = materialization.blocks[b];
		return compiler.CompileStates(
		    materialization.spec, b,
		    [&](idx_t atom, const FunctionExpression &compiled) {
			    return SemanticStateExpressions(block.atoms[atom], compiled);
		    },
		    options);
	}

	void FullBuild(const map<string, idx_t> &snapshots) {
		Execute("CREATE SCHEMA IF NOT EXISTS " + QuoteIdentifier(state.catalog) + "." + QuoteIdentifier(state.schema));
		RefreshSources sources(model, inventory.by_table, snapshots);
		SemanticCompileOptions options;
		options.sources = &sources;
		for (idx_t b = 0; b < materialization.blocks.size(); b++) {
			CreateTableAs(state.catalog, state.schema, state.tables[b], States(b, options), false);
		}
	}

	vector<string> DimensionColumns() const {
		vector<string> result;
		for (idx_t d = 0; d < materialization.dimension_count; d++) {
			result.push_back(SemanticDimensionColumn(d));
		}
		return result;
	}

	//! Applies the source changes in (watermark, current] to every block; the number of changed groups, or invalid
	//! when no block was maintained incrementally. In AUTO mode, a block whose dimension tables changed is rebuilt
	//! instead: moving the rows that reach the changed rows scans its home table twice (as of before and as of now),
	//! while rebuilding it scans it once. `rebuilt` describes those blocks.
	optional_idx IncrementalRefresh(SemanticRefreshMode mode, const map<string, idx_t> &watermark,
	                                const map<string, idx_t> &current,
	                                const map<string, SemanticCatalogChanges> &changes, string &rebuilt) {
		optional_idx changed_groups;
		for (idx_t b = 0; b < materialization.blocks.size(); b++) {
			set<idx_t> changed;
			for (auto table : materialization.blocks[b].SourceTables()) {
				auto &source = inventory.by_table.at(table);
				if (watermark.at(source.catalog) == current.at(source.catalog)) {
					continue;
				}
				auto kinds = changes.at(source.catalog).RowChanges(source);
				if (kinds.Any()) {
					row_changes[table] = kinds;
					changed.insert(table);
				}
			}
			if (changed.empty()) {
				continue;
			}
			auto home = materialization.blocks[b].home;
			if (mode == SemanticRefreshMode::AUTO && (changed.size() > 1 || !changed.count(home))) {
				RefreshSources sources(model, inventory.by_table, current);
				SemanticCompileOptions options;
				options.sources = &sources;
				CreateTableAs(state.catalog, state.schema, state.tables[b], States(b, options), false);
				for (auto table : changed) {
					if (table != home) {
						auto kind = materialization.blocks[b].tables.count(table) ? "dimension table" : "child table";
						rebuilt = "rebuilt because " + string(kind) + " \"" + model.tables[table].name + "\" changed";
					}
				}
				continue;
			}
			auto groups = RefreshBlock(b, changed, watermark, current);
			changed_groups = (changed_groups.IsValid() ? changed_groups.GetIndex() : 0) + groups;
		}
		return changed_groups;
	}

	//! The temp table holding the changed rows of `table` (read once per refresh)
	string Changes(idx_t table, const map<string, idx_t> &watermark, const map<string, idx_t> &current) {
		auto name = "__sv_changes" + to_string(table);
		if (loaded_changes.insert(table).second) {
			auto &source = inventory.by_table.at(table);
			Execute("CREATE OR REPLACE TEMP TABLE " + name + " AS " +
			        SemanticChangeTracker::ChangesSQL(source, watermark.at(source.catalog), current.at(source.catalog),
			                                          row_changes.at(table)));
		}
		return name;
	}

	//! The state query of block `b` over `sources` (restricted by `filter`), as a delta part with `sign`
	void AddPart(idx_t b, const RefreshSources &sources, const SemanticRowFilter &filter, int sign,
	             vector<pair<unique_ptr<SelectStatement>, int>> &parts) {
		SemanticCompileOptions options;
		options.sources = &sources;
		options.row_filter = filter;
		parts.emplace_back(States(b, options), sign);
	}

	idx_t RefreshBlock(idx_t b, const set<idx_t> &changed, const map<string, idx_t> &watermark,
	                   const map<string, idx_t> &current) {
		auto &block = materialization.blocks[b];
		bool home_changed = changed.count(block.home) > 0;
		vector<pair<unique_ptr<SelectStatement>, int>> parts;
		string home_changes;
		// 1. changed rows of the home table: + their new image (joined as of now), - their old image (as of before)
		if (home_changed) {
			home_changes = Changes(block.home, watermark, current);
			RefreshSources new_rows(model, inventory.by_table, current);
			new_rows.replacements[block.home] = ChangedRowImages(home_changes, true);
			AddPart(b, new_rows, nullptr, 1, parts);
			RefreshSources old_rows(model, inventory.by_table, watermark);
			old_rows.replacements[block.home] = ChangedRowImages(home_changes, false);
			AddPart(b, old_rows, nullptr, -1, parts);
		}
		// 2. unchanged home rows that reach a changed row of another table, or whose nested aggregations read changed
		// rows of a child table: + as of now, - as of before
		struct Link {
			//! changed rows (temp table) and their columns matching `parent_columns` of table `parent`
			string changes;
			vector<string> columns;
			idx_t parent;
			vector<string> parent_columns;
		};
		vector<Link> links;
		for (auto table : changed) {
			if (table == block.home) {
				continue;
			}
			auto changes = Changes(table, watermark, current);
			if (block.tables.count(table)) {
				auto path = model.Path(block.home, table, block.using_relationships);
				auto &edge = path.back();
				auto &rel = model.relationships[edge.relationship];
				auto parent = path.size() >= 2 ? path[path.size() - 2].target : block.home;
				links.push_back({changes, edge.reversed ? rel.from_columns : rel.to_columns, parent,
				                 edge.reversed ? rel.to_columns : rel.from_columns});
			}
			for (auto &child : block.children) {
				if (child.table == table) {
					links.push_back({changes, child.columns, child.parent, child.parent_columns});
				}
			}
		}
		if (!links.empty()) {
			auto filter = [&](SemanticJoinScope &scope, const vector<unique_ptr<ParsedExpression>> &) {
				unique_ptr<ParsedExpression> result;
				for (auto &link : links) {
					vector<unique_ptr<ParsedExpression>> values;
					for (auto &column : link.parent_columns) {
						values.push_back(scope.ColumnReference(link.parent, column));
					}
					auto reaching = ExistsIn(link.changes, link.columns, std::move(values), false);
					result = result ? make_uniq<ConjunctionExpression>(ExpressionType::CONJUNCTION_OR,
					                                                   std::move(result), std::move(reaching))
					                : std::move(reaching);
				}
				if (home_changed) {
					vector<unique_ptr<ParsedExpression>> rowid;
					rowid.push_back(scope.ColumnReference(block.home, "rowid"));
					auto unchanged = make_uniq<OperatorExpression>(
					    ExpressionType::OPERATOR_NOT, ExistsIn(home_changes, {"rowid"}, std::move(rowid), false));
					result = make_uniq<ConjunctionExpression>(ExpressionType::CONJUNCTION_AND, std::move(unchanged),
					                                          std::move(result));
				}
				return result;
			};
			RefreshSources now(model, inventory.by_table, current);
			AddPart(b, now, filter, 1, parts);
			RefreshSources before(model, inventory.by_table, watermark);
			AddPart(b, before, filter, -1, parts);
		}
		return MergeDelta(b, std::move(parts), current);
	}

	//! Combines the delta parts per group and merges them into the block's table (MERGE, then removes the groups
	//! left without rows); groups whose MIN / MAX lost rows, or with opaque aggregates, are recomputed
	idx_t MergeDelta(idx_t b, vector<pair<unique_ptr<SelectStatement>, int>> parts, const map<string, idx_t> &current) {
		auto &block = materialization.blocks[b];
		auto table = materialization.TableName(b);
		auto dimensions = DimensionColumns();
		bool grouped = !dimensions.empty();
		auto signed_value = [](const string &column) {
			return "CASE WHEN __sv_sign > 0 THEN coalesce(" + column + ", 0) ELSE -coalesce(" + column + ", 0) END";
		};
		// the delta: per group, the signed sum of the parts' states
		vector<string> delta_columns;
		delta_columns.push_back("sum(" + signed_value(MATERIALIZED_ROWS_COLUMN) + ") AS " + MATERIALIZED_ROWS_COLUMN);
		delta_columns.push_back("sum(CASE WHEN __sv_sign < 0 THEN __sv_rows ELSE 0 END) AS __sv_removed");
		bool opaque = false;
		bool extremes = false;
		vector<SemanticAggregateMaintenance> maintenance;
		for (auto &atom : block.atoms) {
			auto plan = PlanSemanticAggregateMaintenance(atom);
			delta_columns.insert(delta_columns.end(), plan.delta.begin(), plan.delta.end());
			opaque = opaque || plan.recompute == SemanticRecompute::ALWAYS;
			extremes = extremes || plan.recompute == SemanticRecompute::DELETIONS;
			maintenance.push_back(std::move(plan));
		}
		auto delta = make_uniq<SelectNode>();
		for (auto &dimension : dimensions) {
			delta->select_list.push_back(ColumnRef(dimension));
			delta->groups.group_expressions.push_back(ColumnRef(dimension));
		}
		for (auto &expr : Parser::ParseExpressionList(Join(delta_columns, ", "))) {
			delta->select_list.push_back(std::move(expr));
		}
		auto unions = make_uniq<SetOperationNode>();
		unions->setop_type = SetOperationType::UNION_BY_NAME;
		unions->setop_all = true;
		for (auto &part : parts) {
			auto signed_part = make_uniq<SelectNode>();
			signed_part->select_list.push_back(make_uniq<StarExpression>());
			auto sign = make_uniq<ConstantExpression>(Value::INTEGER(part.second));
			sign->alias = "__sv_sign";
			signed_part->select_list.push_back(std::move(sign));
			signed_part->from_table = make_uniq<SubqueryRef>(std::move(part.first), "__sv_part");
			unions->children.push_back(std::move(signed_part));
		}
		GroupByAll(*delta);
		delta->from_table = MakeSubqueryRef(std::move(unions), "__sv_parts");
		CreateTempTableAs("__sv_delta", MakeSelectStatement(std::move(delta)));
		// merge it: stored state + delta
		vector<string> matches;
		for (auto &dimension : dimensions) {
			matches.push_back("__sv_m." + dimension + " IS NOT DISTINCT FROM __sv_d." + dimension);
		}
		auto match = matches.empty() ? string("true") : Join(matches, " AND ");
		auto plus = [](const string &column) {
			return "coalesce(__sv_m." + column + ", 0) + coalesce(__sv_d." + column + ", 0)";
		};
		vector<string> stored_columns = dimensions;
		stored_columns.push_back(MATERIALIZED_ROWS_COLUMN);
		vector<string> updates {string(MATERIALIZED_ROWS_COLUMN) + " = " + plus(MATERIALIZED_ROWS_COLUMN)};
		vector<string> inserted;
		for (auto &dimension : dimensions) {
			inserted.push_back("__sv_d." + dimension);
		}
		inserted.push_back("coalesce(__sv_d.__sv_rows, 0)");
		for (auto &plan : maintenance) {
			stored_columns.insert(stored_columns.end(), plan.columns.begin(), plan.columns.end());
			updates.insert(updates.end(), plan.updates.begin(), plan.updates.end());
			inserted.insert(inserted.end(), plan.inserts.begin(), plan.inserts.end());
		}
		auto merged =
		    Execute("MERGE INTO " + table + " AS __sv_m USING __sv_delta AS __sv_d ON " + match +
		            " WHEN MATCHED THEN UPDATE SET " + Join(updates, ", ") + " WHEN NOT MATCHED THEN INSERT (" +
		            Join(stored_columns, ", ") + ") VALUES (" + Join(inserted, ", ") + ")");
		auto changed_groups = merged->GetValue(0, 0).GetValue<idx_t>();
		if (grouped) {
			// groups without rows disappear (the only group of an ungrouped block stays)
			Execute("DELETE FROM " + table + " WHERE " + MATERIALIZED_ROWS_COLUMN + " = 0");
		}
		if (opaque || extremes) {
			auto recompute = opaque ? string("true") : string("coalesce(__sv_removed, 0) > 0");
			Execute("CREATE OR REPLACE TEMP TABLE __sv_recompute AS SELECT " +
			        (grouped ? Join(dimensions, ", ") : string("1 AS __sv_all")) + " FROM __sv_delta WHERE " +
			        recompute);
			if (Count("__sv_recompute") > 0) {
				if (grouped) {
					vector<string> recompute_matches;
					for (auto &dimension : dimensions) {
						recompute_matches.push_back("__sv_r." + dimension + " IS NOT DISTINCT FROM __sv_m." +
						                            dimension);
					}
					Execute("DELETE FROM " + table +
					        " AS __sv_m WHERE EXISTS (SELECT 1 FROM __sv_recompute AS __sv_r WHERE " +
					        Join(recompute_matches, " AND ") + ")");
				} else {
					Execute("DELETE FROM " + table);
				}
				RefreshSources now(model, inventory.by_table, current);
				SemanticCompileOptions options;
				options.sources = &now;
				if (grouped) {
					options.row_filter = [&](SemanticJoinScope &, const vector<unique_ptr<ParsedExpression>> &groups) {
						vector<unique_ptr<ParsedExpression>> values;
						for (auto &group : groups) {
							values.push_back(group->Copy());
						}
						return ExistsIn("__sv_recompute", dimensions, std::move(values), true);
					};
				}
				CreateTempTableAs("__sv_recomputed", States(b, options));
				Execute("INSERT INTO " + table + " (" + Join(stored_columns, ", ") + ") SELECT " +
				        Join(stored_columns, ", ") + " FROM __sv_recomputed");
			}
		}
		return changed_groups;
	}

	const SemanticModel &model;
	const SemanticMaterialization &materialization;
	SemanticMaterializationState &state;
	SemanticChangeTracker tracker;
	Connection &connection;
	SemanticQueryCompiler compiler;
	SemanticMaterializationSources inventory;
	//! Per changed table: the kinds of changes, and whether its changed rows were loaded
	map<idx_t, SemanticTableChanges> row_changes;
	set<idx_t> loaded_changes;
};

string PhysicalName(const string &name) {
	string result;
	for (auto c : StringUtil::Lower(name)) {
		result += StringUtil::CharacterIsAlphaNumeric(c) ? c : '_';
	}
	return result;
}

} // namespace

void SemanticMaterializationRefreshResult::Publish(SemanticMaterializationState &state) const {
	lock_guard<mutex> guard(state.lock);
	state.built = true;
	state.watermark = watermark;
	state.verified_at = verified_at;
	state.last_refresh = outcome;
	state.last_refresh_detail = detail;
}

string SemanticMaterializationRefreshResult::Describe(const string &name) const {
	if (outcome == "NONE") {
		return "Materialization " + name + " is up to date.";
	}
	return "Materialization " + name + " refreshed (" + outcome + ": " + detail + ").";
}

SemanticMaterializationRefreshResult BuildSemanticMaterialization(ClientContext &context, Connection &connection,
                                                                  const SemanticModel &model,
                                                                  SemanticMaterialization &materialization) {
	auto &state = *materialization.state;
	if (Catalog::GetCatalog(context, materialization.storage_catalog).GetAttached().IsReadOnly()) {
		throw BinderException("database \"%s\" is read-only", materialization.storage_catalog);
	}
	{
		lock_guard<mutex> guard(state.lock);
		state.owner = model.Name();
		state.catalog = materialization.storage_catalog;
		state.schema = materialization.storage_schema;
		state.tables.clear();
		auto identity = StringUtil::Replace(UUID::ToString(UUID::GenerateRandomUUID()), "-", "");
		for (idx_t b = 0; b < materialization.blocks.size(); b++) {
			state.tables.push_back(PhysicalName(model.Name().name) + "__" + PhysicalName(materialization.name) + "__" +
			                       to_string(b) + "__" + identity);
		}
	}
	for (auto &record : ReadStoredMaterializations(connection, state.catalog, model.Name())) {
		if (!StringUtil::CIEquals(record.location.name, materialization.name) ||
		    !StringUtil::CIEquals(record.location.schema, state.schema)) {
			continue;
		}
		ReattachStoredMaterialization(materialization, record);
		if (state.built) {
			bool exists = true;
			for (auto &table : state.tables) {
				EntryLookupInfo lookup(CatalogType::TABLE_ENTRY, table);
				exists = exists && Catalog::GetEntry(*connection.context, state.catalog, state.schema, lookup,
				                                     OnEntryNotFound::RETURN_NULL);
			}
			if (exists) {
				return {state.watermark, state.verified_at, state.last_refresh, state.last_refresh_detail};
			}
		}
		break;
	}
	return MaterializationRefresh(context, model, materialization, connection).Build();
}

string RefreshSemanticMaterialization(ClientContext &context, const SemanticModel &model,
                                      const SemanticMaterialization &materialization, SemanticRefreshMode mode) {
	auto &state = *materialization.state;
	lock_guard<mutex> refreshing(state.refresh_lock);
	{
		lock_guard<mutex> guard(state.lock);
		if (state.retired) {
			throw CatalogException("Materialization \"%s\" was removed by a concurrent statement",
			                       materialization.name);
		}
	}
	Connection connection(*context.db);
	connection.BeginTransaction();
	auto result = MaterializationRefresh(context, model, materialization, connection).Refresh(mode);
	connection.Commit();
	result.Publish(state);
	return result.Describe(materialization.name);
}

} // namespace duckdb
