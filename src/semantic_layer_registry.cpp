#include "semantic_layer_registry.hpp"

#include "duckdb/catalog/catalog.hpp"
#include "duckdb/catalog/catalog_search_path.hpp"
#include "duckdb/common/exception.hpp"
#include "duckdb/common/exception/binder_exception.hpp"
#include "duckdb/common/exception/catalog_exception.hpp"
#include "duckdb/common/to_string.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/main/client_context.hpp"
#include "duckdb/main/client_data.hpp"
#include "duckdb/main/database_manager.hpp"
#include "duckdb/planner/binder.hpp"

namespace duckdb {

static bool IsSet(const string &part, const string &invalid) {
	return !part.empty() && part != invalid;
}

//! The schema a name qualified only by `catalog` lives in
static Identifier DefaultSchema(ClientContext &context, CatalogSearchPath &search_path, const Identifier &catalog) {
	auto schema = search_path.GetDefaultSchema(context, catalog);
	return schema ? *schema : Identifier(DEFAULT_SCHEMA);
}

SemanticQualifiedName SemanticViewRegistry::ResolveSchema(ClientContext &context, const string &catalog_p,
                                                          const string &schema_p, OnEntryNotFound if_not_found) {
	Identifier catalog(IsSet(catalog_p, INVALID_CATALOG) ? catalog_p : string());
	Identifier schema(IsSet(schema_p, INVALID_SCHEMA) ? schema_p : string());
	if (catalog.empty() && !schema.empty()) {
		// `x.view`: x may be a schema of the default database or a database name
		Binder::BindSchemaOrCatalog(context, catalog, schema);
	}
	auto &search_path = *ClientData::Get(context).catalog_search_path;
	if (catalog.empty() && schema.empty()) {
		auto &entry = search_path.GetDefault();
		catalog = entry.GetCatalog();
		schema = entry.GetSchema();
	}
	if (catalog.empty()) {
		catalog = DatabaseManager::GetDefaultDatabase(context);
	}
	if (schema.empty()) {
		schema = DefaultSchema(context, search_path, catalog);
	}
	// validates that the database and schema exist (and normalizes the spelling
	// of catalog / schema)
	Catalog::GetCatalog(context, catalog);
	auto schema_entry = Catalog::GetSchema(context, catalog, schema, if_not_found);
	SemanticQualifiedName result;
	result.catalog = (schema_entry ? schema_entry->ParentCatalog().GetName() : catalog).GetIdentifierName();
	result.schema = (schema_entry ? schema_entry->name : schema).GetIdentifierName();
	return result;
}

SemanticQualifiedName SemanticViewRegistry::ResolveDDLName(ClientContext &context, const SemanticQualifiedName &name,
                                                           OnEntryNotFound if_not_found) {
	auto result = ResolveSchema(context, name.catalog, name.schema, if_not_found);
	result.name = name.name;
	return result;
}

string SemanticViewRegistry::Key(const SemanticQualifiedName &name) {
	return StringUtil::Lower(name.catalog) + "\x1f" + StringUtil::Lower(name.schema) + "\x1f" +
	       StringUtil::Lower(name.name);
}

bool SemanticViewRegistry::Create(shared_ptr<SemanticModel> model, bool replace,
                                  const std::function<void(shared_ptr<SemanticModel>)> &prepare) {
	lock_guard<mutex> writing(ddl_lock);
	auto key = Key(model->Name());
	shared_ptr<SemanticModel> previous;
	{
		lock_guard<mutex> guard(lock);
		auto current = views.find(key);
		if (current != views.end()) {
			if (!replace) {
				return false;
			}
			previous = current->second;
		}
	}
	if (prepare) {
		prepare(previous);
	}
	lock_guard<mutex> guard(lock);
	views[key] = std::move(model);
	return true;
}

bool SemanticViewRegistry::Drop(const SemanticQualifiedName &name,
                                const std::function<void(shared_ptr<SemanticModel>)> &prepare) {
	lock_guard<mutex> writing(ddl_lock);
	auto previous = Get(name);
	if (prepare) {
		prepare(previous);
	}
	lock_guard<mutex> guard(lock);
	return views.erase(Key(name)) > 0;
}

bool SemanticViewRegistry::Replace(const SemanticModel &expected, shared_ptr<SemanticModel> model,
                                   const std::function<void()> &prepare) {
	lock_guard<mutex> writing(ddl_lock);
	auto old_key = Key(expected.Name());
	auto new_key = Key(model->Name());
	{
		lock_guard<mutex> guard(lock);
		auto current = views.find(old_key);
		if (current == views.end() || current->second.get() != &expected) {
			return false;
		}
		if (new_key != old_key && views.find(new_key) != views.end()) {
			throw CatalogException("Semantic view with name \"%s\" already exists", model->Name().name);
		}
	}
	if (prepare) {
		prepare();
	}
	lock_guard<mutex> guard(lock);
	views.erase(old_key);
	views[new_key] = std::move(model);
	return true;
}

shared_ptr<SemanticModel> SemanticViewRegistry::Get(const SemanticQualifiedName &fully_qualified) const {
	lock_guard<mutex> guard(lock);
	auto entry = views.find(Key(fully_qualified));
	return entry == views.end() ? nullptr : entry->second;
}

shared_ptr<SemanticModel> SemanticViewRegistry::Lookup(ClientContext &context,
                                                       const SemanticQualifiedName &name) const {
	bool has_catalog = IsSet(name.catalog, INVALID_CATALOG);
	bool has_schema = IsSet(name.schema, INVALID_SCHEMA);
	vector<SemanticQualifiedName> candidates;
	auto &search_path = *ClientData::Get(context).catalog_search_path;
	auto default_db = DatabaseManager::GetDefaultDatabase(context).GetIdentifierName();
	if (has_catalog && has_schema) {
		candidates.push_back(name);
	} else if (has_schema) {
		// x.view: schema x of the search-path databases, or database x
		for (auto &entry : search_path.Get()) {
			SemanticQualifiedName candidate = name;
			candidate.catalog = entry.GetCatalog().empty() ? default_db : entry.GetCatalog().GetIdentifierName();
			candidates.push_back(candidate);
		}
		SemanticQualifiedName as_catalog;
		as_catalog.catalog = name.schema;
		as_catalog.schema = DefaultSchema(context, search_path, Identifier(name.schema)).GetIdentifierName();
		as_catalog.name = name.name;
		candidates.push_back(as_catalog);
	} else {
		for (auto &entry : search_path.Get()) {
			SemanticQualifiedName candidate;
			candidate.catalog = entry.GetCatalog().empty() ? default_db : entry.GetCatalog().GetIdentifierName();
			candidate.schema = entry.GetSchema().GetIdentifierName();
			candidate.name = name.name;
			candidates.push_back(candidate);
		}
	}
	for (auto &candidate : candidates) {
		auto result = Get(candidate);
		if (result) {
			return result;
		}
	}
	return nullptr;
}

shared_ptr<SemanticModel> SemanticViewRegistry::GetOrThrow(ClientContext &context,
                                                           const SemanticQualifiedName &name) const {
	auto result = Lookup(context, name);
	if (!result) {
		throw CatalogException("Semantic view with name \"%s\" does not exist!", name.name);
	}
	return result;
}

vector<shared_ptr<SemanticModel>> SemanticViewRegistry::List() const {
	lock_guard<mutex> guard(lock);
	vector<shared_ptr<SemanticModel>> result;
	for (auto &entry : views) {
		result.push_back(entry.second);
	}
	return result;
}

} // namespace duckdb
