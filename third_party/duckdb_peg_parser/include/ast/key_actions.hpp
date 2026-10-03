#pragma once
#include "duckdb/common/enums/join_type.hpp"
#include "duckdb/common/enums/joinref_type.hpp"

namespace duckdb {
namespace semantic_views_peg {
struct KeyActions {
	string update_action;
	string delete_action;
};
} // namespace semantic_views_peg
} // namespace duckdb
