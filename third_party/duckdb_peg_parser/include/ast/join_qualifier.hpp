#pragma once
#include "duckdb/parser/parsed_expression.hpp"

namespace duckdb {
namespace semantic_views_peg {
struct JoinQualifier {
	unique_ptr<ParsedExpression> on_clause;
	vector<string> using_columns;
};

} // namespace semantic_views_peg
} // namespace duckdb
