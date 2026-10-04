#include "semantic_change_tracking.hpp"

#include "duckdb/catalog/catalog.hpp"
#include "duckdb/catalog/entry_lookup_info.hpp"
#include "duckdb/common/exception/binder_exception.hpp"
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
	auto catalog = Catalog::GetCatalogEntry(context, logical.catalog_name);
	if (!catalog || catalog->GetCatalogType() != DUCKLAKE_CATALOG_TYPE) {
		reason = "logical table \"" + logical.name + "\" reads " + logical.catalog_name + "." + logical.schema_name +
		         "." + logical.table_name + ", which is not a DuckLake table";
		return SemanticSourceTable();
	}
	EntryLookupInfo lookup(CatalogType::TABLE_ENTRY, logical.table_name);
	auto entry = catalog->GetEntry(context, logical.schema_name, lookup, OnEntryNotFound::RETURN_NULL);
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
	return KeywordHelper::WriteQuoted(value, '\'');
}

unique_ptr<MaterializedQueryResult> SemanticChangeTracker::Run(const string &sql) {
	auto result = connection.Query(sql);
	if (result->HasError()) {
		result->ThrowError("semantic view materialization: ");
	}
	return result;
}

idx_t SemanticChangeTracker::CurrentSnapshot(const string &catalog) {
	auto result = Run("SELECT id FROM ducklake_current_snapshot(" + Literal(catalog) + ")");
	return result->GetValue(0, 0).GetValue<idx_t>();
}

bool SemanticChangeTracker::SnapshotAvailable(const string &catalog, idx_t snapshot) {
	auto result = Run("SELECT count(*) FROM ducklake_snapshots(" + Literal(catalog) +
	                  ") WHERE snapshot_id = " + to_string(snapshot));
	return result->GetValue(0, 0).GetValue<int64_t>() > 0;
}

bool SemanticChangeTracker::SchemaChanged(const SemanticSourceTable &table, idx_t from, idx_t to) {
	// conservative: any schema change of the catalog in (from, to] counts (DuckLake bumps schema_version on DDL)
	auto result = Run("SELECT count(DISTINCT schema_version) FROM ducklake_snapshots(" + Literal(table.catalog) +
	                  ") WHERE snapshot_id IN (" + to_string(from) + ", " + to_string(to) + ")");
	return result->GetValue(0, 0).GetValue<int64_t>() != 1;
}

bool SemanticChangeTracker::RowsChanged(const SemanticSourceTable &table, idx_t from, idx_t to) {
	auto result = Run("SELECT count(*) FROM (SELECT 1 FROM ducklake_table_changes(" + Literal(table.catalog) + ", " +
	                  Literal(table.schema) + ", " + Literal(table.table) + ", " + to_string(from + 1) + ", " +
	                  to_string(to) + ") LIMIT 1)");
	return result->GetValue(0, 0).GetValue<int64_t>() > 0;
}

unique_ptr<TableRef> SemanticChangeTracker::ChangesRef(const SemanticSourceTable &table, idx_t from, idx_t to) {
	vector<unique_ptr<ParsedExpression>> arguments;
	arguments.push_back(make_uniq<ConstantExpression>(Value(table.catalog)));
	arguments.push_back(make_uniq<ConstantExpression>(Value(table.schema)));
	arguments.push_back(make_uniq<ConstantExpression>(Value(table.table)));
	arguments.push_back(make_uniq<ConstantExpression>(Value::UBIGINT(from + 1)));
	arguments.push_back(make_uniq<ConstantExpression>(Value::UBIGINT(to)));
	auto ref = make_uniq<TableFunctionRef>();
	ref->function = make_uniq<FunctionExpression>("ducklake_table_changes", std::move(arguments));
	return std::move(ref);
}

unique_ptr<TableRef> SemanticChangeTracker::AtSnapshot(const SemanticSourceTable &table, idx_t snapshot) {
	auto ref = make_uniq<BaseTableRef>();
	ref->catalog_name = table.catalog;
	ref->schema_name = table.schema;
	ref->table_name = table.table;
	ref->at_clause = make_uniq<AtClause>("VERSION", make_uniq<ConstantExpression>(Value::UBIGINT(snapshot)));
	return std::move(ref);
}

} // namespace duckdb
