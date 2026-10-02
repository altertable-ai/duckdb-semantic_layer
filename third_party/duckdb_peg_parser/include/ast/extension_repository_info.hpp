#pragma once
#include "duckdb/common/string.hpp"

namespace duckdb {
namespace semantic_views_peg {
struct ExtensionRepositoryInfo {
	string name;
	bool repository_is_alias = false;
};
} // namespace semantic_views_peg
} // namespace duckdb
