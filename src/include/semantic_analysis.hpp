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
#include "semantic_model.hpp"

namespace duckdb {

//! An aggregate evaluated over the rows of `home`
struct SemanticAtom {
	idx_t home;
	//! The aggregate expression (resolved; marker references)
	unique_ptr<ParsedExpression> aggregate;
	//! Logical table whose rows provide the grain for nested (child) aggregates inside `aggregate`
	idx_t context_table;
	//! Relationships pinned by the metric the atom comes from (USING)
	vector<idx_t> using_relationships;
	//! Metric whose NON ADDITIVE BY applies to this atom, or INVALID_INDEX
	idx_t non_additive_member = DConstants::INVALID_INDEX;
};

class SemanticAnalyzer {
public:
	SemanticAnalyzer(const SemanticModel &model, ClientContext &context);

	//! Whether `expr` is a call to an aggregate function
	bool IsAggregate(const ParsedExpression &expr);
	//! Whether `expr` contains an aggregate or a metric reference (not looking into subqueries)
	bool ContainsAggregateOrMetric(const ParsedExpression &expr);

	//! Logical tables a row-level expression reads, evaluated in the row context of `context_table`
	//! (INVALID_INDEX for view-level expressions). Child aggregations count as `context_table`.
	void RowTables(const ParsedExpression &expr, idx_t context_table, set<idx_t> &tables, const string &what);
	//! The table (of `tables`) from which every other table is reachable; INVALID_INDEX when `tables` is empty
	idx_t FinestTable(const set<idx_t> &tables, const string &what);
	//! Home table of a row-level view-level expression (INVALID_INDEX for constants)
	idx_t RowHome(const ParsedExpression &expr, const string &what);
	//! Home table of an aggregate call evaluated in `context_table` (INVALID_INDEX = view level)
	idx_t AggregateHome(const FunctionExpression &aggregate, idx_t context_table, const string &what);

	//! Splits a metric-level expression into a post-aggregation expression over atom references.
	//! Metric references are inlined recursively. Throws when row-level values appear outside aggregates.
	unique_ptr<ParsedExpression> Decompose(const ParsedExpression &expr, idx_t context_table,
	                                       vector<SemanticAtom> &atoms, const string &what,
	                                       const vector<idx_t> &using_relationships = {},
	                                       idx_t non_additive_member = DConstants::INVALID_INDEX);

	static unique_ptr<ParsedExpression> MakeAtomRef(idx_t atom);
	static bool IsAtomRef(const ParsedExpression &expr, idx_t &atom);

private:
	const SemanticModel &model;
	ClientContext &context;
	case_insensitive_map_t<bool> aggregate_cache;
};

} // namespace duckdb
