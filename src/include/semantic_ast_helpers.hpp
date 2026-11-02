//===----------------------------------------------------------------------===//
//                         DuckDB - semantic_layer
//
// semantic_ast_helpers.hpp
//
// Small builders for the DuckDB parser AST nodes the compiler emits.
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/parser/expression/columnref_expression.hpp"
#include "duckdb/parser/expression/comparison_expression.hpp"
#include "duckdb/parser/expression/conjunction_expression.hpp"
#include "duckdb/parser/expression/constant_expression.hpp"
#include "duckdb/parser/expression/function_expression.hpp"
#include "duckdb/parser/expression/subquery_expression.hpp"
#include "duckdb/parser/query_node/select_node.hpp"
#include "duckdb/parser/statement/select_statement.hpp"
#include "duckdb/parser/tableref/subqueryref.hpp"

namespace duckdb {

inline unique_ptr<ParsedExpression> ColumnRef(const string &column, const string &table = string()) {
	return table.empty() ? make_uniq<ColumnRefExpression>(Identifier(column))
	                     : make_uniq<ColumnRefExpression>(Identifier(column), Identifier(table));
}

//! The value of a constant expression
inline Value ConstantValue(const ParsedExpression &expr) {
	return expr.Cast<ConstantExpression>().GetLiteral().ToValue();
}

//! The positional arguments of a function call
inline idx_t ArgumentCount(const FunctionExpression &function) {
	return function.GetArguments().size();
}
inline const ParsedExpression &Argument(const FunctionExpression &function, idx_t index) {
	return function.GetArguments()[index].GetExpression();
}
inline unique_ptr<ParsedExpression> &ArgumentMutable(FunctionExpression &function, idx_t index) {
	return function.GetArgumentsMutable()[index].GetExpressionMutable();
}

//! name(children...), or an operator such as "-" when `is_operator`
inline unique_ptr<ParsedExpression> Call(const string &name, vector<unique_ptr<ParsedExpression>> children,
                                         bool is_operator = false) {
	return make_uniq<FunctionExpression>(Identifier(name), std::move(children), nullptr, nullptr, false, is_operator);
}

inline unique_ptr<ParsedExpression> Compare(ExpressionType type, unique_ptr<ParsedExpression> left,
                                            unique_ptr<ParsedExpression> right) {
	return make_uniq<ComparisonExpression>(type, std::move(left), std::move(right));
}

//! Conjunction of `conditions`; nullptr when empty
inline unique_ptr<ParsedExpression> AndAll(vector<unique_ptr<ParsedExpression>> conditions) {
	unique_ptr<ParsedExpression> result;
	for (auto &condition : conditions) {
		result = result ? make_uniq<ConjunctionExpression>(ExpressionType::CONJUNCTION_AND, std::move(result),
		                                                   std::move(condition))
		                : std::move(condition);
	}
	return result;
}

//! Top-level AND conjuncts of `expr` (pointers into it)
inline void SplitConjuncts(const ParsedExpression &expr, vector<const ParsedExpression *> &conjuncts) {
	if (expr.GetExpressionType() == ExpressionType::CONJUNCTION_AND) {
		for (auto &child : expr.Cast<ConjunctionExpression>().GetChildren()) {
			SplitConjuncts(*child, conjuncts);
		}
		return;
	}
	conjuncts.push_back(&expr);
}

inline unique_ptr<SelectStatement> MakeSelectStatement(unique_ptr<QueryNode> node) {
	auto statement = make_uniq<SelectStatement>();
	statement->node = std::move(node);
	return statement;
}

inline unique_ptr<TableRef> MakeSubqueryRef(unique_ptr<QueryNode> node, const string &alias) {
	return make_uniq<SubqueryRef>(MakeSelectStatement(std::move(node)), Identifier(alias));
}

inline unique_ptr<ParsedExpression> MakeScalarSubquery(unique_ptr<QueryNode> node) {
	auto subquery = make_uniq<SubqueryExpression>();
	subquery->GetSubqueryTypeMutable() = SubqueryType::SCALAR;
	subquery->SubqueryMutable() = MakeSelectStatement(std::move(node));
	return std::move(subquery);
}

//! GROUP BY every expression in node.groups.group_expressions (a single grouping set)
inline void GroupByAll(SelectNode &node) {
	if (node.groups.group_expressions.empty()) {
		return;
	}
	GroupingSet set;
	for (idx_t i = 0; i < node.groups.group_expressions.size(); i++) {
		set.insert(ProjectionIndex(i));
	}
	node.groups.grouping_sets.push_back(std::move(set));
}

} // namespace duckdb
