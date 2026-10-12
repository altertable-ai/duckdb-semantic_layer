//===----------------------------------------------------------------------===//
//                         DuckDB - semantic_views
//
// semantic_join_scope.hpp
//
// The FROM clause of one compiled (sub)query: a base logical table plus the
// tables reached from it through many-to-one relationships, joined on demand.
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/set.hpp"
#include "duckdb/common/string.hpp"
#include "duckdb/common/to_string.hpp"
#include "semantic_model.hpp"
#include "semantic_query_dependencies.hpp"

namespace duckdb {

//! Unique alias prefixes for the nested scopes (child aggregations, range lookups) of one compiled query
struct SemanticAliasGenerator {
	string NestedPrefix() {
		return "__sv" + to_string(++next) + "_";
	}

private:
	idx_t next = 0;
};

//! Where compiled queries read logical tables from, instead of their definitions (materialization refreshes read the
//! sources at a snapshot, or only the rows that changed)
class SemanticTableSources {
public:
	virtual ~SemanticTableSources() = default;
	//! The table reference of logical table `table` (the caller sets its alias)
	virtual unique_ptr<TableRef> Source(idx_t table) const = 0;
};

class SemanticJoinScope {
public:
	SemanticJoinScope(const SemanticModel &model, SemanticAliasGenerator &aliases, idx_t base,
	                  vector<idx_t> using_relationships, string prefix = string(),
	                  optional_ptr<const SemanticTableSources> sources = nullptr,
	                  optional_ptr<SemanticBlockDependencies> dependencies = nullptr);

	//! Scope of a correlated subquery over `base`: tables reachable from `outer_context` (the row the subquery
	//! belongs to) resolve in this scope instead
	SemanticJoinScope Nested(idx_t base, idx_t outer_context);

	const vector<idx_t> &UsingRelationships() const {
		return using_relationships;
	}

	//! Joins `table` (and the tables on its path) into this scope; returns its alias
	string Ensure(idx_t table);
	//! Reference to `column` of `table`. Tables reached through a range relationship are read with a correlated
	//! scalar subquery: DuckDB decorrelates it on the distinct values of the referencing column, so the lookup is a
	//! small join of those values with the range table instead of a range join over every row.
	unique_ptr<ParsedExpression> ColumnReference(idx_t table, const string &column);
	unique_ptr<TableRef> BuildFrom() const;

	optional_ptr<SemanticBlockDependencies> Dependencies() const {
		return dependencies;
	}
	idx_t Depth() const {
		return depth;
	}
	bool ReadsOnlyBase() const {
		return present.size() == 1 && !reads_outer;
	}

private:
	struct Joined {
		idx_t table;
		SemanticEdge edge;
		idx_t parent_table;
	};

	string AliasOf(idx_t table) const;
	unique_ptr<TableRef> Source(idx_t table) const;
	bool BelongsToOuter(idx_t table) const;
	//! metric_time.day: the TIME column of the table whose relationship enters the spine (joined like any of its
	//! columns), cast to DATE. The spine itself is never joined.
	unique_ptr<ParsedExpression> TimeColumn(idx_t spine);
	unique_ptr<ParsedExpression> RangeLookup(const vector<SemanticEdge> &path, idx_t range_at, idx_t table,
	                                         const string &column);

	const SemanticModel &model;
	SemanticAliasGenerator &aliases;
	idx_t base;
	vector<idx_t> using_relationships;
	string prefix;
	optional_ptr<const SemanticTableSources> sources;
	optional_ptr<SemanticBlockDependencies> dependencies;
	idx_t depth = 0;
	bool reads_outer = false;
	optional_ptr<SemanticJoinScope> outer;
	idx_t outer_context = DConstants::INVALID_INDEX;
	vector<Joined> joins;
	set<idx_t> present;
};

} // namespace duckdb
