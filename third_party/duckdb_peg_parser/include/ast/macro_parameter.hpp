#pragma once
#include "duckdb/parser/parsed_expression.hpp"

namespace duckdb {
namespace semantic_views_peg {
struct MacroParameter {
	unique_ptr<ParsedExpression> expression;
	string name;
	LogicalType type = LogicalType::UNKNOWN;
	bool is_default = false;
};

} // namespace semantic_views_peg
} // namespace duckdb
