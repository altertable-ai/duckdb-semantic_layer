#pragma once
#include "duckdb/common/common.hpp"

namespace duckdb {
namespace semantic_views_peg {
struct AnalyzeTarget {
	unique_ptr<TableRef> ref;
	vector<string> columns;
};
} // namespace semantic_views_peg
} // namespace duckdb
