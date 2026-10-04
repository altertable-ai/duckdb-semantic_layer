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

//! Default schema of materialization tables, and of the metadata recording them (in every database holding some)
static constexpr const char *SEMANTIC_MATERIALIZATION_SCHEMA = "__semantic_materializations";
static constexpr const char *SEMANTIC_MATERIALIZATION_METADATA = "__sv_materializations";

//! Analyzes one materialization of `model`
shared_ptr<SemanticMaterialization> PlanSemanticMaterialization(ClientContext &context, SemanticModel &model,
                                                                const ParsedSemanticMaterialization &parsed);

//! The state kind of an aggregate call (resolved) with result type `type`
SemanticAggregateKind ClassifySemanticAggregate(const ParsedExpression &expr, const LogicalType &type);
//! The aliased state columns of `atom`, computed from its compiled aggregate call
vector<unique_ptr<ParsedExpression>> SemanticStateExpressions(const SemanticMaterializedAtom &atom,
                                                              const FunctionExpression &compiled);
//! The aggregate computing `atom` from its state columns in table `alias` (rolled up over the grouped rows)
unique_ptr<ParsedExpression> SemanticRollupExpression(const SemanticMaterializedAtom &atom, const string &alias);
//! One current classification of every logical source read by a materialization.
struct SemanticMaterializationSources {
	SemanticMaterializationSources(ClientContext &context, const SemanticModel &model,
	                               const SemanticMaterialization &materialization);

	//! Whether the watermark covers exactly the catalogs of all known, tracked sources.
	bool Covers(const map<string, idx_t> &watermark) const;

	map<idx_t, SemanticSourceTable> by_table;
	map<string, vector<SemanticSourceTable>> by_catalog;
	bool complete = true;
	string untracked_reason;
};

} // namespace duckdb
