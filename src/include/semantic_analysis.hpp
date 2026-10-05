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
	//! While decomposing: metrics whose FILTER applies (outer metrics first). Applied, then cleared, when an atom is
	//! created (SemanticAnalyzer::ApplyMetricContext).
	vector<idx_t> filters;
	//! Offsets (SemanticModel::offsets) that apply, in the order the shifts are applied (innermost metric first)
	vector<idx_t> offsets;
	//! CUMULATIVE (SemanticModel::cumulatives) that applies, or INVALID_INDEX
	idx_t cumulative = DConstants::INVALID_INDEX;
	//! The metric of that CUMULATIVE (error messages only, not compared)
	idx_t cumulative_member = DConstants::INVALID_INDEX;

	bool operator==(const SemanticMetricContext &other) const {
		return table == other.table && using_relationships == other.using_relationships &&
		       non_additive_member == other.non_additive_member && filters == other.filters &&
		       offsets == other.offsets && cumulative == other.cumulative;
	}
};

//! What atoms aggregated together (one aggregation block) share: the rows of the same home table, joined through the
//! same USING relationships, filtered, picked (NON ADDITIVE BY), shifted (OFFSET) and windowed (CUMULATIVE) the same
//! way
struct SemanticBlockKey {
	idx_t home;
	vector<idx_t> using_relationships;
	//! Metric whose NON ADDITIVE BY picks the last snapshot, or INVALID_INDEX
	idx_t non_additive_member;
	//! Metrics whose FILTER filters the rows (semi-additive atoms only, see SemanticAtom::row_filters)
	vector<idx_t> row_filters;
	//! Offsets (SemanticModel::offsets) shifting the query's dimensions and WHERE, in application order
	vector<idx_t> offsets;
	//! CUMULATIVE (SemanticModel::cumulatives) of the block, or INVALID_INDEX
	idx_t cumulative;

	bool operator==(const SemanticBlockKey &other) const {
		return home == other.home && using_relationships == other.using_relationships &&
		       non_additive_member == other.non_additive_member && row_filters == other.row_filters &&
		       offsets == other.offsets && cumulative == other.cumulative;
	}
};

//! An aggregate evaluated over the rows of `home`.
//! Move-only: MSVC instantiates `vector`'s copy constructor unless the copy is explicitly deleted.
struct SemanticAtom {
	idx_t home;
	//! The aggregate expression (resolved; marker references), metric FILTERs included
	unique_ptr<ParsedExpression> aggregate;
	SemanticMetricContext context;
	//! Metrics whose FILTER filters the rows of the atom's block instead of its aggregate: semi-additive atoms pick
	//! their last snapshot among the filtered rows
	vector<idx_t> row_filters;

	SemanticAtom() = default;
	SemanticAtom(const SemanticAtom &) = delete;
	SemanticAtom &operator=(const SemanticAtom &) = delete;
	SemanticAtom(SemanticAtom &&) noexcept = default;
	SemanticAtom &operator=(SemanticAtom &&) noexcept = default;

	SemanticAtom Copy() const {
		SemanticAtom result;
		result.home = home;
		result.aggregate = aggregate->Copy();
		result.context = context;
		result.row_filters = row_filters;
		return result;
	}
	//! Whether both atoms compute the same values
	bool Equals(const SemanticAtom &other) const {
		return home == other.home && aggregate->Equals(*other.aggregate) && context == other.context &&
		       row_filters == other.row_filters;
	}
	SemanticBlockKey BlockKey() const {
		return SemanticBlockKey {home,
		                         context.using_relationships,
		                         context.non_additive_member,
		                         row_filters,
		                         context.offsets,
		                         context.cumulative};
	}
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
	//! Applies the metric context of a new atom: checks the tables of its OFFSET dimensions and FILTERs are reachable
	//! from its home table, and moves the FILTERs out of the context, AND-ed into its aggregate (or into its row
	//! filters when it is semi-additive)
	void ApplyMetricContext(SemanticAtom &atom, const string &what);
	SemanticMetricContext MetricContext(idx_t metric, const SemanticMetricContext &outer, const string &what) const;

	const SemanticModel &model;
	ClientContext &context;
	case_insensitive_map_t<bool> aggregate_cache;
};

} // namespace duckdb
