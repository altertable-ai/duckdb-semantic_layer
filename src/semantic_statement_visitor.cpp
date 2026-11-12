#include "semantic_statement_visitor.hpp"

#include "duckdb/parser/expression/subquery_expression.hpp"
#include "duckdb/parser/parsed_data/create_table_info.hpp"
#include "duckdb/parser/parsed_expression_iterator.hpp"
#include "duckdb/parser/query_node/recursive_cte_node.hpp"
#include "duckdb/parser/query_node/set_operation_node.hpp"
#include "duckdb/function/scalar_macro_function.hpp"
#include "duckdb/function/table_macro_function.hpp"
#include "duckdb/parser/parsed_data/copy_info.hpp"
#include "duckdb/parser/parsed_data/create_macro_info.hpp"
#include "duckdb/parser/parsed_data/create_type_info.hpp"
#include "duckdb/parser/query_node/copy_query_node.hpp"
#include "duckdb/parser/query_node/delete_query_node.hpp"
#include "duckdb/parser/query_node/insert_query_node.hpp"
#include "duckdb/parser/query_node/merge_query_node.hpp"
#include "duckdb/parser/query_node/update_query_node.hpp"
#include "duckdb/parser/statement/copy_statement.hpp"
#include "duckdb/parser/statement/create_statement.hpp"
#include "duckdb/parser/statement/delete_statement.hpp"
#include "duckdb/parser/statement/insert_statement.hpp"
#include "duckdb/parser/statement/merge_into_statement.hpp"
#include "duckdb/parser/statement/multi_statement.hpp"
#include "duckdb/parser/statement/set_statement.hpp"
#include "duckdb/parser/statement/update_statement.hpp"
#include "duckdb/parser/tableref/pivotref.hpp"
#include "duckdb/parser/statement/explain_statement.hpp"
#include "duckdb/parser/statement/prepare_statement.hpp"
#include "duckdb/parser/statement/select_statement.hpp"
#include "duckdb/parser/tableref/expressionlistref.hpp"
#include "duckdb/parser/tableref/joinref.hpp"
#include "duckdb/parser/tableref/showref.hpp"
#include "duckdb/parser/tableref/subqueryref.hpp"
#include "duckdb/parser/tableref/table_function_ref.hpp"

namespace duckdb {

void SemanticStatementVisitor::VisitStatement(SQLStatement &statement) {
	switch (statement.type) {
	case StatementType::SELECT_STATEMENT:
		VisitNode(*statement.Cast<SelectStatement>().node);
		break;
	case StatementType::EXPLAIN_STATEMENT:
		VisitStatement(*statement.Cast<ExplainStatement>().stmt);
		break;
	case StatementType::PREPARE_STATEMENT:
		VisitStatement(*statement.Cast<PrepareStatement>().statement);
		break;
	case StatementType::CREATE_STATEMENT: {
		auto &create = statement.Cast<CreateStatement>();
		if (create.info) {
			VisitCreateInfo(*create.info);
		}
		break;
	}
	case StatementType::INSERT_STATEMENT: {
		auto &insert = statement.Cast<InsertStatement>();
		if (insert.node) {
			VisitNode(*insert.node);
		}
		break;
	}
	case StatementType::DELETE_STATEMENT: {
		auto &del = statement.Cast<DeleteStatement>();
		if (del.node) {
			VisitNode(*del.node);
		}
		break;
	}
	case StatementType::UPDATE_STATEMENT: {
		auto &update = statement.Cast<UpdateStatement>();
		if (update.node) {
			VisitNode(*update.node);
		}
		break;
	}
	case StatementType::MERGE_INTO_STATEMENT: {
		auto &merge = statement.Cast<MergeIntoStatement>();
		if (merge.node) {
			VisitNode(*merge.node);
		}
		break;
	}
	case StatementType::COPY_STATEMENT: {
		auto &copy = statement.Cast<CopyStatement>();
		if (copy.info) {
			VisitCopyInfo(*copy.info);
		}
		break;
	}
	case StatementType::SET_STATEMENT: {
		auto &set = statement.Cast<SetStatement>();
		if (set.set_type == SetType::SET) {
			auto &variable = set.Cast<SetVariableStatement>();
			if (variable.value) {
				VisitExpression(variable.value);
			}
		}
		break;
	}
	case StatementType::MULTI_STATEMENT:
		for (auto &child : statement.Cast<MultiStatement>().statements) {
			VisitStatement(*child);
		}
		break;
	default:
		break;
	}
}

void SemanticStatementVisitor::VisitCreateInfo(CreateInfo &info) {
	switch (info.type) {
	case CatalogType::TABLE_ENTRY: {
		auto &table = info.Cast<CreateTableInfo>();
		if (table.query) {
			VisitNode(*table.query->node);
		}
		break;
	}
	case CatalogType::MACRO_ENTRY:
	case CatalogType::TABLE_MACRO_ENTRY:
		for (auto &macro : info.Cast<CreateMacroInfo>().macros) {
			if (macro->type == MacroType::TABLE_MACRO) {
				VisitNode(*macro->Cast<TableMacroFunction>().query_node);
			} else if (macro->type == MacroType::SCALAR_MACRO) {
				VisitExpression(macro->Cast<ScalarMacroFunction>().expression);
			}
		}
		break;
	case CatalogType::TYPE_ENTRY: {
		auto &type = info.Cast<CreateTypeInfo>();
		if (type.query) {
			VisitStatement(*type.query);
		}
		break;
	}
	default:
		break;
	}
}

bool SemanticStatementVisitor::IsCTEName(const string &name) const {
	for (auto &scope : cte_scopes) {
		if (scope.count(name)) {
			return true;
		}
	}
	return false;
}

void SemanticStatementVisitor::VisitNode(QueryNode &node) {
	// a CTE sees the CTEs defined before it (and itself, when recursive), as in DuckDB
	cte_scopes.emplace_back();
	for (auto &cte : node.cte_map.map) {
		auto name = cte.first.GetIdentifierName();
		auto &query = *cte.second->query_node;
		if (query.type == QueryNodeType::RECURSIVE_CTE_NODE) {
			cte_scopes.back().insert(name);
		}
		VisitNode(query);
		cte_scopes.back().insert(name);
	}
	ParsedExpressionIterator::EnumerateQueryNodeModifiers(
	    node, [&](unique_ptr<ParsedExpression> &expr) { VisitExpression(expr); });
	switch (node.type) {
	case QueryNodeType::SELECT_NODE: {
		auto &select = node.Cast<SelectNode>();
		EnterSelect(select);
		if (select.from_table) {
			VisitRef(select.from_table, false);
		}
		for (auto &expr : select.select_list) {
			VisitExpression(expr);
		}
		for (auto &expr : select.groups.group_expressions) {
			VisitExpression(expr);
		}
		for (auto *clause : {&select.where_clause, &select.having, &select.qualify}) {
			if (*clause) {
				VisitExpression(*clause);
			}
		}
		VisitSelect(select);
		break;
	}
	case QueryNodeType::SET_OPERATION_NODE:
		for (auto &child : node.Cast<SetOperationNode>().children) {
			VisitNode(*child);
		}
		break;
	case QueryNodeType::INSERT_QUERY_NODE: {
		auto &insert = node.Cast<InsertQueryNode>();
		if (insert.select_statement) {
			VisitNode(*insert.select_statement->node);
		}
		if (insert.on_conflict_info) {
			VisitSetInfo(insert.on_conflict_info->set_info);
			if (insert.on_conflict_info->condition) {
				VisitExpression(insert.on_conflict_info->condition);
			}
		}
		VisitExpressions(insert.returning_list);
		break;
	}
	case QueryNodeType::DELETE_QUERY_NODE: {
		auto &del = node.Cast<DeleteQueryNode>();
		for (auto &using_clause : del.using_clauses) {
			VisitRef(using_clause, true);
		}
		if (del.condition) {
			VisitExpression(del.condition);
		}
		VisitExpressions(del.returning_list);
		break;
	}
	case QueryNodeType::UPDATE_QUERY_NODE: {
		auto &update = node.Cast<UpdateQueryNode>();
		if (update.from_table) {
			VisitRef(update.from_table, true);
		}
		VisitSetInfo(update.set_info);
		VisitExpressions(update.returning_list);
		break;
	}
	case QueryNodeType::MERGE_QUERY_NODE: {
		auto &merge = node.Cast<MergeQueryNode>();
		if (merge.source) {
			VisitRef(merge.source, true);
		}
		if (merge.join_condition) {
			VisitExpression(merge.join_condition);
		}
		for (auto &entry : merge.actions) {
			for (auto &action : entry.second) {
				if (action->condition) {
					VisitExpression(action->condition);
				}
				VisitSetInfo(action->update_info);
				VisitExpressions(action->expressions);
			}
		}
		VisitExpressions(merge.returning_list);
		break;
	}
	case QueryNodeType::COPY_QUERY_NODE:
		if (node.Cast<CopyQueryNode>().info) {
			VisitCopyInfo(*node.Cast<CopyQueryNode>().info);
		}
		break;
	case QueryNodeType::RECURSIVE_CTE_NODE: {
		auto &recursive = node.Cast<RecursiveCTENode>();
		VisitNode(*recursive.left);
		VisitNode(*recursive.right);
		for (auto &expr : recursive.key_targets) {
			VisitExpression(expr);
		}
		break;
	}
	default:
		break;
	}
	cte_scopes.pop_back();
}

void SemanticStatementVisitor::VisitRef(unique_ptr<TableRef> &ref, bool in_join) {
	switch (ref->type) {
	case TableReferenceType::JOIN: {
		auto &join = ref->Cast<JoinRef>();
		VisitRef(join.left, true);
		VisitRef(join.right, true);
		if (join.condition) {
			VisitExpression(join.condition);
		}
		break;
	}
	case TableReferenceType::SUBQUERY:
		VisitNode(*ref->Cast<SubqueryRef>().subquery->node);
		break;
	case TableReferenceType::TABLE_FUNCTION:
		VisitExpression(ref->Cast<TableFunctionRef>().function);
		break;
	case TableReferenceType::EXPRESSION_LIST:
		for (auto &row : ref->Cast<ExpressionListRef>().values) {
			for (auto &expr : row) {
				VisitExpression(expr);
			}
		}
		break;
	case TableReferenceType::PIVOT: {
		auto &pivot = ref->Cast<PivotRef>();
		if (pivot.source) {
			VisitRef(pivot.source, false);
		}
		VisitExpressions(pivot.aggregates);
		for (auto &column : pivot.pivots) {
			VisitExpressions(column.pivot_expressions);
			if (column.subquery) {
				VisitNode(*column.subquery);
			}
		}
		break;
	}
	case TableReferenceType::SHOW_REF: {
		auto &show = ref->Cast<ShowRef>();
		if (show.query && !VisitShowQuery(show)) {
			VisitNode(*show.query);
		}
		break;
	}
	default:
		break;
	}
	VisitTableRef(ref, in_join);
}

void SemanticStatementVisitor::VisitCopyInfo(CopyInfo &info) {
	if (info.select_statement) {
		VisitNode(*info.select_statement);
	}
	if (info.file_path_expression) {
		VisitExpression(info.file_path_expression);
	}
	for (auto &option : info.parsed_options) {
		if (option.second) {
			VisitExpression(option.second);
		}
	}
}

void SemanticStatementVisitor::VisitSetInfo(unique_ptr<UpdateSetInfo> &info) {
	if (!info) {
		return;
	}
	VisitExpressions(info->expressions);
	if (info->condition) {
		VisitExpression(info->condition);
	}
}

void SemanticStatementVisitor::VisitExpressions(vector<unique_ptr<ParsedExpression>> &expressions) {
	for (auto &expr : expressions) {
		VisitExpression(expr);
	}
}

void SemanticStatementVisitor::VisitExpression(unique_ptr<ParsedExpression> &expr) {
	if (expr->GetExpressionClass() == ExpressionClass::SUBQUERY) {
		VisitNode(*expr->Cast<SubqueryExpression>().SubqueryMutable()->node);
	}
	ParsedExpressionIterator::EnumerateChildren(*expr,
	                                            [&](unique_ptr<ParsedExpression> &child) { VisitExpression(child); });
}

} // namespace duckdb
