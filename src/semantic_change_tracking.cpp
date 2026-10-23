#include "semantic_change_tracking.hpp"

#include "duckdb/catalog/catalog.hpp"
#include "duckdb/catalog/entry_lookup_info.hpp"
#include "duckdb/common/exception/binder_exception.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/common/to_string.hpp"
#include "duckdb/main/database.hpp"
#include "duckdb/parser/expression/constant_expression.hpp"
#include "duckdb/parser/expression/function_expression.hpp"
#include "duckdb/parser/keyword_helper.hpp"
#include "duckdb/parser/tableref/basetableref.hpp"
#include "duckdb/parser/tableref/table_function_ref.hpp"

namespace duckdb {

static constexpr const char *DUCKLAKE_CATALOG_TYPE = "ducklake";

SemanticSourceTable ClassifySemanticSource(ClientContext &context, const SemanticModel &model, idx_t table,
                                           string &reason) {
	auto &logical = model.tables[table];
	if (!logical.IsBaseTable()) {
		reason = "logical table \"" + logical.name + "\" is defined by a query";
		return SemanticSourceTable();
	}
	auto catalog = Catalog::GetCatalogEntry(context, Identifier(logical.catalog_name));
	if (!catalog || catalog->GetCatalogType() != DUCKLAKE_CATALOG_TYPE) {
		reason = "logical table \"" + logical.name + "\" reads " + logical.catalog_name + "." + logical.schema_name +
		         "." + logical.table_name + ", which is not a DuckLake table";
		return SemanticSourceTable();
	}
	auto entry = catalog->GetEntry(context, CatalogType::TABLE_ENTRY, Identifier(logical.schema_name),
	                               Identifier(logical.table_name), OnEntryNotFound::RETURN_NULL);
	if (!entry || entry->type != CatalogType::TABLE_ENTRY) {
		reason = "logical table \"" + logical.name + "\" reads a DuckLake view";
		return SemanticSourceTable();
	}
	SemanticSourceTable result;
	result.catalog = logical.catalog_name;
	result.schema = logical.schema_name;
	result.table = logical.table_name;
	return result;
}

SemanticChangeTracker::SemanticChangeTracker(ClientContext &context) : connection(*context.db) {
}

static string Literal(const string &value) {
	return SQLString::ToString(value);
}

unique_ptr<QueryResult> SemanticChangeTracker::Run(const string &sql) {
	auto result = connection.Query(sql);
	if (result->HasError()) {
		result->ThrowError("semantic view materialization: ");
	}
	return result;
}

SemanticCatalogChanges SemanticChangeTracker::CatalogChanges(const string &catalog, optional_idx since) {
	auto name = Literal(catalog);
	string sql =
	    "SELECT 'current' AS kind, NULL::VARCHAR AS value, NULL::VARCHAR AS extra, id::BIGINT AS snapshot FROM "
	    "ducklake_current_snapshot(" +
	    name + ")";
	if (since.IsValid()) {
		auto from = to_string(since.GetIndex());
		sql += " UNION ALL SELECT 'available', NULL, NULL, count(*) FROM ducklake_snapshots(" + name +
		       ") WHERE snapshot_id = " + from;
		sql += " UNION ALL SELECT 'table', table_name, schema_id::VARCHAR, table_id FROM ducklake_table_info(" + name +
		       ")";
		sql += " UNION ALL SELECT kind, unnest(ids), NULL, snapshot_id FROM (SELECT snapshot_id, "
		       "unnest(map_keys(changes)) "
		       "AS kind, unnest(map_values(changes)) AS ids FROM ducklake_snapshots(" +
		       name + ") WHERE snapshot_id > " + from + ")";
	}
	auto result = Run(sql);
	SemanticCatalogChanges changes;
	vector<pair<idx_t, pair<string, string>>> raw;
	for (idx_t row = 0; row < result->RowCount(); row++) {
		auto kind = result->Collection().GetValue(0, row).ToString();
		auto snapshot = result->Collection().GetValue(3, row);
		if (kind == "current") {
			changes.current = snapshot.GetValue<idx_t>();
		} else if (kind == "available") {
			changes.available = snapshot.GetValue<int64_t>() > 0;
		} else if (kind == "table") {
			changes.tables.push_back({result->Collection().GetValue(1, row).ToString(), snapshot.ToString(),
			                          result->Collection().GetValue(2, row).ToString()});
		} else if (!result->Collection().GetValue(1, row).IsNull()) {
			raw.emplace_back(snapshot.GetValue<idx_t>(),
			                 make_pair(kind, result->Collection().GetValue(1, row).ToString()));
		}
	}
	// snapshots committed after the current one was read belong to the next refresh
	for (auto &entry : raw) {
		if (entry.first <= changes.current) {
			changes.changes.push_back(std::move(entry.second));
		}
	}
	return changes;
}

static bool IsSchemaChange(const string &kind) {
	return StringUtil::StartsWith(kind, "tables_created") || StringUtil::StartsWith(kind, "tables_dropped") ||
	       StringUtil::StartsWith(kind, "tables_altered") || StringUtil::StartsWith(kind, "schemas_") ||
	       StringUtil::StartsWith(kind, "views_") || StringUtil::Contains(kind, "macros_");
}

SemanticTableChanges SemanticCatalogChanges::RowChanges(const SemanticSourceTable &table) const {
	// tables of the same name in other schemas are counted too (the table info has no schema names): conservative
	case_insensitive_set_t ids;
	for (auto &entry : tables) {
		if (StringUtil::CIEquals(entry.name, table.table)) {
			ids.insert(entry.id);
		}
	}
	SemanticTableChanges result;
	for (auto &change : changes) {
		if (IsSchemaChange(change.first) || !ids.count(change.second)) {
			continue;
		}
		if (change.first == "tables_inserted_into" || change.first == "inlined_insert") {
			result.inserted = true;
		} else if (change.first == "tables_deleted_from" || change.first == "inlined_delete") {
			result.deleted = true;
		} else {
			// any other change of the table's data: assume rows were both inserted and deleted
			result.inserted = true;
			result.deleted = true;
		}
	}
	return result;
}

bool SemanticCatalogChanges::SchemaChanged(const SemanticSourceTable &table) const {
	case_insensitive_set_t ids;
	case_insensitive_set_t schema_ids;
	for (auto &entry : tables) {
		if (StringUtil::CIEquals(entry.name, table.table)) {
			ids.insert(entry.id);
			schema_ids.insert(entry.schema_id);
		}
	}
	if (ids.empty()) {
		return true;
	}
	auto qualified = table.schema + "." + table.table;
	for (auto &change : changes) {
		auto &kind = change.first;
		if (((kind == "tables_altered" || kind == "tables_dropped") && ids.count(change.second)) ||
		    (kind == "tables_created" && StringUtil::CIEquals(change.second, qualified)) ||
		    (kind == "schemas_dropped" && schema_ids.count(change.second))) {
			return true;
		}
	}
	return false;
}

string SemanticChangeTracker::ChangesSQL(const SemanticSourceTable &table, idx_t from, idx_t to,
                                         const SemanticTableChanges &changes) {
	auto arguments = Literal(table.catalog) + ", " + Literal(table.schema) + ", " + Literal(table.table) + ", " +
	                 to_string(from + 1) + ", " + to_string(to);
	vector<string> parts;
	if (changes.inserted) {
		parts.push_back("SELECT rowid, snapshot_id, 'insert' AS change_type, * FROM ducklake_table_insertions(" +
		                arguments + ")");
	}
	if (changes.deleted) {
		parts.push_back("SELECT rowid, snapshot_id, 'delete' AS change_type, * FROM ducklake_table_deletions(" +
		                arguments + ")");
	}
	if (parts.empty()) {
		parts.push_back("SELECT rowid, snapshot_id, 'insert' AS change_type, * FROM ducklake_table_insertions(" +
		                arguments + ") LIMIT 0");
	}
	return StringUtil::Join(parts, " UNION ALL BY NAME ");
}

unique_ptr<TableRef> SemanticChangeTracker::AtSnapshot(const SemanticSourceTable &table, idx_t snapshot) {
	auto ref = make_uniq<BaseTableRef>();
	ref->SetQualifiedName(Identifier(table.catalog), Identifier(table.schema), Identifier(table.table));
	ref->at_clause = make_uniq<AtClause>("VERSION", ConstantExpression::FromValue(Value::UBIGINT(snapshot)));
	return std::move(ref);
}

} // namespace duckdb
