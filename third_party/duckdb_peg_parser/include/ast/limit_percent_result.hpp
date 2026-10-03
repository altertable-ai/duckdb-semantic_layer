#pragma once
#include "duckdb/parser/parsed_expression.hpp"

namespace duckdb {
namespace semantic_views_peg {

struct LimitPercentResult {
	bool is_percent = false;
	unique_ptr<ParsedExpression> expression;
};
} // namespace semantic_views_peg
} // namespace duckdb
