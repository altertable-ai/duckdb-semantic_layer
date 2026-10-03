#include "semantic_views_parser.hpp"

#include "duckdb/common/error_data.hpp"
#include "duckdb/common/exception.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/main/client_context.hpp"
#include "duckdb/main/query_profiler.hpp"
#include "duckdb/parser/statement/create_statement.hpp"
#include "duckdb/parser/parser.hpp"
#include "duckdb/parser/statement/explain_statement.hpp"
#include "duckdb/planner/binder.hpp"
#include "matcher.hpp"
#include "semantic_sql_rewrite.hpp"
#include "semantic_view_registry.hpp"
#include "tokenizer.hpp"
#include "transformer/peg_transformer.hpp"

namespace duckdb {

using semantic_views_peg::BaseTokenizer;
using semantic_views_peg::MatcherToken;
using semantic_views_peg::PEGTransformerFactory;
using semantic_views_peg::TokenizeState;

//! Statement-splitting tokenizer for the PEG parser (same role as autocomplete's ParserTokenizer).
class SemanticViewsTokenizer : public BaseTokenizer {
public:
	SemanticViewsTokenizer(const string &sql, vector<MatcherToken> &tokens) : BaseTokenizer(sql, tokens) {
	}
	void OnStatementEnd(idx_t pos) override {
		statements.push_back(std::move(tokens));
		tokens.clear();
	}
	void OnLastToken(TokenizeState state, string last_word, idx_t last_pos) override {
		if (last_word.empty()) {
			return;
		}
		tokens.emplace_back(std::move(last_word), last_pos);
	}

	vector<vector<MatcherToken>> statements;
};

static vector<vector<MatcherToken>> TokenizeStatements(const string &sql) {
	vector<MatcherToken> root_tokens;
	SemanticViewsTokenizer tokenizer(sql, root_tokens);
	tokenizer.TokenizeInput();
	tokenizer.statements.push_back(std::move(root_tokens));
	return std::move(tokenizer.statements);
}

bool SemanticViewsRewriter::ContainsSemanticSyntax(const string &sql) {
	if (!StringUtil::Contains(StringUtil::Lower(sql), "semantic")) {
		return false;
	}
	for (auto &tokens : TokenizeStatements(sql)) {
		for (idx_t i = 0; i + 1 < tokens.size(); i++) {
			auto &text = tokens[i].text;
			auto &next = tokens[i + 1].text;
			if (StringUtil::CIEquals(text, "semantic_view") && next == "(") {
				return true;
			}
			if (StringUtil::CIEquals(text, "semantic") &&
			    (StringUtil::CIEquals(next, "view") || StringUtil::CIEquals(next, "views") ||
			     StringUtil::CIEquals(next, "dimensions") || StringUtil::CIEquals(next, "metrics") ||
			     StringUtil::CIEquals(next, "facts"))) {
				return true;
			}
		}
	}
	return false;
}

vector<unique_ptr<SQLStatement>> SemanticViewsRewriter::Parse(const string &query, ParserOptions options) {
	// Avoid recursive overrides while transforming.
	options.parser_override_setting = AllowParserOverride::DEFAULT_OVERRIDE;
	options.extensions = nullptr;

	vector<unique_ptr<SQLStatement>> result;
	for (auto &tokenized_statement : TokenizeStatements(query)) {
		if (tokenized_statement.empty()) {
			continue;
		}
		auto &transformer = PEGTransformerFactory::GetInstance();
		auto statement = transformer.Transform(tokenized_statement, options);
		if (!statement) {
			continue;
		}
		statement->stmt_location = NumericCast<idx_t>(tokenized_statement[0].offset);
		auto &last = tokenized_statement.back();
		statement->stmt_length = last.offset + last.length - tokenized_statement[0].offset;
		result.push_back(std::move(statement));
	}
	if (!result.empty()) {
		result.back()->stmt_length = query.size() - result.back()->stmt_location;
		for (auto &statement : result) {
			statement->query = query.substr(statement->stmt_location, statement->stmt_length);
			statement->stmt_location = 0;
			statement->stmt_length = statement->query.size();
			if (statement->type == StatementType::CREATE_STATEMENT) {
				auto &create = statement->Cast<CreateStatement>();
				create.info->sql = statement->query;
			}
		}
	}
	return result;
}

//===--------------------------------------------------------------------===//
// Parse data
//===--------------------------------------------------------------------===//
unique_ptr<ParserExtensionParseData> SemanticViewsParseData::Copy() const {
	auto result = make_uniq<SemanticViewsParseData>();
	if (statement) {
		result->statement = statement->Copy();
	}
	return std::move(result);
}

string SemanticViewsParseData::ToString() const {
	return statement ? statement->ToString() : string();
}

//===--------------------------------------------------------------------===//
// Pending bind handoff
//===--------------------------------------------------------------------===//
void SemanticViewsPendingBindState::Set(unique_ptr<SQLStatement> statement) {
	lock_guard<mutex> guard(lock);
	pending = std::move(statement);
}

unique_ptr<SQLStatement> SemanticViewsPendingBindState::Take() {
	lock_guard<mutex> guard(lock);
	return std::move(pending);
}

void SemanticViewsPendingBindState::QueryEnd() {
	lock_guard<mutex> guard(lock);
	pending.reset();
}

static SemanticViewsPendingBindState &GetPendingBindState(ClientContext &context) {
	return *context.registered_state->GetOrCreate<SemanticViewsPendingBindState>("semantic_views_pending_bind");
}

void SemanticViewsPendingBind::Set(ClientContext &context, unique_ptr<SQLStatement> statement) {
	GetPendingBindState(context).Set(std::move(statement));
}

unique_ptr<SQLStatement> SemanticViewsPendingBind::Take(ClientContext &context) {
	return GetPendingBindState(context).Take();
}

//===--------------------------------------------------------------------===//
// Parser extension
//===--------------------------------------------------------------------===//
SemanticViewsParserExtension::SemanticViewsParserExtension() {
	parse_function = SemanticViewsParserExtension::ParseFunction;
	plan_function = SemanticViewsParserExtension::PlanFunction;
}

ParserExtensionParseResult SemanticViewsParserExtension::ParseFunction(ParserExtensionInfo *info, const string &query) {
	// Only invoked when the postgres parser failed on this statement.
	if (!SemanticViewsRewriter::ContainsSemanticSyntax(query)) {
		return ParserExtensionParseResult();
	}
	try {
		ParserOptions options;
		auto statements = SemanticViewsRewriter::Parse(query, options);
		if (statements.size() != 1) {
			return ParserExtensionParseResult("semantic_views: expected a single statement");
		}
		auto data = make_uniq<SemanticViewsParseData>();
		data->statement = std::move(statements[0]);
		return ParserExtensionParseResult(std::move(data));
	} catch (std::exception &ex) {
		return ParserExtensionParseResult(ErrorData(ex).RawMessage());
	}
}

ParserExtensionPlanResult SemanticViewsParserExtension::PlanFunction(ParserExtensionInfo *, ClientContext &context,
                                                                     unique_ptr<ParserExtensionParseData> parse_data) {
	// ExtensionStatement planning only supports table-function results. Stash the rewritten AST and
	// fail bind so SemanticViewsOperatorExtension can rebind it as a regular statement.
	auto &data = static_cast<SemanticViewsParseData &>(*parse_data);
	if (!data.statement) {
		throw InternalException("semantic_views plan_function: missing rewritten statement");
	}
	SemanticViewsPendingBind::Set(context, std::move(data.statement));
	throw BinderException("semantic_views: rebind rewritten statement");
}

//===--------------------------------------------------------------------===//
// Operator extension (bind handoff)
//===--------------------------------------------------------------------===//
static void EnsureExplainAnalyzeProfiler(ClientContext &context, SQLStatement &statement) {
	if (statement.type != StatementType::EXPLAIN_STATEMENT) {
		return;
	}
	auto &explain = statement.Cast<ExplainStatement>();
	if (explain.explain_type != ExplainType::EXPLAIN_ANALYZE) {
		return;
	}
	// ClientContext::IsExplainAnalyze only inspects the outer (Extension) statement, so the profiler
	// was never armed; arm it here so the matching EndPhase(PLANNER) after CreatePlan is valid.
	auto &profiler = QueryProfiler::Get(context);
	const auto &query_text = !statement.query.empty() ? statement.query : statement.ToString();
	profiler.StartQuery(query_text, true);
	profiler.StartPhase(MetricType::PLANNER);
}

//===--------------------------------------------------------------------===//
// Rebind on planning errors (standard-SQL form)
//===--------------------------------------------------------------------===//
void SemanticViewsRebindState::Register(ClientContext &context, shared_ptr<SemanticViewsState> state) {
	context.registered_state->GetOrCreate<SemanticViewsRebindState>(KEY, std::move(state));
}

RebindQueryInfo SemanticViewsRebindState::OnPlanningError(ClientContext &context, SQLStatement &statement,
                                                          ErrorData &error) {
	if (error.Type() != ExceptionType::CATALOG && error.Type() != ExceptionType::BINDER) {
		return RebindQueryInfo::DO_NOT_REBIND;
	}
	if (!RewriteSemanticViewReferences(context, *state, statement)) {
		return RebindQueryInfo::DO_NOT_REBIND;
	}
	return RebindQueryInfo::ATTEMPT_TO_REBIND;
}

static BoundStatement SemanticViewsBindStatement(ClientContext &context, Binder &binder, OperatorExtensionInfo *info,
                                                 SQLStatement &) {
	auto &state = *static_cast<SemanticViewsOperatorInfo &>(*info).state;
	auto pending = SemanticViewsPendingBind::Take(context);
	if (pending) {
		// statement rewritten by the semantic-view parser (it may also query semantic views by name)
		RewriteSemanticViewReferences(context, state, *pending);
		EnsureExplainAnalyzeProfiler(context, *pending);
		return binder.Bind(*pending);
	}
	// unrelated binder failure (queries over semantic views by name are rewritten by SemanticViewsRebindState):
	// an empty plan makes DuckDB rethrow the original error
	return BoundStatement();
}

SemanticViewsOperatorExtension::SemanticViewsOperatorExtension(shared_ptr<SemanticViewsState> state) {
	Bind = SemanticViewsBindStatement;
	operator_info = make_shared_ptr<SemanticViewsOperatorInfo>(std::move(state));
}

unique_ptr<LogicalExtensionOperator> SemanticViewsOperatorExtension::Deserialize(Deserializer &) {
	throw NotImplementedException("semantic_views has no logical extension operators");
}

} // namespace duckdb
