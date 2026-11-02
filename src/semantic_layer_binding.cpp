#include "semantic_layer_binding.hpp"

#include "duckdb/common/error_data.hpp"
#include "duckdb/main/client_context.hpp"
#include "duckdb/main/client_context_state.hpp"
#include "duckdb/main/config.hpp"
#include "duckdb/main/connection_manager.hpp"
#include "duckdb/planner/extension_callback.hpp"
#include "semantic_sql_rewrite.hpp"
#include "semantic_layer_registry.hpp"
#include "semantic_layer_grammar.hpp"

namespace duckdb {

namespace {

//===--------------------------------------------------------------------===//
// Standard SQL over a semantic view by name: rewrite on planning errors
//===--------------------------------------------------------------------===//
//! DuckDB binds a copy of every statement first: when that fails, OnPlanningError receives the untouched statement
class SemanticViewsRebindState : public ClientContextState {
public:
	explicit SemanticViewsRebindState(shared_ptr<SemanticViewsState> state_p) : state(std::move(state_p)) {
	}

	static void Register(ClientContext &context, shared_ptr<SemanticViewsState> state) {
		context.registered_state->GetOrCreate<SemanticViewsRebindState>("semantic_layer_rebind", std::move(state));
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
	RegisterSemanticViewsGrammar(db);
	// the rebind state lives on every connection: the ones opened later, and those open when the extension loads
	ExtensionCallback::Register(config, make_shared_ptr<SemanticViewsConnectionCallback>(state));
	for (auto &connection : ConnectionManager::Get(db).GetConnectionList()) {
		SemanticViewsRebindState::Register(*connection, state);
	}
}

} // namespace duckdb
