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
#include "duckdb/planner/extension_callback.hpp"
#include "duckdb/planner/operator_extension.hpp"

namespace duckdb {

class SemanticViewsRewriter {
public:
	//! Whether the statement uses semantic-view syntax (ignoring comments and string literals)
	static bool ContainsSemanticSyntax(const string &sql);
	//! Parses `sql` with the vendored PEG parser (all statements).
	static vector<unique_ptr<SQLStatement>> Parse(const string &sql, ParserOptions options);
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

struct SemanticViewsState;

//! Lets DuckDB bind a copy of every statement first: when that fails, OnPlanningError receives the untouched
//! statement, rewrites `SELECT ... FROM <semantic view>` queries in place and asks for a rebind.
class SemanticViewsRebindState : public ClientContextState {
public:
	explicit SemanticViewsRebindState(shared_ptr<SemanticViewsState> state_p) : state(std::move(state_p)) {
	}
	bool CanRequestRebind() override {
		return true;
	}
	RebindQueryInfo OnPlanningError(ClientContext &context, SQLStatement &statement, ErrorData &error) override;

	static constexpr const char *KEY = "semantic_views_rebind";
	static void Register(ClientContext &context, shared_ptr<SemanticViewsState> state);

private:
	shared_ptr<SemanticViewsState> state;
};

class SemanticViewsConnectionCallback : public ExtensionCallback {
public:
	explicit SemanticViewsConnectionCallback(shared_ptr<SemanticViewsState> state_p) : state(std::move(state_p)) {
	}
	void OnConnectionOpened(ClientContext &context) override {
		SemanticViewsRebindState::Register(context, state);
	}

private:
	shared_ptr<SemanticViewsState> state;
};

struct SemanticViewsOperatorInfo : public OperatorExtensionInfo {
	explicit SemanticViewsOperatorInfo(shared_ptr<SemanticViewsState> state_p) : state(std::move(state_p)) {
	}
	shared_ptr<SemanticViewsState> state;
};

class SemanticViewsOperatorExtension : public OperatorExtension {
public:
	explicit SemanticViewsOperatorExtension(shared_ptr<SemanticViewsState> state);

	std::string GetName() override {
		return "semantic_views";
	}
	unique_ptr<LogicalExtensionOperator> Deserialize(Deserializer &deserializer) override;
};

} // namespace duckdb
