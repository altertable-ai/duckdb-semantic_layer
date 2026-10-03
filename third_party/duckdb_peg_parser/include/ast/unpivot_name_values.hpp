#pragma once
#include "duckdb/parser/tableref/pivotref.hpp"

namespace duckdb {
namespace semantic_views_peg {
struct UnpivotNameValues {
	vector<string> unpivot_names;
	PivotColumn column;
};
} // namespace semantic_views_peg
} // namespace duckdb
