#include "duckdb/common/exception/binder_exception.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/common/to_string.hpp"
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
#include "semantic_materialization_internal.hpp"

namespace duckdb {

static constexpr const char *MATERIALIZATION_SCHEMA = "__semantic_materializations";

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

class MaterializationRefresh {
public:
	MaterializationRefresh(ClientContext &context, const SemanticModel &model,
	                       const SemanticMaterialization &materialization)
	    : context(context), model(model), materialization(materialization), state(*materialization.state),
	      tracker(context), connection(tracker.GetConnection()), compiler(model, context) {
		for (auto &block : materialization.blocks) {
			for (auto table : block.tables) {
				string reason;
				auto source = ClassifySemanticSource(context, model, table, reason);
				if (source.Tracked()) {
					tracked[table] = source;
				} else {
					all_tracked = false;
				}
			}
		}
	}

	string Run(SemanticRefreshMode mode, bool initial) {
		lock_guard<mutex> refreshing(state.refresh_lock);
		map<string, idx_t> watermark;
		bool built;
		{
			lock_guard<mutex> guard(state.lock);
			watermark = state.watermark;
			built = state.built;
		}
		map<string, idx_t> current;
		if (all_tracked) {
			for (auto &entry : tracked) {
				if (!current.count(entry.second.catalog)) {
					current[entry.second.catalog] = tracker.CurrentSnapshot(entry.second.catalog);
				}
			}
		}
		auto why_full = initial ? string("initial build") : FullRefreshReason(mode, built, watermark, current);
		if (!initial && mode == SemanticRefreshMode::INCREMENTAL && !why_full.empty()) {
			throw BinderException("Materialization \"%s\" cannot be refreshed incrementally: %s", materialization.name,
			                      why_full);
		}
		string outcome;
		string detail;
		connection.BeginTransaction();
		try {
			if (!why_full.empty()) {
				FullBuild(current);
				outcome = "FULL";
				detail = why_full;
			} else {
				string rebuilt;
				auto groups = IncrementalRefresh(mode, watermark, current, rebuilt);
				outcome = groups.IsValid() ? "INCREMENTAL" : "NONE";
				detail = groups.IsValid() ? to_string(groups.GetIndex()) + " group(s) changed" : "no source changes";
				if (!rebuilt.empty()) {
					outcome = groups.IsValid() ? "INCREMENTAL" : "FULL";
					detail = groups.IsValid() ? detail + "; " + rebuilt : rebuilt;
				}
			}
			connection.Commit();
		} catch (std::exception &) {
			if (connection.HasActiveTransaction()) {
				connection.Rollback();
			}
			throw;
		}
		{
			lock_guard<mutex> guard(state.lock);
			state.built = true;
			state.watermark = all_tracked ? current : map<string, idx_t>();
			state.verified_at = Timestamp::GetCurrentTimestamp();
			state.last_refresh = outcome;
			state.last_refresh_detail = detail;
		}
		if (outcome == "NONE") {
			return "Materialization " + materialization.name + " is up to date.";
		}
		return "Materialization " + materialization.name + " refreshed (" + outcome + ": " + detail + ").";
	}

private:
	//! Why the refresh must rebuild everything; empty when it can be incremental
	string FullRefreshReason(SemanticRefreshMode mode, bool built, const map<string, idx_t> &watermark,
	                         const map<string, idx_t> &current) {
		if (mode == SemanticRefreshMode::FULL) {
			return "FULL refresh requested";
		}
		if (!materialization.incremental) {
			return materialization.incremental_reason;
		}
		if (!built || watermark.empty()) {
			return "it was not built from tracked snapshots";
		}
		for (auto &entry : current) {
			auto previous = watermark.find(entry.first);
			if (previous == watermark.end()) {
				return "it was not built from tracked snapshots";
			}
			if (previous->second == entry.second) {
				continue;
			}
			if (!tracker.SnapshotAvailable(entry.first, previous->second)) {
				return "the changes since snapshot " + to_string(previous->second) + " of " + entry.first +
				       " are no longer available";
			}
			for (auto &source : tracked) {
				if (source.second.catalog == entry.first &&
				    tracker.SchemaChanged(source.second, previous->second, entry.second)) {
					return "the schema of " + entry.first + " changed since the last refresh";
				}
			}
		}
		for (idx_t b = 0; b < materialization.blocks.size(); b++) {
			auto result = connection.Query("SELECT 1 FROM " + materialization.TableName(b) + " LIMIT 0");
			if (result->HasError()) {
				return "its table " + state.tables[b] + " is missing";
			}
		}
		return string();
	}

	void Execute(const string &sql) {
		auto result = connection.Query(sql);
		if (result->HasError()) {
			result->ThrowError("semantic view materialization: ");
		}
	}

	void Execute(unique_ptr<SQLStatement> statement) {
		auto result = connection.context->Query(std::move(statement), false);
		if (result->HasError()) {
			result->ThrowError("semantic view materialization: ");
		}
	}

	idx_t Count(const string &table) {
		auto result = connection.Query("SELECT count(*) FROM " + table);
		if (result->HasError()) {
			result->ThrowError("semantic view materialization: ");
		}
		return result->GetValue(0, 0).GetValue<idx_t>();
	}

	//! CREATE [OR REPLACE] [TEMP] TABLE <catalog>.<schema>.<table> AS <select>
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
		RefreshSources sources(model, tracked, snapshots);
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
	                                const map<string, idx_t> &current, string &rebuilt) {
		optional_idx changed_groups;
		for (idx_t b = 0; b < materialization.blocks.size(); b++) {
			set<idx_t> changed;
			for (auto table : materialization.blocks[b].tables) {
				auto &source = tracked.at(table);
				auto from = watermark.at(source.catalog);
				auto to = current.at(source.catalog);
				if (from == to) {
					continue;
				}
				if (!row_changes.count(table)) {
					row_changes[table] = tracker.RowChanges(source, from, to);
				}
				if (row_changes[table].Any()) {
					changed.insert(table);
				}
			}
			if (changed.empty()) {
				continue;
			}
			auto home = materialization.blocks[b].home;
			if (mode == SemanticRefreshMode::AUTO && (changed.size() > 1 || !changed.count(home))) {
				RefreshSources sources(model, tracked, current);
				SemanticCompileOptions options;
				options.sources = &sources;
				CreateTableAs(state.catalog, state.schema, state.tables[b], States(b, options), false);
				for (auto table : changed) {
					if (table != home) {
						rebuilt = "rebuilt because dimension table \"" + model.tables[table].name + "\" changed";
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
			auto &source = tracked.at(table);
			Execute("CREATE OR REPLACE TEMP TABLE " + name + " AS " +
			        SemanticChangeTracker::ChangesSQL(source, watermark.at(source.catalog), current.at(source.catalog),
			                                          row_changes.at(table)));
		}
		return name;
	}

	//! Adds the state query of block `b` over `sources` (restricted by `filter`) as a delta part with `sign`
	void AddPart(idx_t b, const RefreshSources &sources, const SemanticRowFilter &filter, int sign,
	             vector<pair<string, int>> &parts) {
		SemanticCompileOptions options;
		options.sources = &sources;
		options.row_filter = filter;
		auto name = "__sv_part" + to_string(parts.size());
		CreateTempTableAs(name, States(b, options));
		parts.emplace_back(name, sign);
	}

	idx_t RefreshBlock(idx_t b, const set<idx_t> &changed, const map<string, idx_t> &watermark,
	                   const map<string, idx_t> &current) {
		auto &block = materialization.blocks[b];
		bool home_changed = changed.count(block.home) > 0;
		vector<pair<string, int>> parts;
		// 1. changed rows of the home table: + their new image (joined as of now), - their old image (as of before)
		if (home_changed) {
			auto changes = Changes(block.home, watermark, current);
			Execute("CREATE OR REPLACE TEMP TABLE __sv_changed_rows AS SELECT DISTINCT rowid FROM " + changes);
			RefreshSources new_rows(model, tracked, current);
			new_rows.replacements[block.home] = ChangedRowImages(changes, true);
			AddPart(b, new_rows, nullptr, 1, parts);
			RefreshSources old_rows(model, tracked, watermark);
			old_rows.replacements[block.home] = ChangedRowImages(changes, false);
			AddPart(b, old_rows, nullptr, -1, parts);
		}
		// 2. unchanged home rows that reach a changed row of another table: + as of now, - as of before
		vector<idx_t> referenced;
		for (auto table : changed) {
			if (table != block.home) {
				referenced.push_back(table);
			}
		}
		if (!referenced.empty()) {
			for (auto table : referenced) {
				auto path = model.Path(block.home, table, block.using_relationships);
				auto &edge = path.back();
				auto &rel = model.relationships[edge.relationship];
				auto &keys = edge.reversed ? rel.from_columns : rel.to_columns;
				vector<string> quoted;
				for (auto &key : keys) {
					quoted.push_back(QuoteIdentifier(key));
				}
				Execute("CREATE OR REPLACE TEMP TABLE __sv_keys" + to_string(table) + " AS SELECT DISTINCT " +
				        Join(quoted, ", ") + " FROM " + Changes(table, watermark, current));
			}
			auto filter = [&](SemanticJoinScope &scope, const vector<unique_ptr<ParsedExpression>> &) {
				vector<unique_ptr<ParsedExpression>> reaching;
				for (auto table : referenced) {
					auto path = model.Path(block.home, table, block.using_relationships);
					auto &edge = path.back();
					auto &rel = model.relationships[edge.relationship];
					auto parent = path.size() >= 2 ? path[path.size() - 2].target : block.home;
					auto &parent_columns = edge.reversed ? rel.to_columns : rel.from_columns;
					auto &keys = edge.reversed ? rel.from_columns : rel.to_columns;
					vector<unique_ptr<ParsedExpression>> values;
					for (auto &column : parent_columns) {
						values.push_back(scope.ColumnReference(parent, column));
					}
					reaching.push_back(ExistsIn("__sv_keys" + to_string(table), keys, std::move(values), false));
				}
				unique_ptr<ParsedExpression> result;
				for (auto &condition : reaching) {
					result = result ? make_uniq<ConjunctionExpression>(ExpressionType::CONJUNCTION_OR,
					                                                   std::move(result), std::move(condition))
					                : std::move(condition);
				}
				if (home_changed) {
					vector<unique_ptr<ParsedExpression>> rowid;
					rowid.push_back(scope.ColumnReference(block.home, "rowid"));
					auto changed_row = ExistsIn("__sv_changed_rows", {"rowid"}, std::move(rowid), false);
					auto unchanged =
					    make_uniq<OperatorExpression>(ExpressionType::OPERATOR_NOT, std::move(changed_row));
					result = make_uniq<ConjunctionExpression>(ExpressionType::CONJUNCTION_AND, std::move(unchanged),
					                                          std::move(result));
				}
				return result;
			};
			RefreshSources now(model, tracked, current);
			AddPart(b, now, filter, 1, parts);
			RefreshSources before(model, tracked, watermark);
			AddPart(b, before, filter, -1, parts);
		}
		return MergeDelta(b, parts, current);
	}

	//! Combines the delta parts per group and merges them into the block's table
	idx_t MergeDelta(idx_t b, const vector<pair<string, int>> &parts, const map<string, idx_t> &current) {
		auto &block = materialization.blocks[b];
		auto table = materialization.TableName(b);
		auto dimensions = DimensionColumns();
		bool grouped = !dimensions.empty();
		auto signed_value = [](const string &column) {
			return "CASE WHEN __sv_sign > 0 THEN coalesce(" + column + ", 0) ELSE -coalesce(" + column + ", 0) END";
		};
		// the delta: per group, the signed sum of the parts' states
		vector<string> delta_columns = dimensions;
		delta_columns.push_back("sum(" + signed_value(MATERIALIZED_ROWS_COLUMN) + ") AS " + MATERIALIZED_ROWS_COLUMN);
		delta_columns.push_back("sum(CASE WHEN __sv_sign < 0 THEN __sv_rows ELSE 0 END) AS __sv_removed");
		bool may_recompute = false;
		for (auto &atom : block.atoms) {
			auto columns = atom.StateColumns();
			switch (atom.kind) {
			case SemanticAggregateKind::SUM:
			case SemanticAggregateKind::AVG:
			case SemanticAggregateKind::COUNT:
				for (auto &column : columns) {
					delta_columns.push_back("sum(" + signed_value(column) + ") AS " + column);
				}
				break;
			case SemanticAggregateKind::MIN:
				delta_columns.push_back("min(CASE WHEN __sv_sign > 0 THEN " + columns[0] + " END) AS " + columns[0]);
				may_recompute = true;
				break;
			case SemanticAggregateKind::MAX:
				delta_columns.push_back("max(CASE WHEN __sv_sign > 0 THEN " + columns[0] + " END) AS " + columns[0]);
				may_recompute = true;
				break;
			default:
				may_recompute = true;
				break;
			}
		}
		vector<string> unions;
		for (auto &part : parts) {
			unions.push_back("SELECT *, " + to_string(part.second) + " AS __sv_sign FROM " + part.first);
		}
		Execute("CREATE OR REPLACE TEMP TABLE __sv_delta AS SELECT " + Join(delta_columns, ", ") + " FROM (" +
		        Join(unions, " UNION ALL BY NAME ") + ")" + (grouped ? " GROUP BY " + Join(dimensions, ", ") : ""));
		// merged groups: stored state + delta
		vector<string> matches;
		for (auto &dimension : dimensions) {
			matches.push_back("__sv_m." + dimension + " IS NOT DISTINCT FROM __sv_d." + dimension);
		}
		auto match = matches.empty() ? string("true") : Join(matches, " AND ");
		auto plus = [](const string &column) {
			return "coalesce(__sv_m." + column + ", 0) + coalesce(__sv_d." + column + ", 0)";
		};
		vector<string> merged_columns;
		vector<string> stored_columns = dimensions;
		for (auto &dimension : dimensions) {
			merged_columns.push_back("__sv_d." + dimension);
		}
		merged_columns.push_back(plus(MATERIALIZED_ROWS_COLUMN) + " AS " + MATERIALIZED_ROWS_COLUMN);
		stored_columns.push_back(MATERIALIZED_ROWS_COLUMN);
		vector<string> recompute;
		for (auto &atom : block.atoms) {
			auto columns = atom.StateColumns();
			for (auto &column : columns) {
				stored_columns.push_back(column);
			}
			switch (atom.kind) {
			case SemanticAggregateKind::SUM:
			case SemanticAggregateKind::AVG:
				merged_columns.push_back("CASE WHEN " + plus(columns[1]) + " = 0 THEN NULL ELSE " + plus(columns[0]) +
				                         " END AS " + columns[0]);
				merged_columns.push_back(plus(columns[1]) + " AS " + columns[1]);
				break;
			case SemanticAggregateKind::COUNT:
				merged_columns.push_back(plus(columns[0]) + " AS " + columns[0]);
				break;
			case SemanticAggregateKind::MIN:
			case SemanticAggregateKind::MAX: {
				// a group that only gained rows keeps the least / greatest value; one that lost rows is recomputed
				auto merge = atom.kind == SemanticAggregateKind::MIN ? "least" : "greatest";
				merged_columns.push_back(string(merge) + "(__sv_m." + columns[0] + ", __sv_d." + columns[0] + ") AS " +
				                         columns[0]);
				recompute.push_back("coalesce(__sv_d.__sv_removed, 0) > 0");
				break;
			}
			default:
				merged_columns.push_back("CAST(NULL AS " + atom.type.ToString() + ") AS " + columns[0]);
				recompute.push_back("true");
				break;
			}
		}
		merged_columns.push_back((recompute.empty() ? string("false") : Join(recompute, " OR ")) +
		                         " AS __sv_recompute");
		Execute("CREATE OR REPLACE TEMP TABLE __sv_merged AS SELECT " + Join(merged_columns, ", ") +
		        " FROM __sv_delta AS __sv_d LEFT JOIN " + table + " AS __sv_m ON " + match);
		// replace the affected groups; groups without rows disappear (the only group of an ungrouped block stays)
		Execute("DELETE FROM " + table + " AS __sv_m WHERE EXISTS (SELECT 1 FROM __sv_delta AS __sv_d WHERE " + match +
		        ")");
		auto keep = string(grouped ? " AND " + string(MATERIALIZED_ROWS_COLUMN) + " > 0" : "");
		Execute("INSERT INTO " + table + " (" + Join(stored_columns, ", ") + ") SELECT " + Join(stored_columns, ", ") +
		        " FROM __sv_merged WHERE NOT __sv_recompute" + keep);
		if (may_recompute) {
			Execute("CREATE OR REPLACE TEMP TABLE __sv_recompute AS SELECT " +
			        (grouped ? Join(dimensions, ", ") : string("1 AS __sv_all")) +
			        " FROM __sv_merged WHERE __sv_recompute" + keep);
			if (Count("__sv_recompute") > 0) {
				RefreshSources now(model, tracked, current);
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
		return Count("__sv_delta");
	}

	ClientContext &context;
	const SemanticModel &model;
	const SemanticMaterialization &materialization;
	SemanticMaterializationState &state;
	SemanticChangeTracker tracker;
	Connection &connection;
	SemanticQueryCompiler compiler;
	map<idx_t, SemanticSourceTable> tracked;
	bool all_tracked = true;
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

void BuildSemanticMaterialization(ClientContext &context, const SemanticModel &model,
                                  const SemanticMaterialization &materialization) {
	{
		auto &state = *materialization.state;
		lock_guard<mutex> guard(state.lock);
		state.catalog = model.Name().catalog;
		state.schema = MATERIALIZATION_SCHEMA;
		state.tables.clear();
		for (idx_t b = 0; b < materialization.blocks.size(); b++) {
			state.tables.push_back(PhysicalName(model.Name().name) + "__" + PhysicalName(materialization.name) + "__" +
			                       to_string(b));
		}
	}
	MaterializationRefresh(context, model, materialization).Run(SemanticRefreshMode::FULL, true);
}

string RefreshSemanticMaterialization(ClientContext &context, const SemanticModel &model,
                                      const SemanticMaterialization &materialization, SemanticRefreshMode mode) {
	return MaterializationRefresh(context, model, materialization).Run(mode, false);
}

void DropSemanticMaterializationTables(ClientContext &context, const SemanticMaterialization &materialization) {
	auto &state = *materialization.state;
	vector<string> tables;
	{
		lock_guard<mutex> guard(state.lock);
		if (!state.built) {
			return;
		}
		for (idx_t b = 0; b < state.tables.size(); b++) {
			tables.push_back(materialization.TableName(b));
		}
		state.built = false;
	}
	Connection connection(*context.db);
	for (auto &table : tables) {
		auto result = connection.Query("DROP TABLE IF EXISTS " + table);
		if (result->HasError()) {
			result->ThrowError("semantic view materialization: ");
		}
	}
}

} // namespace duckdb
