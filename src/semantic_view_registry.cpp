#include "semantic_view_registry.hpp"

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

QualifiedName SemanticViewRegistry::ResolveSchema(ClientContext &context, const string &catalog_p,
                                                  const string &schema_p) {
	string catalog = IsSet(catalog_p, INVALID_CATALOG) ? catalog_p : string();
	string schema = IsSet(schema_p, INVALID_SCHEMA) ? schema_p : string();
	if (catalog.empty() && !schema.empty()) {
		// `x.view`: x may be a schema of the default database or a database name
		Binder::BindSchemaOrCatalog(context, catalog, schema);
	}
	auto &search_path = *ClientData::Get(context).catalog_search_path;
	if (catalog.empty() && schema.empty()) {
		auto &entry = search_path.GetDefault();
		catalog = entry.catalog;
		schema = entry.schema;
	}
	if (catalog.empty() || catalog == INVALID_CATALOG) {
		catalog = DatabaseManager::GetDefaultDatabase(context);
	}
	if (schema.empty() || schema == INVALID_SCHEMA) {
		schema = search_path.GetDefaultSchema(context, catalog);
	}
	// validates that the schema exists (and normalizes the spelling of catalog / schema)
	auto &schema_entry = Catalog::GetSchema(context, catalog, schema);
	QualifiedName result;
	result.catalog = schema_entry.ParentCatalog().GetName();
	result.schema = schema_entry.name;
	return result;
}

QualifiedName SemanticViewRegistry::ResolveDDLName(ClientContext &context, const QualifiedName &name) {
	auto result = ResolveSchema(context, name.catalog, name.schema);
	result.name = name.name;
	return result;
}

string SemanticViewRegistry::Key(const QualifiedName &name) {
	return StringUtil::Lower(name.catalog) + "\x1f" + StringUtil::Lower(name.schema) + "\x1f" +
	       StringUtil::Lower(name.name);
}

bool SemanticViewRegistry::Create(shared_ptr<SemanticModel> model, bool replace) {
	lock_guard<mutex> guard(lock);
	auto key = Key(model->Name());
	if (!replace && views.find(key) != views.end()) {
		return false;
	}
	views[key] = std::move(model);
	return true;
}

bool SemanticViewRegistry::Drop(const QualifiedName &name) {
	lock_guard<mutex> guard(lock);
	return views.erase(Key(name)) > 0;
}

bool SemanticViewRegistry::Replace(const SemanticModel &expected, shared_ptr<SemanticModel> model) {
	lock_guard<mutex> guard(lock);
	auto old_key = Key(expected.Name());
	auto new_key = Key(model->Name());
	auto current = views.find(old_key);
	if (current == views.end() || current->second.get() != &expected) {
		return false;
	}
	if (new_key != old_key && views.find(new_key) != views.end()) {
		throw CatalogException("Semantic view with name \"%s\" already exists", model->Name().name);
	}
	views.erase(old_key);
	views[new_key] = std::move(model);
	return true;
}

shared_ptr<SemanticModel> SemanticViewRegistry::Get(const QualifiedName &fully_qualified) const {
	lock_guard<mutex> guard(lock);
	auto entry = views.find(Key(fully_qualified));
	return entry == views.end() ? nullptr : entry->second;
}

shared_ptr<SemanticModel> SemanticViewRegistry::Lookup(ClientContext &context, const QualifiedName &name) const {
	bool has_catalog = IsSet(name.catalog, INVALID_CATALOG);
	bool has_schema = IsSet(name.schema, INVALID_SCHEMA);
	vector<QualifiedName> candidates;
	auto &search_path = *ClientData::Get(context).catalog_search_path;
	auto default_db = DatabaseManager::GetDefaultDatabase(context);
	if (has_catalog && has_schema) {
		candidates.push_back(name);
	} else if (has_schema) {
		// x.view: schema x of the search-path databases, or database x
		for (auto &entry : search_path.Get()) {
			QualifiedName candidate = name;
			candidate.catalog = entry.catalog.empty() || entry.catalog == INVALID_CATALOG ? default_db : entry.catalog;
			candidates.push_back(candidate);
		}
		QualifiedName as_catalog;
		as_catalog.catalog = name.schema;
		as_catalog.schema = search_path.GetDefaultSchema(context, name.schema);
		as_catalog.name = name.name;
		candidates.push_back(as_catalog);
	} else {
		for (auto &entry : search_path.Get()) {
			QualifiedName candidate;
			candidate.catalog = entry.catalog.empty() || entry.catalog == INVALID_CATALOG ? default_db : entry.catalog;
			candidate.schema = entry.schema;
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

shared_ptr<SemanticModel> SemanticViewRegistry::GetOrThrow(ClientContext &context, const QualifiedName &name) const {
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
