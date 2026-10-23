//===----------------------------------------------------------------------===//
//                         DuckDB - semantic_views
//
// semantic_views_grammar.hpp
//
// Semantic-view syntax as a grammar extension of DuckDB's PEG parser:
// CREATE / DROP / ALTER / DESCRIBE / SHOW SEMANTIC ..., SEMANTIC_VIEW(...)
// and (SHOW SEMANTIC ...) table references, and PARTITION BY EXCLUDING.
// Statements come out lowered onto native DuckDB AST nodes
// (semantic_lowering.hpp).
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/mutex.hpp"
#include "duckdb/parser/grammar_extension.hpp"
#include "duckdb/parser/parser_options.hpp"
#include "duckdb/parser/peg/compiled_grammar.hpp"
#include "duckdb/parser/sql_statement.hpp"

namespace duckdb {

class ClientContext;

//! Registered per database as the "semantic_views" grammar extension and
//! activated on every connection
class SemanticViewsGrammarExtension final : public GrammarExtension {
public:
	static constexpr const char *NAME = "semantic_views";

	SemanticViewsGrammarExtension();

	vector<GrammarChange> GetChanges() const override;

	//! DuckDB's default grammar extended with only this extension (compiled once,
	//! shared by connections)
	shared_ptr<CompiledGrammar> GetCompiledGrammar();

	//! Adds this extension to the connection's active grammar extensions
	void Activate(ClientContext &context);

	//! Parses every statement of `sql` with this grammar, whether or not the
	//! connection has it active
	static vector<unique_ptr<SQLStatement>> Parse(ClientContext &context, const string &sql);

private:
	//! The grammar for a set of active extensions, compiled once per set:
	//! connections that activate the same extensions share it instead of each
	//! compiling their own
	shared_ptr<CompiledGrammar> GetCompiledGrammar(ClientContext &context, const vector<string> &active);

	mutex grammar_lock;
	case_insensitive_map_t<shared_ptr<CompiledGrammar>> compiled_grammars;
};

void RegisterSemanticViewsGrammar(DatabaseInstance &db);

} // namespace duckdb
