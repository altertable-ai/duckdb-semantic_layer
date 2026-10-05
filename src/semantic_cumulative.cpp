#include "semantic_cumulative.hpp"

#include "duckdb/common/string_util.hpp"
#include "duckdb/common/to_string.hpp"
#include "duckdb/parser/expression/constant_expression.hpp"
#include "duckdb/parser/expression/function_expression.hpp"
#include "duckdb/parser/expression/star_expression.hpp"
#include "duckdb/parser/expression/window_expression.hpp"
#include "duckdb/parser/tableref/joinref.hpp"
#include "semantic_ast_helpers.hpp"

namespace duckdb {

//===--------------------------------------------------------------------===//
// Anchor map
//===--------------------------------------------------------------------===//
static string LeafColumn(idx_t index) {
	return "__sv_i" + to_string(index);
}

SemanticAnchorMap::SemanticAnchorMap(const SemanticModel &model, const SemanticCumulative &cumulative,
                                     const vector<idx_t> &offsets)
    : cumulative(cumulative), offsets(model, offsets) {
}

unique_ptr<ParsedExpression> SemanticAnchorMap::Column(idx_t table, const string &column,
                                                       unique_ptr<ParsedExpression> compiled) {
	if (table == cumulative.table && StringUtil::CIEquals(column, cumulative.column)) {
		return offsets.Column(table, column, ColumnRef(CUMULATIVE_ANCHOR_COLUMN));
	}
	return Leaf(offsets.Column(table, column, std::move(compiled)));
}

unique_ptr<ParsedExpression> SemanticAnchorMap::ChildAggregation(unique_ptr<ParsedExpression> compiled) {
	return Leaf(std::move(compiled));
}

unique_ptr<ParsedExpression> SemanticAnchorMap::Leaf(unique_ptr<ParsedExpression> compiled) {
	compiled->alias.clear();
	for (idx_t i = 0; i < leaves.size(); i++) {
		if (leaves[i]->Equals(*compiled)) {
			return ColumnRef(LeafColumn(i));
		}
	}
	leaves.push_back(std::move(compiled));
	return ColumnRef(LeafColumn(leaves.size() - 1));
}

vector<unique_ptr<ParsedExpression>> SemanticAnchorMap::TakeLeaves() {
	auto result = std::move(leaves);
	leaves.clear();
	for (idx_t i = 0; i < result.size(); i++) {
		result[i]->alias = LeafColumn(i);
	}
	return result;
}

//===--------------------------------------------------------------------===//
// Block
//===--------------------------------------------------------------------===//
namespace {

unique_ptr<ParsedExpression> Call(const string &name, vector<unique_ptr<ParsedExpression>> children) {
	return make_uniq<FunctionExpression>(name, std::move(children));
}

//! max(__sv_c) OVER (): the latest date of the block's rows
unique_ptr<ParsedExpression> LatestDate() {
	auto result = make_uniq<WindowExpression>(ExpressionType::WINDOW_AGGREGATE, INVALID_CATALOG, INVALID_SCHEMA, "max");
	result->children.push_back(ColumnRef(CUMULATIVE_DATE_COLUMN));
	result->start = WindowBoundary::UNBOUNDED_PRECEDING;
	result->end = WindowBoundary::UNBOUNDED_FOLLOWING;
	result->alias = CUMULATIVE_BOUND_COLUMN;
	return std::move(result);
}

//! Stage 2 over the raw rows: every row, with the latest date
unique_ptr<SelectNode> BoundRows(unique_ptr<SelectNode> rows) {
	auto node = make_uniq<SelectNode>();
	node->select_list.push_back(make_uniq<StarExpression>());
	node->select_list.push_back(LatestDate());
	node->from_table = MakeSubqueryRef(std::move(rows), "__sv_rows");
	return node;
}

//! The date __sv_c lies in the window of anchor __sv_t, which does not come after the latest date
unique_ptr<ParsedExpression> WindowCondition(const SemanticCumulative &cumulative) {
	vector<unique_ptr<ParsedExpression>> conditions;
	conditions.push_back(Compare(ExpressionType::COMPARE_LESSTHANOREQUALTO, ColumnRef(CUMULATIVE_DATE_COLUMN),
	                             ColumnRef(CUMULATIVE_ANCHOR_COLUMN)));
	switch (cumulative.kind) {
	case SemanticCumulativeKind::WINDOW: {
		vector<unique_ptr<ParsedExpression>> children;
		children.push_back(ColumnRef(CUMULATIVE_ANCHOR_COLUMN));
		children.push_back(cumulative.window->Copy());
		auto start = make_uniq<FunctionExpression>("-", std::move(children), nullptr, nullptr, false, true);
		conditions.push_back(
		    Compare(ExpressionType::COMPARE_GREATERTHAN, ColumnRef(CUMULATIVE_DATE_COLUMN), std::move(start)));
		break;
	}
	case SemanticCumulativeKind::GRAIN: {
		vector<unique_ptr<ParsedExpression>> children;
		children.push_back(make_uniq<ConstantExpression>(Value(cumulative.part)));
		children.push_back(ColumnRef(CUMULATIVE_ANCHOR_COLUMN));
		conditions.push_back(Compare(ExpressionType::COMPARE_GREATERTHANOREQUALTO, ColumnRef(CUMULATIVE_DATE_COLUMN),
		                             Call("date_trunc", std::move(children))));
		break;
	}
	default:
		break;
	}
	conditions.push_back(Compare(ExpressionType::COMPARE_LESSTHANOREQUALTO, ColumnRef(CUMULATIVE_ANCHOR_COLUMN),
	                             ColumnRef(CUMULATIVE_BOUND_COLUMN)));
	return AndAll(std::move(conditions));
}

//! The value of `column` for a group of anchors
unique_ptr<ParsedExpression> Period(SemanticCumulativePeriod period, const string &column) {
	vector<unique_ptr<ParsedExpression>> children;
	children.push_back(ColumnRef(column));
	if (period == SemanticCumulativePeriod::AVERAGE) {
		return Call("avg", std::move(children));
	}
	children.push_back(ColumnRef(CUMULATIVE_ANCHOR_COLUMN));
	return Call(period == SemanticCumulativePeriod::FIRST ? "arg_min_null" : "arg_max_null", std::move(children));
}

} // namespace

unique_ptr<SelectNode> BuildSemanticCumulativeBlock(const SemanticCumulative &cumulative, SemanticCumulativeParts parts,
                                                    const vector<string> &atom_columns) {
	auto dimension_count = parts.dimensions.size();
	// stages 2 and 3: the rows joined to the anchors whose window holds their date
	auto join = make_uniq<JoinRef>(JoinRefType::REGULAR);
	join->type = JoinType::INNER;
	join->left = MakeSubqueryRef(BoundRows(std::move(parts.rows)), "__sv_dated");
	join->right = MakeSubqueryRef(std::move(parts.anchors), "__sv_anchors");
	join->condition = WindowCondition(cumulative);
	// stage 4: per dimension tuple and anchor
	auto anchored = make_uniq<SelectNode>();
	for (idx_t d = 0; d < dimension_count; d++) {
		anchored->groups.group_expressions.push_back(parts.dimensions[d]->Copy());
		parts.dimensions[d]->alias = SemanticDimensionColumn(d);
		anchored->select_list.push_back(std::move(parts.dimensions[d]));
	}
	anchored->groups.group_expressions.push_back(ColumnRef(CUMULATIVE_ANCHOR_COLUMN));
	anchored->select_list.push_back(ColumnRef(CUMULATIVE_ANCHOR_COLUMN));
	for (idx_t k = 0; k < parts.aggregates.size(); k++) {
		parts.aggregates[k]->alias = atom_columns[k];
		anchored->select_list.push_back(std::move(parts.aggregates[k]));
	}
	anchored->where_clause = std::move(parts.anchor_where);
	anchored->from_table = std::move(join);
	GroupByAll(*anchored);
	// stage 5: per dimension tuple
	auto result = make_uniq<SelectNode>();
	for (idx_t d = 0; d < dimension_count; d++) {
		result->select_list.push_back(ColumnRef(SemanticDimensionColumn(d)));
		if (!parts.dimension_is_anchor) {
			result->groups.group_expressions.push_back(ColumnRef(SemanticDimensionColumn(d)));
		}
	}
	for (auto &column : atom_columns) {
		auto value = parts.dimension_is_anchor ? ColumnRef(column) : Period(cumulative.period, column);
		value->alias = column;
		result->select_list.push_back(std::move(value));
	}
	result->from_table = MakeSubqueryRef(std::move(anchored), "__sv_anchored");
	GroupByAll(*result);
	return result;
}

} // namespace duckdb
