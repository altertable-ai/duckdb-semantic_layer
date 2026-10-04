//===----------------------------------------------------------------------===//
//                         DuckDB - semantic_views
//
// semantic_analysis.hpp
//
// Grain analysis over resolved semantic expressions: which logical tables an
// expression lives on, and how metric expressions decompose into aggregate
// "atoms" evaluated over the rows of a home table.
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/case_insensitive_map.hpp"
#include "duckdb/common/set.hpp"
#include "duckdb/parser/expression/function_expression.hpp"
#include "semantic_markers.hpp"
#include "semantic_model.hpp"

namespace duckdb {

//! The metric an aggregate belongs to
struct SemanticMetricContext {
	//! Table whose rows give the grain of nested (child) aggregates; INVALID_INDEX for view-level metrics
	idx_t table = DConstants::INVALID_INDEX;
	//! Relationships pinned with USING (sorted)
	vector<idx_t> using_relationships;
	//! Metric whose NON ADDITIVE BY applies, or INVALID_INDEX
	idx_t non_additive_member = DConstants::INVALID_INDEX;
};

//! An aggregate evaluated over the rows of `home`
struct SemanticAtom {
	idx_t home;
	//! The aggregate expression (resolved; marker references)
	unique_ptr<ParsedExpression> aggregate;
	SemanticMetricContext context;
};

class SemanticAnalyzer {
public:
	SemanticAnalyzer(const SemanticModel &model, ClientContext &context);

	ClientContext &Context() const {
		return context;
	}

	//! Whether `expr` is a call to an aggregate function
	bool IsAggregate(const ParsedExpression &expr);
	//! Whether `expr` contains an aggregate or a metric reference (not looking into subqueries)
	bool ContainsAggregateOrMetric(const ParsedExpression &expr);
	static bool ContainsWindow(const ParsedExpression &expr);

	//! Logical tables a row-level expression reads, evaluated in the row context of `context_table`
	//! (INVALID_INDEX for view-level expressions). Child aggregations count as `context_table`.
	void RowTables(const ParsedExpression &expr, idx_t context_table, set<idx_t> &tables, const string &what);
	//! The table (of `tables`) from which every other table is reachable; INVALID_INDEX when `tables` is empty
	idx_t FinestTable(const set<idx_t> &tables, const string &what);
	//! Home table of an aggregate call evaluated in `context_table` (INVALID_INDEX = view level)
	idx_t AggregateHome(const FunctionExpression &aggregate, idx_t context_table, const string &what);

	//! Splits a metric-level expression into a post-aggregation expression over atom markers. Metric references
	//! are inlined recursively (with their own USING / NON ADDITIVE BY); window functions are kept, with their
	//! PARTITION BY / ORDER BY dimension references left for the compiler to map onto query columns.
	unique_ptr<ParsedExpression> Decompose(const ParsedExpression &expr, const SemanticMetricContext &context,
	                                       vector<SemanticAtom> &atoms, const string &what);
	//! Decompose() of a metric in its own context
	unique_ptr<ParsedExpression> DecomposeMetric(idx_t metric, vector<SemanticAtom> &atoms, const string &what);
	//! Dimensions referenced by the window clauses of a (metric-level) expression, following metric references
	void WindowDimensions(const ParsedExpression &expr, set<idx_t> &dimensions) const;

private:
	SemanticMetricContext MetricContext(idx_t metric, const SemanticMetricContext &outer) const;

	const SemanticModel &model;
	ClientContext &context;
	case_insensitive_map_t<bool> aggregate_cache;
};

} // namespace duckdb
