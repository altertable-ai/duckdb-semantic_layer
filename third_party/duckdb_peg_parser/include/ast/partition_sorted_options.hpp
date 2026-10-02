#pragma once
#include "duckdb/common/vector.hpp"
#include "duckdb/parser/parsed_expression.hpp"

namespace duckdb {
namespace semantic_views_peg {
struct PartitionSortedOptions {
	vector<unique_ptr<ParsedExpression>> partition_keys;
	vector<unique_ptr<ParsedExpression>> sort_keys;
};
} // namespace semantic_views_peg
} // namespace duckdb
