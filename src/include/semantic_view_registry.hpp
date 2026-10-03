//===----------------------------------------------------------------------===//
//                         DuckDB - semantic_views
//
// semantic_view_registry.hpp
//
// In-memory registry of semantic views, one per database instance.
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/mutex.hpp"
#include "duckdb/function/table_function.hpp"
#include "semantic_model.hpp"

namespace duckdb {

class ClientContext;

class SemanticViewRegistry {
public:
	//! Resolves a (catalog, schema) pair: missing parts default to the client's search path; `name` is left empty
	//! The schema must exist, except with `RETURN_NULL` (IF EXISTS): a missing schema is then kept as written. A
	//! missing database is always an error (like DROP VIEW IF EXISTS).
	static QualifiedName ResolveSchema(ClientContext &context, const string &catalog, const string &schema,
	                                   OnEntryNotFound if_not_found = OnEntryNotFound::THROW_EXCEPTION);
	//! Resolves (catalog, schema) of a name used in DDL
	static QualifiedName ResolveDDLName(ClientContext &context, const QualifiedName &name,
	                                    OnEntryNotFound if_not_found = OnEntryNotFound::THROW_EXCEPTION);

	//! Registers `model`; returns false when a view with the same name exists and `replace` is false
	bool Create(shared_ptr<SemanticModel> model, bool replace);
	//! Returns false when the view does not exist
	bool Drop(const QualifiedName &name);
	//! Atomically replaces `expected` with `model` (ALTER), which may have a different name; throws when the new name
	//! is taken by another view. Returns false when `expected` is no longer registered (dropped or altered meanwhile).
	bool Replace(const SemanticModel &expected, shared_ptr<SemanticModel> model);

	shared_ptr<SemanticModel> Get(const QualifiedName &fully_qualified) const;
	//! Looks up a name as used in a query (unqualified names follow the search path); nullptr when missing
	shared_ptr<SemanticModel> Lookup(ClientContext &context, const QualifiedName &name) const;
	//! Like Lookup, but throws a CatalogException when missing
	shared_ptr<SemanticModel> GetOrThrow(ClientContext &context, const QualifiedName &name) const;
	//! All registered views, sorted by (catalog, schema, name)
	vector<shared_ptr<SemanticModel>> List() const;

private:
	static string Key(const QualifiedName &name);

	mutable mutex lock;
	map<string, shared_ptr<SemanticModel>> views;
};

//! Extension state shared by all functions of the extension
struct SemanticViewsState {
	SemanticViewRegistry registry;
};

struct SemanticViewsFunctionInfo : public TableFunctionInfo {
	explicit SemanticViewsFunctionInfo(shared_ptr<SemanticViewsState> state_p) : state(std::move(state_p)) {
	}
	shared_ptr<SemanticViewsState> state;
};

} // namespace duckdb
