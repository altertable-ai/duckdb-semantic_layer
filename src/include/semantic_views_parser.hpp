//===----------------------------------------------------------------------===//
//                         DuckDB - semantic_views
//
// semantic_views_parser.hpp
//
// Runs the vendored PEG parser (third_party/duckdb_peg_parser), whose grammar
// is extended with semantic-view syntax. Semantic-view statements come out
// lowered onto native DuckDB AST nodes (semantic_lowering.hpp).
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/parser/parser_options.hpp"
#include "duckdb/parser/sql_statement.hpp"

namespace duckdb {

class SemanticViewsParser {
public:
	//! Whether the SQL uses semantic-view syntax (ignoring comments and string literals)
	static bool ContainsSemanticSyntax(const string &sql);
	//! Parses every statement of `sql`
	static vector<unique_ptr<SQLStatement>> Parse(const string &sql, ParserOptions options);
};

} // namespace duckdb
