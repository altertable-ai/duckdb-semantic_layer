#include "semantic_materialization_storage.hpp"

#include "duckdb/catalog/catalog.hpp"
#include "duckdb/catalog/entry_lookup_info.hpp"
#include "duckdb/main/attached_database.hpp"
#include "duckdb/main/database_manager.hpp"
#include "duckdb/common/exception/binder_exception.hpp"
#include "duckdb/common/string_util.hpp"
#include "semantic_definition_exception.hpp"
#include "semantic_lowering.hpp"

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

unique_ptr<MaterializedQueryResult> RunStorageSQL(Connection &connection, const string &sql) {
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

} // namespace

//! Writes the whole metadata row of a materialization (in the transaction that built it)
void WriteMetadata(Connection &connection, const SemanticModel &model, const SemanticMaterialization &materialization,
                   const SemanticMaterializationState &state, const map<string, idx_t> &watermark,
                   timestamp_t verified_at, const string &outcome, const string &detail) {
	auto table = MetadataTable(state.catalog);
	RunStorageSQL(connection, "CREATE SCHEMA IF NOT EXISTS " + QuoteIdentifier(state.catalog) + "." +
	                              QuoteIdentifier(SEMANTIC_MATERIALIZATION_SCHEMA));
	RunStorageSQL(connection, "CREATE TABLE IF NOT EXISTS " + table + " (" + METADATA_COLUMNS + ")");
	RunPrepared(connection, "DELETE FROM " + table + " WHERE " + METADATA_KEY,
	            MetadataKey(state.owner, materialization.name));
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
	auto values = MetadataKey(state.owner, materialization.name);
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
void UpdateMetadata(Connection &connection, const SemanticMaterialization &materialization,
                    const SemanticMaterializationState &state, const map<string, idx_t> &watermark,
                    timestamp_t verified_at, const string &outcome, const string &detail) {
	vector<Value> catalogs;
	vector<Value> snapshots;
	for (auto &entry : watermark) {
		catalogs.emplace_back(entry.first);
		snapshots.push_back(Value::UBIGINT(entry.second));
	}
	auto values = MetadataKey(state.owner, materialization.name);
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

//! Drops tables and deletes the metadata row of a materialization, in one transaction
static void DropStored(Connection &connection, const string &catalog, const vector<string> &tables,
                       const QualifiedName &view, const string &name) {
	for (auto &table : tables) {
		RunStorageSQL(connection, "DROP TABLE IF EXISTS " + table);
	}
	if (HasMetadata(*connection.context, catalog)) {
		RunPrepared(connection, "DELETE FROM " + MetadataTable(catalog) + " WHERE " + METADATA_KEY,
		            MetadataKey(view, name));
	}
}

void DropSemanticMaterializationTables(Connection &connection, const SemanticMaterialization &materialization) {
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
	}
	DropStored(connection, catalog, tables, state.owner, materialization.name);
}

void DropStoredMaterialization(Connection &connection, const SemanticStoredMaterialization &stored) {
	vector<string> tables;
	for (auto &table : stored.tables) {
		tables.push_back(QuoteIdentifier(stored.catalog) + "." + QuoteIdentifier(stored.schema) + "." +
		                 QuoteIdentifier(table));
	}
	DropStored(connection, stored.catalog, tables, stored.view, stored.name);
}

void RenameStoredMaterializations(Connection &connection, const SemanticModel &model,
                                  const QualifiedName &previous_name, const string &storage_catalog) {
	for (auto &materialization : model.materializations) {
		if (!StringUtil::CIEquals(materialization->storage_catalog, storage_catalog)) {
			continue;
		}
		string catalog;
		{
			lock_guard<mutex> guard(materialization->state->lock);
			if (materialization->state->tables.empty()) {
				continue;
			}
			catalog = materialization->state->catalog;
		}
		if (!HasMetadata(*connection.context, catalog)) {
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

vector<SemanticMaterializationRecord> ReadStoredMaterializations(Connection &connection, const string &catalog,
                                                                 const QualifiedName &view) {
	vector<SemanticMaterializationRecord> records;
	if (!HasMetadata(*connection.context, catalog)) {
		return records;
	}
	auto result = RunPrepared(connection,
	                          "SELECT name, definition, fingerprint, table_schema, tables, watermark_catalogs, "
	                          "watermark_snapshots, verified_at, last_refresh, last_refresh_detail FROM " +
	                              MetadataTable(catalog) +
	                              " WHERE view_catalog = $1 AND view_schema = $2 AND lower(view_name) = lower($3)",
	                          {Value(view.catalog), Value(view.schema), Value(view.name)});
	auto rows = unique_ptr_cast<QueryResult, MaterializedQueryResult>(std::move(result));
	for (idx_t row = 0; row < rows->RowCount(); row++) {
		SemanticMaterializationRecord record;
		auto &entry = record.location;
		entry.catalog = catalog;
		entry.schema = rows->GetValue(3, row).ToString();
		entry.view = view;
		entry.name = rows->GetValue(0, row).ToString();
		auto tables = rows->GetValue(4, row);
		for (auto &table : ListValue::GetChildren(tables)) {
			entry.tables.push_back(table.ToString());
		}
		record.definition = std::move(
		    SemanticLowering::Decode<ParsedSemanticMaterializationPayload>(rows->GetValue(1, row))->materialization);
		record.fingerprint = rows->GetValue(2, row).ToString();
		auto catalogs = ListValue::GetChildren(rows->GetValue(5, row));
		auto snapshots = ListValue::GetChildren(rows->GetValue(6, row));
		if (catalogs.size() != snapshots.size()) {
			throw InvalidInputException(
			    "Invalid materialization metadata: watermark catalogs and snapshots have different lengths");
		}
		for (idx_t i = 0; i < catalogs.size(); i++) {
			record.watermark[catalogs[i].ToString()] = snapshots[i].GetValue<idx_t>();
		}
		record.verified_at = rows->GetValue(7, row).GetValue<timestamp_t>();
		record.last_refresh = rows->GetValue(8, row).ToString();
		record.last_refresh_detail = rows->GetValue(9, row).ToString();
		records.push_back(std::move(record));
	}
	if (!records.empty()) {
		// Older allocations used sanitized names, which could already have collided before upgrading.
		// Reject ambiguous ownership without deleting either owner's recoverable data.
		auto conflicts = RunStorageSQL(connection, "SELECT lower(table_schema), lower(table_name) FROM (SELECT "
		                                           "table_schema, unnest(tables) AS table_name FROM " +
		                                               MetadataTable(catalog) + ") GROUP BY ALL HAVING count(*) > 1");
		set<pair<string, string>> ambiguous;
		for (idx_t row = 0; row < conflicts->RowCount(); row++) {
			ambiguous.emplace(conflicts->GetValue(0, row).ToString(), conflicts->GetValue(1, row).ToString());
		}
		for (auto &record : records) {
			for (auto &table : record.location.tables) {
				if (ambiguous.count({StringUtil::Lower(record.location.schema), StringUtil::Lower(table)})) {
					throw InvalidInputException(
					    "Invalid materialization metadata: multiple owners claim table %s.%s.%s", catalog,
					    record.location.schema, table);
				}
			}
		}
	}
	return records;
}

bool ReattachStoredMaterialization(SemanticMaterialization &materialization,
                                   const SemanticMaterializationRecord &record) {
	auto &entry = record.location;
	auto &state = *materialization.state;
	state.owner = entry.view;
	state.catalog = entry.catalog;
	state.schema = entry.schema;
	state.tables = entry.tables;
	state.built =
	    record.fingerprint == materialization.fingerprint && entry.tables.size() == materialization.blocks.size();
	if (state.built) {
		state.watermark = record.watermark;
		state.verified_at = record.verified_at;
		state.last_refresh = record.last_refresh;
		state.last_refresh_detail = record.last_refresh_detail;
	} else {
		state.last_refresh_detail = "its definition changed since it was built";
	}
	return state.built;
}

vector<SemanticStoredMaterialization> RestoreSemanticMaterializations(ClientContext &context, SemanticModel &model) {
	vector<SemanticStoredMaterialization> obsolete;
	Connection connection(*context.db);
	connection.BeginTransaction();
	for (auto &database : DatabaseManager::Get(context).GetDatabases(context)) {
		if (database->IsSystem() || database->IsTemporary()) {
			continue;
		}
		auto &catalog = database->GetCatalog();
		auto type = catalog.GetCatalogType();
		if (type != "duckdb" && type != "ducklake") {
			continue;
		}
		for (auto &record : ReadStoredMaterializations(connection, catalog.GetName(), model.Name())) {
			auto &entry = record.location;
			bool defined = false;
			for (auto &materialization : model.materializations) {
				defined = defined || StringUtil::CIEquals(materialization->name, entry.name);
			}
			if (defined) {
				continue;
			}
			auto &parsed = record.definition;
			if (!parsed.storage_catalog.empty() || !StringUtil::CIEquals(entry.catalog, model.Name().catalog)) {
				parsed.storage_catalog = entry.catalog;
			}
			shared_ptr<SemanticMaterialization> materialization;
			try {
				materialization = PlanSemanticMaterialization(context, model, parsed);
			} catch (SemanticDefinitionException &) {
				obsolete.push_back(std::move(entry));
				continue;
			}
			if (!StringUtil::CIEquals(materialization->storage_catalog, entry.catalog) ||
			    !StringUtil::CIEquals(materialization->storage_schema, entry.schema)) {
				obsolete.push_back(std::move(entry));
				continue;
			}
			ReattachStoredMaterialization(*materialization, record);
			model.definition->materializations.push_back(parsed.Copy());
			model.materializations.push_back(std::move(materialization));
		}
	}
	connection.Commit();
	return obsolete;
}

} // namespace duckdb
