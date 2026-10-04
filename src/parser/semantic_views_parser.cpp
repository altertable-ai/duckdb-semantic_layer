#include "semantic_views_parser.hpp"

#include "duckdb/common/string_util.hpp"
#include "duckdb/parser/statement/create_statement.hpp"
#include "matcher.hpp"
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

bool SemanticViewsParser::ContainsSemanticSyntax(const string &sql) {
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
			     StringUtil::CIEquals(next, "facts") || StringUtil::CIEquals(next, "materializations"))) {
				return true;
			}
		}
	}
	return false;
}

vector<unique_ptr<SQLStatement>> SemanticViewsParser::Parse(const string &query, ParserOptions options) {
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

} // namespace duckdb
