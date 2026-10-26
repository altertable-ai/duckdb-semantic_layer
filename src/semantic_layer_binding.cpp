#include "semantic_layer_binding.hpp"

#include "duckdb/common/error_data.hpp"
#include "duckdb/common/exception.hpp"
#include "duckdb/main/client_context.hpp"
#include "duckdb/main/client_context_state.hpp"
#include "duckdb/main/config.hpp"
#include "duckdb/main/connection_manager.hpp"
#include "duckdb/main/query_profiler.hpp"
#include "duckdb/parser/parser_extension.hpp"
#include "duckdb/parser/statement/explain_statement.hpp"
#include "duckdb/planner/binder.hpp"
#include "duckdb/planner/extension_callback.hpp"
#include "duckdb/planner/operator_extension.hpp"
#include "semantic_sql_rewrite.hpp"
#include "semantic_layer_registry.hpp"
#include "semantic_views_parser.hpp"

namespace duckdb {

namespace {

//===--------------------------------------------------------------------===//
// Semantic-view syntax: parser extension -> operator extension
//===--------------------------------------------------------------------===//
struct SemanticViewsParseData : public ParserExtensionParseData {
	unique_ptr<SQLStatement> statement;

	unique_ptr<ParserExtensionParseData> Copy() const override {
		auto result = make_uniq<SemanticViewsParseData>();
		result->statement = statement->Copy();
		return std::move(result);
	}
	string ToString() const override {
		return statement->ToString();
	}
};

//! plan_function cannot return an arbitrary statement: it stores the lowered statement here and fails the bind;
//! the operator extension's Bind takes it back from the same ClientContext and binds it.
class SemanticViewsPendingBind : public ClientContextState {
public:
	static void Set(ClientContext &context, unique_ptr<SQLStatement> statement) {
		Get(context).pending = std::move(statement);
	}
	static unique_ptr<SQLStatement> Take(ClientContext &context) {
		return std::move(Get(context).pending);
	}
	void QueryEnd() override {
		pending.reset();
	}

private:
	static SemanticViewsPendingBind &Get(ClientContext &context) {
		return *context.registered_state->GetOrCreate<SemanticViewsPendingBind>("semantic_views_pending_bind");
	}

	unique_ptr<SQLStatement> pending;
};

//! Only invoked for statements the postgres parser rejected
ParserExtensionParseResult ParseFunction(ParserExtensionInfo *, const string &query) {
	if (!SemanticViewsParser::ContainsSemanticSyntax(query)) {
		return ParserExtensionParseResult();
	}
	try {
		auto statements = SemanticViewsParser::Parse(query, ParserOptions());
		if (statements.size() != 1) {
			return ParserExtensionParseResult("semantic_layer: expected a single statement");
		}
		auto data = make_uniq<SemanticViewsParseData>();
		data->statement = std::move(statements[0]);
		return ParserExtensionParseResult(std::move(data));
	} catch (std::exception &ex) {
		return ParserExtensionParseResult(ErrorData(ex).RawMessage());
	}
}

ParserExtensionPlanResult PlanFunction(ParserExtensionInfo *, ClientContext &context,
                                       unique_ptr<ParserExtensionParseData> parse_data) {
	auto &data = static_cast<SemanticViewsParseData &>(*parse_data);
	SemanticViewsPendingBind::Set(context, std::move(data.statement));
	throw BinderException("semantic_layer: rebind rewritten statement");
}

//! ClientContext::IsExplainAnalyze only inspects the outer (extension) statement, so the profiler of an
//! EXPLAIN ANALYZE was never armed: arm it so that the EndPhase(PLANNER) following CreatePlan is valid.
void EnsureExplainAnalyzeProfiler(ClientContext &context, SQLStatement &statement) {
	if (statement.type != StatementType::EXPLAIN_STATEMENT ||
	    statement.Cast<ExplainStatement>().explain_type != ExplainType::EXPLAIN_ANALYZE) {
		return;
	}
	auto &profiler = QueryProfiler::Get(context);
	profiler.StartQuery(statement.query.empty() ? statement.ToString() : statement.query, true);
	profiler.StartPhase(MetricType::PLANNER);
}

struct SemanticViewsOperatorInfo : public OperatorExtensionInfo {
	explicit SemanticViewsOperatorInfo(shared_ptr<SemanticViewsState> state_p) : state(std::move(state_p)) {
	}
	shared_ptr<SemanticViewsState> state;
};

BoundStatement BindPendingStatement(ClientContext &context, Binder &binder, OperatorExtensionInfo *info,
                                    SQLStatement &) {
	auto pending = SemanticViewsPendingBind::Take(context);
	if (!pending) {
		// an unrelated binder failure: an empty plan makes DuckDB rethrow the original error
		return BoundStatement();
	}
	// the lowered statement may also query semantic views by name
	RewriteSemanticViewReferences(context, *static_cast<SemanticViewsOperatorInfo &>(*info).state, *pending);
	EnsureExplainAnalyzeProfiler(context, *pending);
	return binder.Bind(*pending);
}

class SemanticViewsOperatorExtension : public OperatorExtension {
public:
	explicit SemanticViewsOperatorExtension(shared_ptr<SemanticViewsState> state) {
		Bind = BindPendingStatement;
		operator_info = make_shared_ptr<SemanticViewsOperatorInfo>(std::move(state));
	}

	std::string GetName() override {
		return "semantic_layer";
	}
	unique_ptr<LogicalExtensionOperator> Deserialize(Deserializer &) override {
		throw NotImplementedException("semantic_layer has no logical extension operators");
	}
};

//===--------------------------------------------------------------------===//
// Standard SQL over a semantic view by name: rewrite on planning errors
//===--------------------------------------------------------------------===//
//! DuckDB binds a copy of every statement first: when that fails, OnPlanningError receives the untouched statement
class SemanticViewsRebindState : public ClientContextState {
public:
	explicit SemanticViewsRebindState(shared_ptr<SemanticViewsState> state_p) : state(std::move(state_p)) {
	}

	static void Register(ClientContext &context, shared_ptr<SemanticViewsState> state) {
		context.registered_state->GetOrCreate<SemanticViewsRebindState>("semantic_views_rebind", std::move(state));
	}

	bool CanRequestRebind() override {
		return true;
	}
	RebindQueryInfo OnPlanningError(ClientContext &context, SQLStatement &statement, ErrorData &error) override {
		if (error.Type() != ExceptionType::CATALOG && error.Type() != ExceptionType::BINDER) {
			return RebindQueryInfo::DO_NOT_REBIND;
		}
		return RewriteSemanticViewReferences(context, *state, statement) ? RebindQueryInfo::ATTEMPT_TO_REBIND
		                                                                 : RebindQueryInfo::DO_NOT_REBIND;
	}

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

} // namespace

void RegisterSemanticViewsBinding(DatabaseInstance &db, shared_ptr<SemanticViewsState> state) {
	auto &config = DBConfig::GetConfig(db);
	ParserExtension parser;
	parser.parse_function = ParseFunction;
	parser.plan_function = PlanFunction;
	ParserExtension::Register(config, parser);
	OperatorExtension::Register(config, make_shared_ptr<SemanticViewsOperatorExtension>(state));
	// the rebind state lives on every connection: the ones opened later, and those open when the extension loads
	ExtensionCallback::Register(config, make_shared_ptr<SemanticViewsConnectionCallback>(state));
	for (auto &connection : ConnectionManager::Get(db).GetConnectionList()) {
		SemanticViewsRebindState::Register(*connection, state);
	}
}

} // namespace duckdb
