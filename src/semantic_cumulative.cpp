#include "semantic_cumulative.hpp"

#include "duckdb/common/enums/date_part_specifier.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/common/to_string.hpp"
#include "duckdb/parser/expression/cast_expression.hpp"
#include "duckdb/parser/expression/constant_expression.hpp"
#include "duckdb/parser/expression/function_expression.hpp"
#include "duckdb/parser/expression/operator_expression.hpp"
#include "duckdb/parser/expression/star_expression.hpp"
#include "duckdb/parser/expression/window_expression.hpp"
#include "duckdb/parser/common_table_expression_info.hpp"
#include "duckdb/parser/query_node/select_node.hpp"
#include "duckdb/parser/tableref/basetableref.hpp"
#include "duckdb/parser/tableref/joinref.hpp"
#include "duckdb/planner/binder.hpp"
#include "semantic_ast_helpers.hpp"
#include "semantic_materialization_internal.hpp"

namespace duckdb {

//===--------------------------------------------------------------------===//
// Window
//===--------------------------------------------------------------------===//
unique_ptr<ParsedExpression> SemanticCumulativeWindow(const SemanticCumulative &cumulative,
                                                      unique_ptr<ParsedExpression> date,
                                                      unique_ptr<ParsedExpression> anchor) {
	vector<unique_ptr<ParsedExpression>> children;
	unique_ptr<ParsedExpression> start;
	switch (cumulative.kind) {
	case SemanticCumulativeKind::WINDOW:
		// anchor - window < date
		children.push_back(anchor->Copy());
		children.push_back(cumulative.window->Copy());
		start = Compare(ExpressionType::COMPARE_GREATERTHAN, date->Copy(), Call("-", std::move(children), true));
		break;
	case SemanticCumulativeKind::GRAIN:
		// date_trunc(part, anchor) <= date
		children.push_back(make_uniq<ConstantExpression>(Value(cumulative.part)));
		children.push_back(anchor->Copy());
		start = Compare(ExpressionType::COMPARE_GREATERTHANOREQUALTO, date->Copy(),
		                Call("date_trunc", std::move(children)));
		break;
	default:
		break;
	}
	vector<unique_ptr<ParsedExpression>> conditions;
	conditions.push_back(Compare(ExpressionType::COMPARE_LESSTHANOREQUALTO, std::move(date), std::move(anchor)));
	if (start) {
		conditions.push_back(std::move(start));
	}
	return AndAll(std::move(conditions));
}

bool SemanticIsTruncationPart(const string &part) {
	DatePartSpecifier specifier;
	if (!TryGetDatePartSpecifier(part, specifier)) {
		return false;
	}
	switch (specifier) {
	case DatePartSpecifier::MILLENNIUM:
	case DatePartSpecifier::CENTURY:
	case DatePartSpecifier::DECADE:
	case DatePartSpecifier::YEAR:
	case DatePartSpecifier::QUARTER:
	case DatePartSpecifier::MONTH:
	case DatePartSpecifier::WEEK:
	case DatePartSpecifier::ISOYEAR:
	case DatePartSpecifier::DAY:
	case DatePartSpecifier::HOUR:
	case DatePartSpecifier::MINUTE:
	case DatePartSpecifier::SECOND:
	case DatePartSpecifier::MILLISECONDS:
	case DatePartSpecifier::MICROSECONDS:
		return true;
	default:
		return false;
	}
}

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
	return Opaque(offsets.Column(table, column, std::move(compiled)));
}

unique_ptr<ParsedExpression> SemanticAnchorMap::Opaque(unique_ptr<ParsedExpression> compiled) {
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
		group_by(leaf->alias);
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
unique_ptr<ParsedExpression> AnchorJoinCondition(const SemanticCumulative &cumulative) {
	vector<unique_ptr<ParsedExpression>> conditions;
	conditions.push_back(
	    SemanticCumulativeWindow(cumulative, ColumnRef(CUMULATIVE_DATE_COLUMN), ColumnRef(CUMULATIVE_ANCHOR_COLUMN)));
	conditions.push_back(Compare(ExpressionType::COMPARE_LESSTHANOREQUALTO, ColumnRef(CUMULATIVE_ANCHOR_COLUMN),
	                             ColumnRef(CUMULATIVE_BOUND_COLUMN)));
	return AndAll(std::move(conditions));
}

static constexpr const char *DATED_CTE = "__sv_dated";
static constexpr const char *ANCHORS_CTE = "__sv_anchors";

unique_ptr<TableRef> CteRef(const string &name) {
	auto ref = make_uniq<BaseTableRef>();
	ref->table_name = name;
	ref->alias = name;
	return std::move(ref);
}

unique_ptr<CommonTableExpressionInfo> Cte(unique_ptr<QueryNode> query, CTEMaterialize materialized) {
	auto cte = make_uniq<CommonTableExpressionInfo>();
	cte->query = MakeSelectStatement(std::move(query));
	cte->materialized = materialized;
	return cte;
}

//! The time spine's anchors: every day from the dated rows' minimum to their maximum, inclusive
//! (SELECT CAST(unnest(generate_series(lo, hi, INTERVAL 1 DAY)) AS DATE) FROM (SELECT min(c) lo, max(c) hi FROM dated))
unique_ptr<QueryNode> SpineAnchors() {
	auto bound = [](const string &aggregate, const string &alias) {
		vector<unique_ptr<ParsedExpression>> arguments;
		arguments.push_back(ColumnRef(CUMULATIVE_DATE_COLUMN));
		auto result = Call(aggregate, std::move(arguments));
		result->alias = alias;
		return result;
	};
	auto bounds = make_uniq<SelectNode>();
	bounds->select_list.push_back(bound("min", "lo"));
	bounds->select_list.push_back(bound("max", "hi"));
	bounds->from_table = CteRef(DATED_CTE);
	vector<unique_ptr<ParsedExpression>> series;
	series.push_back(ColumnRef("lo"));
	series.push_back(ColumnRef("hi"));
	series.push_back(make_uniq<ConstantExpression>(Value::INTERVAL(0, 1, 0)));
	vector<unique_ptr<ParsedExpression>> unnest;
	unnest.push_back(Call("generate_series", std::move(series)));
	auto node = make_uniq<SelectNode>();
	auto day = make_uniq<CastExpression>(LogicalType::DATE, Call("unnest", std::move(unnest)));
	day->alias = CUMULATIVE_ANCHOR_COLUMN;
	node->select_list.push_back(std::move(day));
	node->from_table = MakeSubqueryRef(std::move(bounds), "__sv_bounds");
	node->where_clause = make_uniq<OperatorExpression>(ExpressionType::OPERATOR_IS_NOT_NULL, ColumnRef("lo"));
	return std::move(node);
}

//! The anchor dates: SELECT DISTINCT c AS __sv_t FROM <c's table> WHERE c IS NOT NULL, or the time spine's days
unique_ptr<QueryNode> Anchors(const SemanticModel &model, SemanticAliasGenerator &aliases,
                              optional_ptr<const SemanticTableSources> sources, const SemanticCumulative &cumulative) {
	if (model.IsTimeSpine(cumulative.table)) {
		return SpineAnchors();
	}
	SemanticJoinScope scope(model, aliases, cumulative.table, {}, aliases.NestedPrefix(), sources);
	auto node = make_uniq<SelectNode>();
	auto anchor = scope.ColumnReference(cumulative.table, cumulative.column);
	node->where_clause = make_uniq<OperatorExpression>(ExpressionType::OPERATOR_IS_NOT_NULL, anchor->Copy());
	anchor->alias = CUMULATIVE_ANCHOR_COLUMN;
	node->select_list.push_back(std::move(anchor));
	node->modifiers.push_back(make_uniq<DistinctModifier>());
	node->from_table = scope.BuildFrom();
	return std::move(node);
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

//! Stage 4's column of aggregate `k`
string ValueColumn(idx_t k) {
	return "__sv_v" + to_string(k);
}

} // namespace

SemanticCumulativeBlock BuildSemanticCumulativeBlock(ClientContext &context, const SemanticModel &model,
                                                     SemanticAliasGenerator &aliases,
                                                     optional_ptr<const SemanticTableSources> sources,
                                                     const SemanticCumulative &cumulative,
                                                     SemanticCumulativeParts parts) {
	// stage 1: the rows read the date and the leaves
	parts.date->alias = CUMULATIVE_DATE_COLUMN;
	parts.rows->select_list.push_back(std::move(parts.date));
	for (auto &leaf : parts.leaves) {
		parts.rows->select_list.push_back(leaf->Copy());
	}
	// one anchor per dimension tuple when a dimension is the anchor itself (not shifted by an OFFSET): no period
	auto anchor = ColumnRef(CUMULATIVE_ANCHOR_COLUMN);
	auto one_anchor =
	    std::any_of(parts.dimensions.begin(), parts.dimensions.end(),
	                [&](const unique_ptr<ParsedExpression> &dimension) { return dimension->Equals(*anchor); });
	auto dimension_count = parts.dimensions.size();
	auto value_count = parts.aggregates.size();
	// stage 2: the pre-aggregated or raw rows, with the latest date
	auto dated = PreAggregate(context, parts);
	if (!dated) {
		dated = BoundRows(std::move(parts.rows));
	}
	// stage 3: joined to the anchors whose window holds their date. The time spine's anchors read the dated rows
	// too, which are then materialized so that the fact table is scanned once.
	auto join = make_uniq<JoinRef>(JoinRefType::REGULAR);
	join->type = JoinType::INNER;
	join->left = CteRef(DATED_CTE);
	join->right = CteRef(ANCHORS_CTE);
	join->condition = AnchorJoinCondition(cumulative);
	// stage 4: per dimension tuple and anchor
	auto anchored = make_uniq<SelectNode>();
	for (idx_t d = 0; d < dimension_count; d++) {
		anchored->groups.group_expressions.push_back(parts.dimensions[d]->Copy());
		parts.dimensions[d]->alias = SemanticDimensionColumn(d);
		anchored->select_list.push_back(std::move(parts.dimensions[d]));
	}
	anchored->groups.group_expressions.push_back(ColumnRef(CUMULATIVE_ANCHOR_COLUMN));
	anchored->select_list.push_back(ColumnRef(CUMULATIVE_ANCHOR_COLUMN));
	for (idx_t k = 0; k < value_count; k++) {
		parts.aggregates[k]->alias = ValueColumn(k);
		anchored->select_list.push_back(std::move(parts.aggregates[k]));
	}
	anchored->where_clause = std::move(parts.anchor_where);
	anchored->from_table = std::move(join);
	auto dated_materialized = model.IsTimeSpine(cumulative.table) ? CTEMaterialize::CTE_MATERIALIZE_ALWAYS
	                                                              : CTEMaterialize::CTE_MATERIALIZE_NEVER;
	anchored->cte_map.map.insert(DATED_CTE, Cte(std::move(dated), dated_materialized));
	anchored->cte_map.map.insert(
	    ANCHORS_CTE, Cte(Anchors(model, aliases, sources, cumulative), CTEMaterialize::CTE_MATERIALIZE_NEVER));
	GroupByAll(*anchored);
	// stage 5: per dimension tuple
	SemanticCumulativeBlock result;
	result.node = make_uniq<SelectNode>();
	for (idx_t d = 0; d < dimension_count; d++) {
		result.node->select_list.push_back(ColumnRef(SemanticDimensionColumn(d)));
		if (!one_anchor) {
			result.node->groups.group_expressions.push_back(ColumnRef(SemanticDimensionColumn(d)));
		}
	}
	for (idx_t k = 0; k < value_count; k++) {
		result.values.push_back(one_anchor ? ColumnRef(ValueColumn(k)) : Period(cumulative.period, ValueColumn(k)));
	}
	result.node->from_table = MakeSubqueryRef(std::move(anchored), "__sv_anchored");
	GroupByAll(*result.node);
	return result;
}

} // namespace duckdb
