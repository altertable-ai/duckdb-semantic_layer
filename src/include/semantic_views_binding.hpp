//===----------------------------------------------------------------------===//
//                         DuckDB - semantic_views
//
// semantic_views_binding.hpp
//
// Hooks the extension into DuckDB's parse / bind pipeline:
// - semantic-view syntax is a grammar extension of the PEG parser
//   (semantic_views_grammar.hpp), active on every connection;
// - standard-SQL queries over a semantic view by name fail to bind; a client
//   context state rewrites them (semantic_sql_rewrite.hpp) and asks for a
//   rebind.
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/shared_ptr.hpp"

namespace duckdb {

class DatabaseInstance;
struct SemanticViewsState;

void RegisterSemanticViewsBinding(DatabaseInstance &db, shared_ptr<SemanticViewsState> state);

} // namespace duckdb
