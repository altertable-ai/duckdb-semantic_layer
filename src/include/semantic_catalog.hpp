//===----------------------------------------------------------------------===//
//                         DuckDB - semantic_layer
//
// semantic_catalog.hpp
//
// Catalog visibility: every semantic view has a placeholder VIEW of the same
// name, with one typed, commented column per public member, so that
// information_schema, duckdb_views() / duckdb_columns() and BI tools list it.
// The placeholder's query always fails at bind time, which hands queries over
// to the standard-SQL rewrite (semantic_sql_rewrite.hpp).
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/catalog/catalog_entry.hpp"
#include "duckdb/parser/query_node/select_node.hpp"
#include "semantic_model.hpp"

namespace duckdb {

class Binder;
class Catalog;
class ExtensionLoader;
struct SemanticViewsState;

//! (catalog, schema, name): fails at bind time
static constexpr const char *SEMANTIC_VIEW_PLACEHOLDER_FUNCTION = "__semantic_view_placeholder";
//! Tag marking placeholder views (saved with them in database files, shown by duckdb_views())
static constexpr const char *SEMANTIC_VIEW_PLACEHOLDER_TAG = "semantic_view";

struct SemanticCatalogColumn {
	string name;
	LogicalType type;
	//! NULL when the member has no comment
	Value comment;
};

//! Public facts, dimensions and metrics (in that order); names shared by several members are qualified as
//! "table.name"
vector<SemanticCatalogColumn> SemanticCatalogColumns(const SemanticModel &model);

//! A DDL statement turns the semantic view named `before` into `after` (fully qualified names): an empty `before` name
//! creates it, an empty `after` name drops it, and both change it (ALTER, possibly renaming it).

//! At bind time: a new name must not belong to a table or view, and the statement declares its catalog modifications
void BindSemanticViewChange(ClientContext &context, optional_ptr<Binder> binder, const QualifiedName &before,
                            const QualifiedName &after);
//! At execution, before the registry changes: replaces the placeholder of `before` by the placeholder of `after`
//! (nullptr when dropped). When the registry change then fails, the statement's rollback undoes this one. Returns
//! whether a placeholder of `before` was dropped (one left in a database file by a previous session counts too).
bool ApplySemanticViewChange(ClientContext &context, const QualifiedName &before,
                             optional_ptr<const SemanticModel> after);
//! Whether a catalog entry is the placeholder of a semantic view
bool IsSemanticPlaceholder(const CatalogEntry &entry);
//! SELECT CAST(NULL AS <type>) AS "<column>", ... LIMIT 0: the placeholder's columns, for DESCRIBE
unique_ptr<SelectNode> SemanticDescribeNode(const SemanticModel &model);

void RegisterSemanticPlaceholderFunction(ExtensionLoader &loader, shared_ptr<SemanticViewsState> state);

} // namespace duckdb
