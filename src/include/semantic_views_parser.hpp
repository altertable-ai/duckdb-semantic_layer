//===----------------------------------------------------------------------===//
//                         DuckDB - semantic_views
//
// semantic_views_parser.hpp
//
// ParserExtension that runs the vendored PEG parser for statements that use
// semantic-view syntax and lowers them onto native DuckDB AST nodes.
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/main/client_context_state.hpp"
#include "duckdb/parser/parser_extension.hpp"
#include "duckdb/parser/parser_options.hpp"
#include "duckdb/parser/sql_statement.hpp"
#include "duckdb/planner/operator_extension.hpp"
#include "parsed_semantic_view.hpp"

namespace duckdb {

class SemanticViewsRewriter {
public:
	//! Whether the statement uses semantic-view syntax (ignoring comments and string literals)
	static bool ContainsSemanticSyntax(const string &sql);
	//! Parses `sql` with the vendored PEG parser (all statements).
	static vector<unique_ptr<SQLStatement>> Parse(const string &sql, ParserOptions options);
	//! Parses a canonical `CREATE SEMANTIC VIEW` statement into its definition
	static unique_ptr<ParsedSemanticView> ParseDefinition(const string &sql);
};

//! Parse data for the parse_function path (postgres failed on semantic-view syntax).
struct SemanticViewsParseData : public ParserExtensionParseData {
	unique_ptr<SQLStatement> statement;

	unique_ptr<ParserExtensionParseData> Copy() const override;
	string ToString() const override;
};

class SemanticViewsParserExtension : public ParserExtension {
public:
	SemanticViewsParserExtension();

	static ParserExtensionParseResult ParseFunction(ParserExtensionInfo *info, const string &query);
	static ParserExtensionPlanResult PlanFunction(ParserExtensionInfo *info, ClientContext &context,
	                                              unique_ptr<ParserExtensionParseData> parse_data);
};

//! plan_function cannot return an arbitrary SQLStatement: it stores the rewritten statement here and
//! throws; SemanticViewsOperatorExtension::Bind consumes it from the same ClientContext and rebinds it.
class SemanticViewsPendingBindState : public ClientContextState {
public:
	void Set(unique_ptr<SQLStatement> statement);
	unique_ptr<SQLStatement> Take();
	void QueryEnd() override;

private:
	mutex lock;
	unique_ptr<SQLStatement> pending;
};

struct SemanticViewsPendingBind {
	static void Set(ClientContext &context, unique_ptr<SQLStatement> statement);
	static unique_ptr<SQLStatement> Take(ClientContext &context);
};

class SemanticViewsOperatorExtension : public OperatorExtension {
public:
	SemanticViewsOperatorExtension();

	std::string GetName() override {
		return "semantic_views";
	}
	unique_ptr<LogicalExtensionOperator> Deserialize(Deserializer &deserializer) override;
};

} // namespace duckdb
