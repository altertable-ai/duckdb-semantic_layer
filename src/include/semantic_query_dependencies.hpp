#pragma once

#include "duckdb/common/set.hpp"
#include "duckdb/common/map.hpp"
#include "duckdb/common/string.hpp"
#include "duckdb/common/vector.hpp"

namespace duckdb {

//! A child aggregation's correlation, recorded when the compiler builds its subquery.
struct SemanticChildDependency {
	idx_t table;
	idx_t parent;
	vector<string> columns;
	vector<string> parent_columns;
};

//! Sources actually read while compiling a block, including expanded row-level members.
struct SemanticBlockDependencies {
	set<idx_t> tables;
	set<idx_t> nested_tables;
	vector<SemanticChildDependency> children;
	//! Number of source occurrences emitted by the compiler, checked after DuckDB expands macros.
	map<idx_t, idx_t> scans;
	//! Unaccounted reads, including explicit or macro-expanded subqueries, have unknown freshness.
	bool complete = true;
	bool in_dimension = false;
	string incremental_reason;

	void Unsupported(const string &reason) {
		if (incremental_reason.empty()) {
			incremental_reason = reason;
		}
	}
};

} // namespace duckdb
