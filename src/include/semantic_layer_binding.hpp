//===----------------------------------------------------------------------===//
//                         DuckDB - semantic_views
//
// semantic_layer_binding.hpp
//
// Hooks the extension into DuckDB's parse / bind pipeline:
// - statements with semantic-view syntax fail the postgres parser and reach
//   the parser extension, which parses them with SemanticViewsParser; the
//   lowered statement is handed to an operator extension that binds it;
// - standard-SQL queries over a semantic view by name fail to bind; a client
//   context state rewrites them (semantic_sql_rewrite.hpp) and asks for a rebind.
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/shared_ptr.hpp"

namespace duckdb {

class DatabaseInstance;
struct SemanticViewsState;

void RegisterSemanticViewsBinding(DatabaseInstance &db, shared_ptr<SemanticViewsState> state);

} // namespace duckdb
