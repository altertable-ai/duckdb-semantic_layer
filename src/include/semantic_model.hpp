//===----------------------------------------------------------------------===//
//                         DuckDB - semantic_views
//
// semantic_model.hpp
//
// Validated, name-resolved semantic view definition. Immutable once built;
// shared between the registry and in-flight queries.
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/case_insensitive_map.hpp"
#include "duckdb/common/optional_idx.hpp"
#include "duckdb/common/set.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/common/types/timestamp.hpp"
#include "duckdb/parser/expression/columnref_expression.hpp"
#include "parsed_semantic_view.hpp"
#include <functional>
#include "semantic_markers.hpp"

namespace duckdb {

class ClientContext;
class SemanticMaterialization;

struct SemanticTable {
	string name;
	//! Fully qualified BaseTableRef, or SubqueryRef for query-backed logical tables (and the empty time spine)
	unique_ptr<TableRef> source;
	string catalog_name;
	string schema_name;
	string table_name;
	vector<string> column_names;
	vector<LogicalType> column_types;
	case_insensitive_map_t<idx_t> columns;
	vector<string> primary_key;
	vector<vector<string>> unique_keys;
	vector<ParsedSemanticRangeConstraint> range_constraints;
	vector<string> synonyms;
	string comment;
	vector<ParsedSemanticTag> tags;
	//! TIME (<column>) of this table, empty when it has none (resolved column name)
	string time_column;

	bool IsBaseTable() const {
		return !table_name.empty();
	}
	//! Whether `cols` (case-insensitive, any order) equals the primary key or a unique key
	bool IsKey(const vector<string> &cols) const;
};

struct SemanticRelationship {
	string name;
	//! The "many" side (foreign key side)
	idx_t from_table;
	//! The "one" side (primary / unique key side)
	idx_t to_table;
	vector<string> from_columns;
	vector<string> to_columns;
	//! from_columns form a key of from_table: the relationship can be traversed in both directions
	bool one_to_one = false;
	idx_t asof_index = DConstants::INVALID_INDEX;
	bool is_range = false;
	string range_start;
	string range_end;
	//! Generated from TIME (...): its table's default path to the time spine. When several paths reach a table,
	//! TryPath prefers the one through a default relationship.
	bool is_default = false;
};

//! A traversal step of the relationship graph
struct SemanticEdge {
	idx_t relationship;
	//! true when traversing a one-to-one relationship from its "one" side to its "many" side
	bool reversed;
	idx_t target;
};

enum class SemanticPathResult : uint8_t {
	FOUND,
	//! `to` is not at an equal or lower granularity than `from`
	UNREACHABLE,
	//! every path uses an edge that the USING relationships exclude
	EXCLUDED_BY_USING,
	//! several paths
	AMBIGUOUS
};

//! One entry of NON ADDITIVE BY (...)
struct SemanticNonAdditive {
	//! Resolved row-level expression (normally a dimension reference)
	unique_ptr<ParsedExpression> resolved;
	//! Explicit sort order (ASC / DESC with NULLS FIRST / LAST resolved)
	OrderType type;
	OrderByNullType null_order;
};

//! OFFSET (<dimension> BY <interval>) of metrics: in their aggregation blocks, the dimension's column reads shifted by
//! the interval in the query's dimensions and WHERE (semantic_offsets.hpp). Equal offsets are stored once.
//! Move-only: MSVC instantiates `vector`'s copy constructor unless the copy is explicitly deleted.
struct SemanticOffset {
	//! The offset dimension: a dimension defined as a column
	idx_t dimension;
	idx_t table;
	string column;
	//! The column's type: shifted values are cast back to it
	LogicalType type;
	//! Constant interval expression, as written
	unique_ptr<ParsedExpression> interval;

	SemanticOffset() = default;
	SemanticOffset(const SemanticOffset &) = delete;
	SemanticOffset &operator=(const SemanticOffset &) = delete;
	SemanticOffset(SemanticOffset &&) noexcept = default;
	SemanticOffset &operator=(SemanticOffset &&) noexcept = default;

	//! Whether both shift the same column by the same interval
	bool Equals(const SemanticOffset &other) const {
		return table == other.table && StringUtil::CIEquals(column, other.column) && interval->Equals(*other.interval);
	}
};

enum class SemanticCumulativeKind : uint8_t {
	//! running total: c <= t
	ALL,
	//! OVER <interval>: t - interval < c <= t
	WINDOW,
	//! TO <part>: date_trunc(part, t) <= c <= t
	GRAIN
};

//! CUMULATIVE (<dimension> ...) of metrics: in their aggregation blocks, each row counts for every anchor date t (a
//! value of the dimension's column) whose window holds its date, and the query's dimensions and WHERE read t
//! (semantic_cumulative.hpp). Equal clauses are stored once.
//! Move-only: MSVC instantiates `vector`'s copy constructor unless the copy is explicitly deleted.
struct SemanticCumulative {
	//! The cumulative dimension: a dimension defined as a column
	idx_t dimension;
	idx_t table;
	string column;
	LogicalType type;
	SemanticCumulativeKind kind;
	//! WINDOW: constant interval expression, as written
	unique_ptr<ParsedExpression> window;
	//! GRAIN: date part (lower case)
	string part;
	SemanticCumulativePeriod period;

	SemanticCumulative() = default;
	SemanticCumulative(const SemanticCumulative &) = delete;
	SemanticCumulative &operator=(const SemanticCumulative &) = delete;
	SemanticCumulative(SemanticCumulative &&) noexcept = default;
	SemanticCumulative &operator=(SemanticCumulative &&) noexcept = default;

	//! Whether both aggregate the same windows of the same column and reduce them the same way
	bool Equals(const SemanticCumulative &other) const {
		return table == other.table && StringUtil::CIEquals(column, other.column) && kind == other.kind &&
		       part == other.part && period == other.period &&
		       (window ? other.window && window->Equals(*other.window) : !other.window);
	}
};

struct SemanticMember {
	//! Definition entry, owned by SemanticModel::definition
	const ParsedSemanticMember *parsed;
	//! Owning logical table, DConstants::INVALID_INDEX for derived metrics
	idx_t table;
	//! Expression with every column reference replaced by a marker reference
	unique_ptr<ParsedExpression> resolved;
	//! Output type, computed by binding a probe query
	LogicalType type = LogicalType::UNKNOWN;
	//! Relationships pinned with USING (...) (metrics only)
	vector<idx_t> using_relationships;
	//! NON ADDITIVE BY (...) (metrics only)
	vector<SemanticNonAdditive> non_additive;
	//! Resolved FILTER condition (metrics only), or null
	unique_ptr<ParsedExpression> filter;
	//! OFFSET (metrics only): index into SemanticModel::offsets
	optional_idx offset;
	//! CUMULATIVE (metrics only): index into SemanticModel::cumulatives
	optional_idx cumulative;

	SemanticMemberKind Kind() const {
		return parsed->kind;
	}
	const string &Name() const {
		return parsed->name;
	}
	bool IsPrivate() const {
		return parsed->is_private;
	}
	//! Expression as written in the definition
	const ParsedExpression &Expression() const {
		return *parsed->expression;
	}
	bool IsDerived() const {
		return table == DConstants::INVALID_INDEX;
	}
	bool IsRowLevel() const {
		return Kind() != SemanticMemberKind::METRIC;
	}
};

//! Name-resolution context of an expression
struct SemanticResolveContext {
	//! Table owning the expression (unqualified names resolve against it first), or INVALID_INDEX
	idx_t owner_table = DConstants::INVALID_INDEX;
	//! Member being defined (a reference to its own name resolves to the column instead)
	idx_t self_member = DConstants::INVALID_INDEX;
	//! Whether hidden (PRIVATE) members may be referenced
	bool allow_private = true;
	//! Prefix for error messages
	string what;
};

class SemanticModel {
public:
	//! The validated definition (its name is fully qualified). Never modified after building: members point into it.
	unique_ptr<ParsedSemanticView> definition;
	timestamp_tz_t created_on;

	//! Logical tables: those of the definition, in order, then the time spine (semantic_time_spine.hpp)
	vector<SemanticTable> tables;
	//! Index of the time spine in `tables`, invalid when no table declares TIME
	optional_idx time_spine;
	vector<SemanticRelationship> relationships;
	vector<SemanticMember> members;
	//! Distinct OFFSETs of the metrics
	vector<SemanticOffset> offsets;
	//! Distinct CUMULATIVE clauses of the metrics
	vector<SemanticCumulative> cumulatives;
	//! Built-in metric_time dimensions. Members point into these; the parsed definition stays unchanged.
	vector<unique_ptr<ParsedSemanticMember>> generated_members;
	//! Materializations of the view (semantic_materialization.hpp), in definition order
	vector<shared_ptr<SemanticMaterialization>> materializations;

	//! Traversal edges leaving each table towards equal-or-lower granularity tables
	vector<vector<SemanticEdge>> edges;
	//! path_count[a][b]: number of distinct simple paths a -> b over `edges` (capped at 2); a -> a counts 1
	vector<vector<idx_t>> path_count;

public:
	const QualifiedName &Name() const {
		return definition->name;
	}
	const string &Comment() const {
		return definition->comment;
	}

	optional_idx FindTable(const string &table_name) const;
	bool IsTimeSpine(idx_t table) const {
		return time_spine.IsValid() && time_spine.GetIndex() == table;
	}
	optional_idx FindMember(idx_t table, const string &member_name) const;
	vector<idx_t> FindMembersByName(const string &member_name) const;

	//! Whether `to` is at an equal or lower granularity than `from` and reachable from it
	bool Reaches(idx_t from, idx_t to) const {
		return path_count[from][to] > 0;
	}
	//! The table of `tables` from which every other one is reachable; invalid when there is none (or `tables` is empty)
	optional_idx FinestTable(const set<idx_t> &tables) const;
	//! The unique path from -> to (empty when from == to), or why there is none
	SemanticPathResult TryPath(idx_t from, idx_t to, const vector<idx_t> &using_relationships,
	                           vector<SemanticEdge> &path) const;
	//! The unique path from -> to (empty when from == to); throws when there are several paths or none.
	//! `using_relationships` (USING on metrics) restricts how the tables they reference are reached.
	vector<SemanticEdge> Path(idx_t from, idx_t to, const vector<idx_t> &using_relationships = {}) const;

	//! Looks up a (possibly qualified) name: a member or a column; NONE with `error` set when unknown or ambiguous
	SemanticRef Lookup(const vector<string> &names, const SemanticResolveContext &context, string &error) const;
	//! Resolves every column reference of `expr` to a marker reference (semantic_markers.hpp)
	unique_ptr<ParsedExpression> Resolve(const ParsedExpression &expr, const SemanticResolveContext &context) const;

	string MemberDisplayName(idx_t member) const;
	static string KindDisplayName(SemanticMemberKind kind);

private:
	//! Up to two simple paths from -> to through the edges `allowed` accepts
	vector<vector<SemanticEdge>> FindPaths(idx_t from, idx_t to,
	                                       const std::function<bool(const SemanticEdge &)> &allowed) const;
};

//! Builds and validates a model from a parsed definition (catalog lookups through `context`)
shared_ptr<SemanticModel> BuildSemanticModel(ClientContext &context, const ParsedSemanticView &definition,
                                             const QualifiedName &resolved_name);

} // namespace duckdb
