#pragma once
#include "duckdb/parser/parsed_expression.hpp"

namespace duckdb {
namespace semantic_views_peg {
struct DistinctClause {
	bool is_distinct;
	vector<unique_ptr<ParsedExpression>> distinct_targets;
};
} // namespace semantic_views_peg
} // namespace duckdb
