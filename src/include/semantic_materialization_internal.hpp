//===----------------------------------------------------------------------===//
//                         DuckDB - semantic_views
//
// semantic_materialization_internal.hpp
//
// Helpers shared by the materialization planner / matcher and its refresh.
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/parser/expression/function_expression.hpp"
#include "duckdb/parser/keyword_helper.hpp"
#include "semantic_change_tracking.hpp"
#include "semantic_materialization.hpp"

namespace duckdb {

inline string QuoteIdentifier(const string &identifier) {
	return KeywordHelper::WriteOptionallyQuoted(identifier, '"');
}

//! The state kind of an aggregate call (resolved) with result type `type`
SemanticAggregateKind ClassifySemanticAggregate(const ParsedExpression &expr, const LogicalType &type);
//! The aliased state columns of `atom`, computed from its compiled aggregate call
vector<unique_ptr<ParsedExpression>> SemanticStateExpressions(const SemanticMaterializedAtom &atom,
                                                              const FunctionExpression &compiled);
//! The aggregate computing `atom` from its state columns in table `alias` (rolled up over the grouped rows)
unique_ptr<ParsedExpression> SemanticRollupExpression(const SemanticMaterializedAtom &atom, const string &alias);
//! The tracked source tables of a materialization, per DuckLake catalog; `all_tracked` tells whether every source is
map<string, vector<SemanticSourceTable>> SemanticMaterializationSources(ClientContext &context,
                                                                        const SemanticModel &model,
                                                                        const SemanticMaterialization &materialization,
                                                                        bool &all_tracked);

} // namespace duckdb
