#pragma once

#include "duckdb/common/enums/set_scope.hpp"
#include "duckdb/common/string.hpp"

namespace duckdb {
namespace semantic_views_peg {

struct SettingInfo {
	string name;
	SetScope scope = SetScope::AUTOMATIC; // Default value is defined here
};

} // namespace semantic_views_peg
} // namespace duckdb
