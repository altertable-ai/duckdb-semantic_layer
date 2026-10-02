#pragma once
#include "duckdb/parser/statement/select_statement.hpp"

namespace duckdb {
namespace semantic_views_peg {
struct InsertValues {
	bool default_values = false;
	unique_ptr<SelectStatement> select_statement;
};
} // namespace semantic_views_peg
} // namespace duckdb
