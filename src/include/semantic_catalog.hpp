//===----------------------------------------------------------------------===//
//                         DuckDB - semantic_views
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

struct SemanticCatalogColumn {
	string name;
	LogicalType type;
	//! NULL when the member has no comment
	Value comment;
};

//! Public facts, dimensions and metrics (in that order); names shared by several members are qualified as
//! "table.name"
vector<SemanticCatalogColumn> SemanticCatalogColumns(const SemanticModel &model);

//! Called while binding DDL on `name` (fully qualified): fails when a table or view that is not a placeholder has the
//! name (`check_conflict`), and declares the catalog modification on the statement
void PrepareSemanticPlaceholder(ClientContext &context, optional_ptr<Binder> binder, const QualifiedName &name,
                                bool check_conflict);
//! Creates (or replaces) the placeholder of `model`
void CreateSemanticPlaceholder(ClientContext &context, const SemanticModel &model);
//! Drops the placeholder named `name` (fully qualified); false when there is none
bool DropSemanticPlaceholder(ClientContext &context, const QualifiedName &name);
//! Whether a catalog entry is the placeholder of a semantic view
bool IsSemanticPlaceholder(const CatalogEntry &entry);
//! SELECT CAST(NULL AS <type>) AS "<column>", ... LIMIT 0: the placeholder's columns, for DESCRIBE
unique_ptr<SelectNode> SemanticDescribeNode(const SemanticModel &model);

void RegisterSemanticPlaceholderFunction(ExtensionLoader &loader, shared_ptr<SemanticViewsState> state);

} // namespace duckdb
