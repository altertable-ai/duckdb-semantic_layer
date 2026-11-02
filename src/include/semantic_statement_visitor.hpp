//===----------------------------------------------------------------------===//
//                         DuckDB - semantic_layer
//
// semantic_statement_visitor.hpp
//
// Walks every query node and table reference of a statement - CTEs, joins,
// subqueries in FROM and in expressions, DESCRIBE / SHOW, EXPLAIN, PREPARE,
// CREATE TABLE AS and INSERT ... SELECT - so that subclasses can rewrite them.
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/case_insensitive_map.hpp"
#include "duckdb/parser/query_node/select_node.hpp"
#include "duckdb/parser/sql_statement.hpp"
#include "duckdb/parser/tableref/showref.hpp"

namespace duckdb {

class SemanticStatementVisitor {
public:
	virtual ~SemanticStatementVisitor() = default;

	void VisitStatement(SQLStatement &statement);

protected:
	//! Every table reference, after its children; may replace `ref`
	virtual void VisitTableRef(unique_ptr<TableRef> &ref, bool in_join) {
	}
	//! Every SELECT node, after its children
	virtual void VisitSelect(SelectNode &node) {
	}
	//! Every DESCRIBE / SHOW / SUMMARIZE of a query, before its query; returns true when it handled (replaced) the
	//! query
	virtual bool VisitShowQuery(ShowRef &show) {
		return false;
	}
	//! Whether `name` is a CTE visible from the node being visited
	bool IsCTEName(const string &name) const;

private:
	void VisitNode(QueryNode &node);
	void VisitRef(unique_ptr<TableRef> &ref, bool in_join);
	void VisitExpression(unique_ptr<ParsedExpression> &expr);

	vector<case_insensitive_set_t> cte_scopes;
};

} // namespace duckdb
