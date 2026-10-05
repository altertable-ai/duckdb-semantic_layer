#include "semantic_cumulative.hpp"

#include "duckdb/common/string_util.hpp"
#include "duckdb/common/to_string.hpp"
#include "duckdb/parser/expression/constant_expression.hpp"
#include "duckdb/parser/expression/function_expression.hpp"
#include "duckdb/parser/expression/star_expression.hpp"
#include "duckdb/parser/expression/window_expression.hpp"
#include "duckdb/parser/tableref/joinref.hpp"
#include "duckdb/parser/expression/columnref_expression.hpp"
#include "duckdb/planner/binder.hpp"
#include "semantic_ast_helpers.hpp"
#include "semantic_materialization_internal.hpp"

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

//! Stage 2 when every aggregate of the block can be pre-aggregated: the rows grouped by date, leaves and the
//! arguments of the DISTINCT aggregates (each distinct value is kept once), with the states of the other aggregates
//! (as materializations store them), and the latest date. Rewrites `parts.aggregates` to roll the states up. nullptr
//! (with `parts` unchanged) when an aggregate can only read the raw rows (MEDIAN, ordered aggregates, ...).
unique_ptr<SelectNode> PreAggregate(ClientContext &context, SemanticCumulativeParts &parts) {
	// the result type of each aggregate picks its state
	vector<LogicalType> types;
	try {
		auto probe = make_uniq<SelectNode>();
		for (auto &aggregate : parts.aggregates) {
			probe->select_list.push_back(aggregate->Copy());
		}
		auto rows = parts.rows->Copy();
		// the WHERE may hold prepared statement parameters; it does not change the types
		rows->Cast<SelectNode>().where_clause = nullptr;
		probe->from_table = MakeSubqueryRef(std::move(rows), "__sv_rows");
		auto statement = MakeSelectStatement(std::move(probe));
		auto binder = Binder::CreateBinder(context);
		types = binder->Bind(statement->Cast<SQLStatement>()).types;
	} catch (std::exception &) {
		// e.g. a parameter in a many-to-many bridge's condition: aggregate the raw rows
		return nullptr;
	}
	vector<SemanticMaterializedAtom> states(parts.aggregates.size());
	for (idx_t k = 0; k < parts.aggregates.size(); k++) {
		auto &function = parts.aggregates[k]->Cast<FunctionExpression>();
		states[k].kind = ClassifySemanticAggregate(function, types[k]);
		states[k].type = types[k];
		states[k].prefix = "__sv_s" + to_string(k);
		auto ordered = function.order_bys && !function.order_bys->orders.empty();
		if (states[k].kind == SemanticAggregateKind::OPAQUE &&
		    (!function.distinct || ordered || function.export_state)) {
			return nullptr;
		}
	}
	auto node = make_uniq<SelectNode>();
	auto group_by = [&](const string &column) {
		for (auto &group : node->groups.group_expressions) {
			if (group->Cast<ColumnRefExpression>().GetColumnName() == column) {
				return;
			}
		}
		node->select_list.push_back(ColumnRef(column));
		node->groups.group_expressions.push_back(ColumnRef(column));
	};
	group_by(CUMULATIVE_DATE_COLUMN);
	for (auto &leaf : parts.leaves) {
		group_by(leaf);
	}
	for (idx_t k = 0; k < parts.aggregates.size(); k++) {
		auto &function = parts.aggregates[k]->Cast<FunctionExpression>();
		if (states[k].kind == SemanticAggregateKind::OPAQUE) {
			// DISTINCT: its arguments (columns of the rows) are kept, once per distinct value
			for (auto &child : function.children) {
				group_by(child->Cast<ColumnRefExpression>().GetColumnName());
			}
			if (function.filter) {
				group_by(function.filter->Cast<ColumnRefExpression>().GetColumnName());
			}
			continue;
		}
		for (auto &state : SemanticStateExpressions(states[k], function)) {
			node->select_list.push_back(std::move(state));
		}
		parts.aggregates[k] = SemanticRollupExpression(states[k], string());
	}
	node->select_list.push_back(LatestDate());
	node->from_table = MakeSubqueryRef(std::move(parts.rows), "__sv_rows");
	GroupByAll(*node);
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

unique_ptr<SelectNode> BuildSemanticCumulativeBlock(ClientContext &context, const SemanticCumulative &cumulative,
                                                    SemanticCumulativeParts parts, const vector<string> &atom_columns) {
	auto dimension_count = parts.dimensions.size();
	// stage 2: the pre-aggregated or raw rows, with the latest date
	auto dated = PreAggregate(context, parts);
	if (!dated) {
		dated = BoundRows(std::move(parts.rows));
	}
	// stage 3: joined to the anchors whose window holds their date
	auto join = make_uniq<JoinRef>(JoinRefType::REGULAR);
	join->type = JoinType::INNER;
	join->left = MakeSubqueryRef(std::move(dated), "__sv_dated");
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
