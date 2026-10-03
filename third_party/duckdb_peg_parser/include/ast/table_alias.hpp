#pragma once

namespace duckdb {
namespace semantic_views_peg {
struct TableAlias {
	string name;
	vector<string> column_name_alias;
};
} // namespace semantic_views_peg
} // namespace duckdb
