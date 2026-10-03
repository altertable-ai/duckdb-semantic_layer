#pragma once

#include "duckdb/common/enums/set_scope.hpp"
#include "duckdb/common/string.hpp"
#include "duckdb/common/vector.hpp"

namespace duckdb {
namespace semantic_views_peg {

struct OnConflictExpressionTarget {
	vector<string> indexed_columns;
	unique_ptr<ParsedExpression> where_clause; // Default value is defined here
};

} // namespace semantic_views_peg
} // namespace duckdb
