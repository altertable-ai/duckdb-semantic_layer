#include "duckdb/catalog/catalog.hpp"
#include "duckdb/catalog/entry_lookup_info.hpp"
#include "duckdb/parser/expression/star_expression.hpp"
#include "duckdb/parser/query_node/set_operation_node.hpp"
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
#include "semantic_lowering.hpp"
#include "duckdb/main/attached_database.hpp"
#include "duckdb/main/database_manager.hpp"
#include "semantic_materialization_internal.hpp"

namespace duckdb {

namespace {

//===--------------------------------------------------------------------===//
// Metadata: one row per materialization, in every database holding materialization tables
//===--------------------------------------------------------------------===//
string MetadataTable(const string &catalog) {
	return QuoteIdentifier(catalog) + "." + QuoteIdentifier(SEMANTIC_MATERIALIZATION_SCHEMA) + "." +
	       QuoteIdentifier(SEMANTIC_MATERIALIZATION_METADATA);
}

constexpr const char *METADATA_COLUMNS =
    "view_catalog VARCHAR, view_schema VARCHAR, view_name VARCHAR, name VARCHAR, definition BLOB, fingerprint VARCHAR, "
    "table_schema VARCHAR, tables VARCHAR[], watermark_catalogs VARCHAR[], watermark_snapshots UBIGINT[], "
    "verified_at TIMESTAMP, last_refresh VARCHAR, last_refresh_detail VARCHAR";
constexpr const char *METADATA_KEY =
    "view_catalog = $1 AND view_schema = $2 AND lower(view_name) = lower($3) AND lower(name) = lower($4)";

unique_ptr<MaterializedQueryResult> Run(Connection &connection, const string &sql) {
	auto result = connection.Query(sql);
	if (result->HasError()) {
		result->ThrowError("semantic view materialization: ");
	}
	return result;
}

unique_ptr<QueryResult> RunPrepared(Connection &connection, const string &sql, vector<Value> values) {
	auto prepared = connection.Prepare(sql);
	if (prepared->HasError()) {
		throw BinderException("semantic view materialization: %s", prepared->GetError());
	}
	auto result = prepared->Execute(values, false);
	if (result->HasError()) {
		result->ThrowError("semantic view materialization: ");
	}
	return result;
}

Value StringList(const vector<string> &values) {
	vector<Value> list;
	for (auto &value : values) {
		list.emplace_back(value);
	}
	return Value::LIST(LogicalType::VARCHAR, std::move(list));
}

vector<Value> MetadataKey(const QualifiedName &view, const string &name) {
	return {Value(view.catalog), Value(view.schema), Value(view.name), Value(name)};
}

//! Writes the whole metadata row of a materialization (in the transaction that built it)
void WriteMetadata(Connection &connection, const SemanticModel &model, const SemanticMaterialization &materialization,
                   const SemanticMaterializationState &state, const map<string, idx_t> &watermark,
                   timestamp_t verified_at, const string &outcome, const string &detail) {
	auto table = MetadataTable(state.catalog);
	Run(connection, "CREATE SCHEMA IF NOT EXISTS " + QuoteIdentifier(state.catalog) + "." +
	                    QuoteIdentifier(SEMANTIC_MATERIALIZATION_SCHEMA));
	Run(connection, "CREATE TABLE IF NOT EXISTS " + table + " (" + METADATA_COLUMNS + ")");
	RunPrepared(connection, "DELETE FROM " + table + " WHERE " + METADATA_KEY,
	            MetadataKey(model.Name(), materialization.name));
	ParsedSemanticMaterializationPayload payload;
	for (auto &parsed : model.definition->materializations) {
		if (StringUtil::CIEquals(parsed.name, materialization.name)) {
			payload.materialization = parsed.Copy();
		}
	}
	vector<Value> catalogs;
	vector<Value> snapshots;
	for (auto &entry : watermark) {
		catalogs.emplace_back(entry.first);
		snapshots.push_back(Value::UBIGINT(entry.second));
	}
	auto values = MetadataKey(model.Name(), materialization.name);
	values.push_back(SemanticLowering::Encode(payload));
	values.emplace_back(materialization.fingerprint);
	values.emplace_back(state.schema);
	values.push_back(StringList(state.tables));
	values.push_back(Value::LIST(LogicalType::VARCHAR, std::move(catalogs)));
	values.push_back(Value::LIST(LogicalType::UBIGINT, std::move(snapshots)));
	values.push_back(Value::TIMESTAMP(verified_at));
	values.emplace_back(outcome);
	values.emplace_back(detail);
	RunPrepared(connection, "INSERT INTO " + table + " VALUES ($1, $2, $3, $4, $5, $6, $7, $8, $9, $10, $11, $12, $13)",
	            values);
}

//! Updates the watermark and refresh outcome of a materialization's metadata row (after an incremental refresh)
void UpdateMetadata(Connection &connection, const SemanticModel &model, const SemanticMaterialization &materialization,
                    const SemanticMaterializationState &state, const map<string, idx_t> &watermark,
                    timestamp_t verified_at, const string &outcome, const string &detail) {
	vector<Value> catalogs;
	vector<Value> snapshots;
	for (auto &entry : watermark) {
		catalogs.emplace_back(entry.first);
		snapshots.push_back(Value::UBIGINT(entry.second));
	}
	auto values = MetadataKey(model.Name(), materialization.name);
	values.push_back(Value::LIST(LogicalType::VARCHAR, std::move(catalogs)));
	values.push_back(Value::LIST(LogicalType::UBIGINT, std::move(snapshots)));
	values.push_back(Value::TIMESTAMP(verified_at));
	values.emplace_back(outcome);
	values.emplace_back(detail);
	RunPrepared(connection,
	            "UPDATE " + MetadataTable(state.catalog) +
	                " SET watermark_catalogs = $5, watermark_snapshots = $6, verified_at = $7, last_refresh = $8, "
	                "last_refresh_detail = $9 WHERE " +
	                METADATA_KEY,
	            values);
}

bool HasMetadata(ClientContext &context, const string &catalog_name) {
	auto catalog = Catalog::GetCatalogEntry(context, catalog_name);
	if (!catalog || !catalog->GetSchema(context, SEMANTIC_MATERIALIZATION_SCHEMA, OnEntryNotFound::RETURN_NULL)) {
		return false;
	}
	// the lookup keeps a reference to the name
	const string table = SEMANTIC_MATERIALIZATION_METADATA;
	EntryLookupInfo lookup(CatalogType::TABLE_ENTRY, table);
	return catalog->GetEntry(context, SEMANTIC_MATERIALIZATION_SCHEMA, lookup, OnEntryNotFound::RETURN_NULL) != nullptr;
}

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
			// e.g. restored with another definition than it was built with
			unbuilt_reason = state.built ? string() : state.last_refresh_detail;
		}
		// what happened in every source catalog since the last refresh (one metadata query each)
		map<string, SemanticCatalogChanges> changes;
		map<string, idx_t> current;
		if (all_tracked) {
			for (auto &entry : tracked) {
				auto &catalog = entry.second.catalog;
				if (changes.count(catalog)) {
					continue;
				}
				optional_idx since;
				auto previous = watermark.find(catalog);
				if (built && previous != watermark.end()) {
					since = previous->second;
				}
				changes[catalog] = tracker.CatalogChanges(catalog, since);
				current[catalog] = changes[catalog].current;
			}
		}
		string outcome;
		string detail;
		auto now = Timestamp::GetCurrentTimestamp();
		connection.BeginTransaction();
		try {
			auto why_full = initial ? string("initial build") : FullRefreshReason(mode, built, watermark, changes);
			if (!initial && mode == SemanticRefreshMode::INCREMENTAL && !why_full.empty()) {
				throw BinderException("Materialization \"%s\" cannot be refreshed incrementally: %s",
				                      materialization.name, why_full);
			}
			if (!why_full.empty()) {
				FullBuild(current);
				outcome = "FULL";
				detail = why_full;
			} else {
				string rebuilt;
				auto groups = IncrementalRefresh(mode, watermark, current, changes, rebuilt);
				outcome = groups.IsValid() ? "INCREMENTAL" : "NONE";
				detail = groups.IsValid() ? to_string(groups.GetIndex()) + " group(s) changed" : "no source changes";
				if (!rebuilt.empty()) {
					outcome = groups.IsValid() ? "INCREMENTAL" : "FULL";
					detail = groups.IsValid() ? detail + "; " + rebuilt : rebuilt;
				}
			}
			// the metadata changes with the tables (a refresh without changes keeps the previous watermark: still
			// correct, nothing to write)
			auto watermark_after = all_tracked ? current : map<string, idx_t>();
			if (!why_full.empty()) {
				WriteMetadata(connection, model, materialization, state, watermark_after, now, outcome, detail);
			} else if (outcome != "NONE") {
				UpdateMetadata(connection, model, materialization, state, watermark_after, now, outcome, detail);
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
			state.verified_at = now;
			state.last_refresh = outcome;
			state.last_refresh_detail = detail;
		}
		if (outcome == "NONE") {
			return "Materialization " + materialization.name + " is up to date.";
		}
		return "Materialization " + materialization.name + " refreshed (" + outcome + ": " + detail + ").";
	}

private:
	//! Why the refresh must rebuild everything; empty when it can be incremental (runs in the refresh's transaction)
	string FullRefreshReason(SemanticRefreshMode mode, bool built, const map<string, idx_t> &watermark,
	                         const map<string, SemanticCatalogChanges> &changes) {
		if (mode == SemanticRefreshMode::FULL) {
			return "FULL refresh requested";
		}
		if (!materialization.incremental) {
			return materialization.incremental_reason;
		}
		if (!built && !unbuilt_reason.empty()) {
			return unbuilt_reason;
		}
		if (!built || watermark.empty()) {
			return "it was not built from tracked snapshots";
		}
		for (auto &entry : changes) {
			auto previous = watermark.find(entry.first);
			if (previous == watermark.end()) {
				return "it was not built from tracked snapshots";
			}
			if (previous->second == entry.second.current) {
				continue;
			}
			if (!entry.second.available) {
				return "the changes since snapshot " + to_string(previous->second) + " of " + entry.first +
				       " are no longer available";
			}
			for (auto &source : tracked) {
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
	                                const map<string, idx_t> &current,
	                                const map<string, SemanticCatalogChanges> &changes, string &rebuilt) {
		optional_idx changed_groups;
		for (idx_t b = 0; b < materialization.blocks.size(); b++) {
			set<idx_t> changed;
			for (auto table : materialization.blocks[b].tables) {
				auto &source = tracked.at(table);
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
			RefreshSources new_rows(model, tracked, current);
			new_rows.replacements[block.home] = ChangedRowImages(home_changes, true);
			AddPart(b, new_rows, nullptr, 1, parts);
			RefreshSources old_rows(model, tracked, watermark);
			old_rows.replacements[block.home] = ChangedRowImages(home_changes, false);
			AddPart(b, old_rows, nullptr, -1, parts);
		}
		// 2. unchanged home rows that reach a changed row of another table: + as of now, - as of before
		map<idx_t, string> referenced;
		for (auto table : changed) {
			if (table != block.home) {
				referenced[table] = Changes(table, watermark, current);
			}
		}
		if (!referenced.empty()) {
			auto filter = [&](SemanticJoinScope &scope, const vector<unique_ptr<ParsedExpression>> &) {
				unique_ptr<ParsedExpression> result;
				for (auto &entry : referenced) {
					auto path = model.Path(block.home, entry.first, block.using_relationships);
					auto &edge = path.back();
					auto &rel = model.relationships[edge.relationship];
					auto parent = path.size() >= 2 ? path[path.size() - 2].target : block.home;
					auto &parent_columns = edge.reversed ? rel.to_columns : rel.from_columns;
					auto &keys = edge.reversed ? rel.from_columns : rel.to_columns;
					vector<unique_ptr<ParsedExpression>> values;
					for (auto &column : parent_columns) {
						values.push_back(scope.ColumnReference(parent, column));
					}
					auto reaching = ExistsIn(entry.second, keys, std::move(values), false);
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
			RefreshSources now(model, tracked, current);
			AddPart(b, now, filter, 1, parts);
			RefreshSources before(model, tracked, watermark);
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
			case SemanticAggregateKind::MAX: {
				auto function = atom.kind == SemanticAggregateKind::MIN ? "min" : "max";
				delta_columns.push_back(string(function) + "(CASE WHEN __sv_sign > 0 THEN " + columns[0] + " END) AS " +
				                        columns[0]);
				extremes = true;
				break;
			}
			default:
				opaque = true;
				break;
			}
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
		for (auto &atom : block.atoms) {
			auto columns = atom.StateColumns();
			for (auto &column : columns) {
				stored_columns.push_back(column);
			}
			switch (atom.kind) {
			case SemanticAggregateKind::SUM:
			case SemanticAggregateKind::AVG:
				updates.push_back(columns[0] + " = CASE WHEN " + plus(columns[1]) + " = 0 THEN NULL ELSE " +
				                  plus(columns[0]) + " END");
				updates.push_back(columns[1] + " = " + plus(columns[1]));
				inserted.push_back("CASE WHEN coalesce(__sv_d." + columns[1] + ", 0) = 0 THEN NULL ELSE __sv_d." +
				                   columns[0] + " END");
				inserted.push_back("coalesce(__sv_d." + columns[1] + ", 0)");
				break;
			case SemanticAggregateKind::COUNT:
				updates.push_back(columns[0] + " = " + plus(columns[0]));
				inserted.push_back("coalesce(__sv_d." + columns[0] + ", 0)");
				break;
			case SemanticAggregateKind::MIN:
			case SemanticAggregateKind::MAX: {
				// a group that only gained rows keeps the least / greatest value; one that lost rows is recomputed
				auto merge = atom.kind == SemanticAggregateKind::MIN ? "least" : "greatest";
				updates.push_back(columns[0] + " = " + merge + "(__sv_m." + columns[0] + ", __sv_d." + columns[0] +
				                  ")");
				inserted.push_back("__sv_d." + columns[0]);
				break;
			}
			default:
				inserted.push_back("CAST(NULL AS " + atom.type.ToString() + ")");
				break;
			}
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
		return changed_groups;
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
	//! Why the materialization is not built, when known
	string unbuilt_reason;
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
	auto &state = *materialization.state;
	{
		lock_guard<mutex> guard(state.lock);
		state.catalog = materialization.storage_catalog;
		state.schema = materialization.storage_schema;
		state.tables.clear();
		for (idx_t b = 0; b < materialization.blocks.size(); b++) {
			state.tables.push_back(PhysicalName(model.Name().name) + "__" + PhysicalName(materialization.name) + "__" +
			                       to_string(b));
		}
	}
	// stored tables with the same definition (e.g. after a restart) are reattached instead of rebuilt
	if (HasMetadata(context, state.catalog)) {
		Connection connection(*context.db);
		auto stored = RunPrepared(connection,
		                          "SELECT fingerprint, table_schema, tables, watermark_catalogs, watermark_snapshots, "
		                          "verified_at, last_refresh, last_refresh_detail FROM " +
		                              MetadataTable(state.catalog) + " WHERE " + METADATA_KEY,
		                          MetadataKey(model.Name(), materialization.name));
		auto rows = unique_ptr_cast<QueryResult, MaterializedQueryResult>(std::move(stored));
		if (rows->RowCount() == 1 && rows->GetValue(0, 0).ToString() == materialization.fingerprint &&
		    StringUtil::CIEquals(rows->GetValue(1, 0).ToString(), state.schema)) {
			bool tables_exist = true;
			vector<string> tables;
			auto stored_tables = rows->GetValue(2, 0);
			for (auto &table : ListValue::GetChildren(stored_tables)) {
				tables.push_back(table.ToString());
				EntryLookupInfo lookup(CatalogType::TABLE_ENTRY, tables.back());
				tables_exist = tables_exist && Catalog::GetEntry(context, state.catalog, state.schema, lookup,
				                                                 OnEntryNotFound::RETURN_NULL);
			}
			if (tables_exist && tables.size() == materialization.blocks.size()) {
				lock_guard<mutex> guard(state.lock);
				state.tables = tables;
				state.watermark.clear();
				auto catalogs = ListValue::GetChildren(rows->GetValue(3, 0));
				auto snapshots = ListValue::GetChildren(rows->GetValue(4, 0));
				for (idx_t i = 0; i < catalogs.size(); i++) {
					state.watermark[catalogs[i].ToString()] = snapshots[i].GetValue<idx_t>();
				}
				state.verified_at = rows->GetValue(5, 0).GetValue<timestamp_t>();
				state.last_refresh = rows->GetValue(6, 0).ToString();
				state.last_refresh_detail = rows->GetValue(7, 0).ToString();
				state.built = true;
				return;
			}
		}
	}
	MaterializationRefresh(context, model, materialization).Run(SemanticRefreshMode::FULL, true);
}

string RefreshSemanticMaterialization(ClientContext &context, const SemanticModel &model,
                                      const SemanticMaterialization &materialization, SemanticRefreshMode mode) {
	return MaterializationRefresh(context, model, materialization).Run(mode, false);
}

//! Drops tables and deletes the metadata row of a materialization, in one transaction
static void DropStored(ClientContext &context, const string &catalog, const vector<string> &tables,
                       const QualifiedName &view, const string &name) {
	Connection connection(*context.db);
	connection.BeginTransaction();
	try {
		for (auto &table : tables) {
			Run(connection, "DROP TABLE IF EXISTS " + table);
		}
		if (HasMetadata(context, catalog)) {
			RunPrepared(connection, "DELETE FROM " + MetadataTable(catalog) + " WHERE " + METADATA_KEY,
			            MetadataKey(view, name));
		}
		connection.Commit();
	} catch (std::exception &) {
		if (connection.HasActiveTransaction()) {
			connection.Rollback();
		}
		throw;
	}
}

void DropSemanticMaterializationTables(ClientContext &context, const SemanticModel &model,
                                       const SemanticMaterialization &materialization) {
	auto &state = *materialization.state;
	vector<string> tables;
	string catalog;
	{
		lock_guard<mutex> guard(state.lock);
		if (!state.built && state.tables.empty()) {
			return;
		}
		for (idx_t b = 0; b < state.tables.size(); b++) {
			tables.push_back(materialization.TableName(b));
		}
		catalog = state.catalog;
		state.built = false;
	}
	DropStored(context, catalog, tables, model.Name(), materialization.name);
}

void DropStoredMaterialization(ClientContext &context, const SemanticStoredMaterialization &stored) {
	vector<string> tables;
	for (auto &table : stored.tables) {
		tables.push_back(QuoteIdentifier(stored.catalog) + "." + QuoteIdentifier(stored.schema) + "." +
		                 QuoteIdentifier(table));
	}
	DropStored(context, stored.catalog, tables, stored.view, stored.name);
}

void RenameStoredMaterializations(ClientContext &context, const SemanticModel &model,
                                  const QualifiedName &previous_name) {
	Connection connection(*context.db);
	for (auto &materialization : model.materializations) {
		string catalog;
		{
			lock_guard<mutex> guard(materialization->state->lock);
			if (!materialization->state->built) {
				continue;
			}
			catalog = materialization->state->catalog;
		}
		if (!HasMetadata(context, catalog)) {
			continue;
		}
		auto values = MetadataKey(previous_name, materialization->name);
		values.emplace_back(model.Name().catalog);
		values.emplace_back(model.Name().schema);
		values.emplace_back(model.Name().name);
		RunPrepared(connection,
		            "UPDATE " + MetadataTable(catalog) +
		                " SET view_catalog = $5, view_schema = $6, view_name = $7 WHERE " + METADATA_KEY,
		            values);
	}
}

vector<SemanticStoredMaterialization> RestoreSemanticMaterializations(ClientContext &context, SemanticModel &model) {
	vector<SemanticStoredMaterialization> obsolete;
	Connection connection(*context.db);
	for (auto &database : DatabaseManager::Get(context).GetDatabases(context)) {
		if (database->IsSystem() || database->IsTemporary()) {
			continue;
		}
		auto &catalog = database->GetCatalog();
		auto type = catalog.GetCatalogType();
		if ((type != "duckdb" && type != "ducklake") || !HasMetadata(context, catalog.GetName())) {
			continue;
		}
		auto stored = RunPrepared(connection,
		                          "SELECT name, definition, fingerprint, table_schema, tables, watermark_catalogs, "
		                          "watermark_snapshots, verified_at, last_refresh, last_refresh_detail FROM " +
		                              MetadataTable(catalog.GetName()) +
		                              " WHERE view_catalog = $1 AND view_schema = $2 AND lower(view_name) = lower($3)",
		                          {Value(model.Name().catalog), Value(model.Name().schema), Value(model.Name().name)});
		auto rows = unique_ptr_cast<QueryResult, MaterializedQueryResult>(std::move(stored));
		for (idx_t row = 0; row < rows->RowCount(); row++) {
			SemanticStoredMaterialization entry;
			entry.catalog = catalog.GetName();
			entry.schema = rows->GetValue(3, row).ToString();
			auto stored_tables = rows->GetValue(4, row);
			for (auto &table : ListValue::GetChildren(stored_tables)) {
				entry.tables.push_back(table.ToString());
			}
			entry.view = model.Name();
			entry.name = rows->GetValue(0, row).ToString();
			bool defined = false;
			for (auto &materialization : model.materializations) {
				defined = defined || StringUtil::CIEquals(materialization->name, entry.name);
			}
			if (defined) {
				continue;
			}
			auto payload = SemanticLowering::Decode<ParsedSemanticMaterializationPayload>(rows->GetValue(1, row));
			// stored where its metadata is, whatever that database is called now (it may be attached under another
			// name)
			auto &parsed = payload->materialization;
			if (!parsed.storage_catalog.empty() || !StringUtil::CIEquals(entry.catalog, model.Name().catalog)) {
				parsed.storage_catalog = entry.catalog;
			}
			shared_ptr<SemanticMaterialization> materialization;
			try {
				materialization = PlanSemanticMaterialization(context, model, parsed);
			} catch (std::exception &) {
				// the view no longer has what it materializes
				obsolete.push_back(std::move(entry));
				continue;
			}
			if (!StringUtil::CIEquals(materialization->storage_catalog, entry.catalog) ||
			    !StringUtil::CIEquals(materialization->storage_schema, entry.schema)) {
				obsolete.push_back(std::move(entry));
				continue;
			}
			auto &state = *materialization->state;
			state.catalog = entry.catalog;
			state.schema = entry.schema;
			state.tables = entry.tables;
			if (rows->GetValue(2, row).ToString() == materialization->fingerprint &&
			    entry.tables.size() == materialization->blocks.size()) {
				auto catalogs = ListValue::GetChildren(rows->GetValue(5, row));
				auto snapshots = ListValue::GetChildren(rows->GetValue(6, row));
				for (idx_t i = 0; i < catalogs.size(); i++) {
					state.watermark[catalogs[i].ToString()] = snapshots[i].GetValue<idx_t>();
				}
				state.verified_at = rows->GetValue(7, row).GetValue<timestamp_t>();
				state.last_refresh = rows->GetValue(8, row).ToString();
				state.last_refresh_detail = rows->GetValue(9, row).ToString();
				state.built = true;
			} else {
				// stored with another definition: the next refresh rebuilds it
				state.last_refresh_detail = "its definition changed since it was built";
			}
			model.definition->materializations.push_back(parsed.Copy());
			model.materializations.push_back(std::move(materialization));
		}
	}
	return obsolete;
}

} // namespace duckdb
