//===----------------------------------------------------------------------===//
//                         DuckDB - semantic_layer
//
// semantic_standard_form.hpp
//
// One query node reading a semantic view by name:
//   SELECT dim, AGG(metric) FROM semantic_view [WHERE ...] GROUP BY dim [HAVING ...]
// becomes an outer query over the internal SEMANTIC_VIEW(...) table function.
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/parser/query_node/select_node.hpp"
#include "duckdb/parser/tableref/basetableref.hpp"

namespace duckdb {

class ClientContext;
class SemanticModel;

//! Rewrites `node`, whose FROM clause is `ref`, a reference to the semantic view `model`
void RewriteStandardForm(ClientContext &context, const SemanticModel &model, const BaseTableRef &ref, SelectNode &node);

} // namespace duckdb
