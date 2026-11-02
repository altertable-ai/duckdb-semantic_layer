//===----------------------------------------------------------------------===//
//                         DuckDB - semantic_layer
//
// semantic_sql_rewrite.hpp
//
// Standard-SQL form of semantic view queries:
//   SELECT dim, AGG(metric) FROM semantic_view [WHERE ...] GROUP BY dim [HAVING ...]
// is rewritten into the internal SEMANTIC_VIEW(...) table function plus an outer query.
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/parser/sql_statement.hpp"

namespace duckdb {

class ClientContext;
struct SemanticViewsState;

//! Rewrites every query node of `statement` whose FROM clause names a semantic view (and no table, view or CTE of
//! that name exists). Returns whether anything was rewritten.
bool RewriteSemanticViewReferences(ClientContext &context, SemanticViewsState &state, SQLStatement &statement);

} // namespace duckdb
