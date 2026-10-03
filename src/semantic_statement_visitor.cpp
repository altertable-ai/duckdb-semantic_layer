#include "semantic_statement_visitor.hpp"

#include "duckdb/parser/expression/subquery_expression.hpp"
#include "duckdb/parser/parsed_data/create_table_info.hpp"
#include "duckdb/parser/parsed_expression_iterator.hpp"
#include "duckdb/parser/query_node/set_operation_node.hpp"
#include "duckdb/parser/statement/create_statement.hpp"
#include "duckdb/parser/statement/explain_statement.hpp"
#include "duckdb/parser/statement/insert_statement.hpp"
#include "duckdb/parser/statement/prepare_statement.hpp"
#include "duckdb/parser/statement/select_statement.hpp"
#include "duckdb/parser/tableref/joinref.hpp"
#include "duckdb/parser/tableref/showref.hpp"
#include "duckdb/parser/tableref/subqueryref.hpp"

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
		if (create.info && create.info->type == CatalogType::TABLE_ENTRY) {
			auto &info = create.info->Cast<CreateTableInfo>();
			if (info.query) {
				VisitNode(*info.query->node);
			}
		}
		break;
	}
	case StatementType::INSERT_STATEMENT: {
		auto &insert = statement.Cast<InsertStatement>();
		if (insert.select_statement) {
			VisitNode(*insert.select_statement->node);
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
	case_insensitive_set_t ctes;
	for (auto &cte : node.cte_map.map) {
		ctes.insert(cte.first);
	}
	cte_scopes.push_back(std::move(ctes));
	for (auto &cte : node.cte_map.map) {
		VisitNode(*cte.second->query->node);
	}
	switch (node.type) {
	case QueryNodeType::SELECT_NODE: {
		auto &select = node.Cast<SelectNode>();
		if (select.from_table) {
			VisitRef(select.from_table, false);
		}
		for (auto &expr : select.select_list) {
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

void SemanticStatementVisitor::VisitExpression(unique_ptr<ParsedExpression> &expr) {
	if (expr->GetExpressionClass() == ExpressionClass::SUBQUERY) {
		VisitNode(*expr->Cast<SubqueryExpression>().subquery->node);
	}
	ParsedExpressionIterator::EnumerateChildren(*expr,
	                                            [&](unique_ptr<ParsedExpression> &child) { VisitExpression(child); });
}

} // namespace duckdb
