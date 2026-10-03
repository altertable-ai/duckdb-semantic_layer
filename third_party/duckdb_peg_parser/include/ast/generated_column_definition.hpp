#pragma once
#include "duckdb/common/common.hpp"

namespace duckdb {
namespace semantic_views_peg {
struct GeneratedColumnDefinition {
	unique_ptr<ParsedExpression> expr;
	bool virtual_column = false;
	bool default_column = false;
};

struct ConstraintColumnDefinition {
	ColumnDefinition column_definition;
	vector<pair<bool, ConstraintType>> constraint_types;
	vector<unique_ptr<Constraint>> constraints;
};
} // namespace semantic_views_peg
} // namespace duckdb
