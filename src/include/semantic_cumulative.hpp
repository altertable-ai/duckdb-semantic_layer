//===----------------------------------------------------------------------===//
//                         DuckDB - semantic_views
//
// semantic_cumulative.hpp
//
// Metric CUMULATIVEs in queries. A cumulative block aggregates, for every anchor date t, the rows whose stored date
// lies in t's window; the query's dimensions and WHERE read t instead of the stored date:
//   1. rows: the stored date __sv_c, leaves __sv_i<n> (everything else the dimensions / WHERE read), the aggregate
//      arguments __sv_x<n>, filtered by the WHERE conjuncts that do not read the date
//   2. the latest date __sv_cmax (anchors never come after it)
//   3. joined to the anchors __sv_t (distinct dates of the cumulative column's table) whose window holds __sv_c
//   4. aggregated per dimension tuple and anchor (the dimensions read __sv_t)
//   5. one row per dimension tuple: the value at its last / first anchor, or the average over its anchors
//
//===----------------------------------------------------------------------===//

#pragma once

#include "semantic_offsets.hpp"

namespace duckdb {

static constexpr const char *CUMULATIVE_DATE_COLUMN = "__sv_c";
static constexpr const char *CUMULATIVE_ANCHOR_COLUMN = "__sv_t";
static constexpr const char *CUMULATIVE_BOUND_COLUMN = "__sv_cmax";

//! Compiles the query's dimensions and WHERE in a cumulative block (SemanticExpressionCompiler::MappedRow): the
//! cumulative column reads the anchor `__sv_t` (shifted by the block's OFFSETs); every other column and child
//! aggregation becomes a leaf, a column `__sv_i<n>` of the block's rows
class SemanticAnchorMap : public SemanticColumnMap {
public:
	SemanticAnchorMap(const SemanticModel &model, const SemanticCumulative &cumulative, const vector<idx_t> &offsets);

	unique_ptr<ParsedExpression> Column(idx_t table, const string &column,
	                                    unique_ptr<ParsedExpression> compiled) override;
	unique_ptr<ParsedExpression> ChildAggregation(unique_ptr<ParsedExpression> compiled) override;
	//! The reference to the leaf computing `compiled` in the block's rows
	unique_ptr<ParsedExpression> Leaf(unique_ptr<ParsedExpression> compiled);
	//! The leaves, aliased `__sv_i<n>`
	vector<unique_ptr<ParsedExpression>> TakeLeaves();

private:
	const SemanticCumulative &cumulative;
	SemanticOffsetMap offsets;
	vector<unique_ptr<ParsedExpression>> leaves;
};

//! A cumulative block as compiled by the query compiler
struct SemanticCumulativeParts {
	//! Stage 1 (its FROM and WHERE included)
	unique_ptr<SelectNode> rows;
	//! The names of the leaves among the rows' columns
	vector<string> leaves;
	//! The query's dimensions, over `__sv_t` and the leaves
	vector<unique_ptr<ParsedExpression>> dimensions;
	//! The WHERE conjuncts reading the cumulative column, over `__sv_t` and the leaves; null when there are none
	unique_ptr<ParsedExpression> anchor_where;
	//! Per atom of the block, its aggregate over the rows' columns
	vector<unique_ptr<ParsedExpression>> aggregates;
	//! SELECT DISTINCT <cumulative column> AS __sv_t FROM <its table> WHERE <cumulative column> IS NOT NULL
	unique_ptr<QueryNode> anchors;
	//! Whether a dimension is the cumulative dimension itself (one anchor per dimension tuple: no period aggregation)
	bool dimension_is_anchor = false;
};

//! SELECT <dimensions AS __sv_d<i>>, <atom k AS atom_columns[k]> ... GROUP BY <dimensions>
unique_ptr<SelectNode> BuildSemanticCumulativeBlock(const SemanticCumulative &cumulative, SemanticCumulativeParts parts,
                                                    const vector<string> &atom_columns);

} // namespace duckdb
