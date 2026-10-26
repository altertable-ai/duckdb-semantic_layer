//===----------------------------------------------------------------------===//
//                         DuckDB - semantic_layer
//
// semantic_materialization.hpp
//
// Materializations: cached aggregation blocks of a semantic view at a chosen
// grain. A materialization stores, per group of its dimensions, the *state*
// of every aggregate of its metrics (SUM -> sum + count, AVG -> sum + count,
// ...), so that queries at the same or a coarser grain can be answered from
// it, and source changes can be applied to it incrementally.
//
// The semantic side (what is aggregated, at what grain, which queries can use
// it) lives here; what changed in the sources comes from
// semantic_change_tracking.hpp.
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/mutex.hpp"
#include "duckdb/common/types/timestamp.hpp"
#include "semantic_query_compiler.hpp"

namespace duckdb {

class SemanticModel;
class SemanticMaterialization;

//! How the state of an aggregate is stored, rolled up to a coarser grain and maintained
enum class SemanticAggregateKind : uint8_t {
	//! state: sum, count of non-NULL arguments (the sum becomes NULL when the count drops to 0)
	SUM,
	//! state: count (COUNT(x) / COUNT(*))
	COUNT,
	//! state: sum, count of non-NULL arguments
	AVG,
	//! state: min / max; inserts are merged, groups that lose rows are recomputed
	MIN,
	MAX,
	//! any other aggregate (DISTINCT, ORDER BY, median, ...): stored as is, cannot be rolled up, recomputed per group
	OPAQUE
};

string SemanticAggregateKindToString(SemanticAggregateKind kind);

//! An aggregate of a materialized block and its state columns (<prefix>_sum, <prefix>_cnt, <prefix>_min, ...)
struct SemanticMaterializedAtom {
	//! The atom (aggregate call and metric context): a query atom equal to it reads this state
	SemanticAtom atom;
	SemanticAggregateKind kind;
	//! Result type of the aggregate
	LogicalType type;
	string prefix;

	SemanticMaterializedAtom() = default;
	SemanticMaterializedAtom(const SemanticMaterializedAtom &) = delete;
	SemanticMaterializedAtom &operator=(const SemanticMaterializedAtom &) = delete;
	SemanticMaterializedAtom(SemanticMaterializedAtom &&) noexcept = default;
	SemanticMaterializedAtom &operator=(SemanticMaterializedAtom &&) noexcept = default;

	//! The state columns, in table order
	vector<string> StateColumns() const;
};

//! One physical table: the atoms of a materialization aggregated over the same home table
struct SemanticMaterializedBlock {
	idx_t home;
	vector<idx_t> using_relationships;
	vector<SemanticMaterializedAtom> atoms;
	//! Logical tables the block reads (home, tables on the join paths of its dimensions and arguments)
	set<idx_t> tables;
	//! Logical tables read by nested aggregations, and those of them maintainable incrementally (direct child tables
	//! aggregating their own columns)
	set<idx_t> nested_tables;
	vector<SemanticChildDependency> children;
	bool dependencies_complete = true;

	SemanticMaterializedBlock() = default;
	SemanticMaterializedBlock(const SemanticMaterializedBlock &) = delete;
	SemanticMaterializedBlock &operator=(const SemanticMaterializedBlock &) = delete;
	SemanticMaterializedBlock(SemanticMaterializedBlock &&) noexcept = default;
	SemanticMaterializedBlock &operator=(SemanticMaterializedBlock &&) noexcept = default;

	//! Every logical table whose changes can change the block
	set<idx_t> SourceTables() const {
		auto result = tables;
		result.insert(nested_tables.begin(), nested_tables.end());
		return result;
	}
};

//! Mutable state of a materialization, shared by the successive models of its semantic view (ALTER)
struct SemanticMaterializationState {
	mutex lock;
	//! Held for the duration of a refresh (refreshes of a materialization are serialized)
	mutex refresh_lock;
	//! Current metadata owner, shared across renamed models and their already-bound refreshes.
	QualifiedName owner;
	//! Physical location, set by the first build
	string catalog;
	string schema;
	vector<string> tables;
	bool built = false;
	//! A removed state cannot be rebuilt by a refresh that was bound before the DDL transition.
	bool retired = false;
	//! Per DuckLake catalog of the sources, the snapshot the materialization is correct at (empty: untracked)
	map<string, idx_t> watermark;
	//! When the materialization was last known to be fresh
	timestamp_t verified_at = timestamp_t(0);
	//! Outcome of the last refresh
	string last_refresh;
	string last_refresh_detail;
};

//! A materialization of a semantic view: its definition, analyzed against the model (immutable), and its state
class SemanticMaterialization {
public:
	string name;
	SemanticRefreshMode refresh_mode;
	//! The grain: the materialization's dimensions (resolved) and the query spec it materializes (dims + metrics)
	SemanticQuerySpec spec;
	idx_t dimension_count;
	vector<string> dimension_names;
	vector<string> metric_names;
	vector<SemanticMaterializedBlock> blocks;
	//! Whether the definition allows incremental refresh (the sources are checked when refreshing), and why not
	bool incremental = false;
	string incremental_reason;
	//! Where its tables are stored: a DuckDB database or a DuckLake catalog, and a schema
	string storage_catalog;
	string storage_schema;
	//! What its tables hold (a hash of the queries building them): stored tables with the same fingerprint, at a
	//! known watermark, can be reattached as they are
	string fingerprint;

	shared_ptr<SemanticMaterializationState> state;

	//! The fully qualified physical table of block `block`
	string TableName(idx_t block) const;
};

//! Analyzes the materializations of a model's definition (called while building the model)
void PlanSemanticMaterializations(ClientContext &context, SemanticModel &model);

//! Reuses the state of the materializations `previous` already had (same name and definition), so that ALTERs keep them
void AdoptMaterializationStates(SemanticModel &model, const SemanticModel &previous);
//! Whether two materializations are stored in the same tables
bool SameMaterializationStorage(const SemanticMaterialization &a, const SemanticMaterialization &b);

//! A materialization stored for a semantic view, as recorded in its database's metadata
struct SemanticStoredMaterialization {
	string catalog;
	string schema;
	vector<string> tables;
	QualifiedName view;
	string name;
};
//! Adds to `model` (being created) the materializations stored for its view in the attached databases that it does not
//! define yet: reattached as they are when their definition did not change, else to be rebuilt by the next refresh.
//! Returns those that no longer apply to the view (their tables are dropped once the view is created).
vector<SemanticStoredMaterialization> RestoreSemanticMaterializations(ClientContext &context, SemanticModel &model);
//! Stages storage changes before publishing a DDL registry transition. The placeholder is changed after all
//! storage operations have succeeded, and before their transactions commit.
void ApplySemanticMaterializationChange(ClientContext &context, const shared_ptr<SemanticModel> &before,
                                        const shared_ptr<SemanticModel> &after,
                                        const vector<shared_ptr<SemanticMaterialization>> &added,
                                        const vector<SemanticStoredMaterialization> &obsolete,
                                        const std::function<void()> &placeholder);

//! Brings a materialization up to date; returns a description of what was done
string RefreshSemanticMaterialization(ClientContext &context, const SemanticModel &model,
                                      const SemanticMaterialization &materialization, SemanticRefreshMode mode);
//! Whether a materialization can currently answer queries, and why not
struct SemanticMaterializationStatus {
	bool usable = false;
	string reason;
};
SemanticMaterializationStatus GetMaterializationStatus(ClientContext &context, const SemanticModel &model,
                                                       const SemanticMaterialization &materialization);

//! The materializations a query of `model` may read blocks from (honors the semantic_view_materializations setting)
class SemanticMaterializationMatcher : public SemanticMaterializationProvider {
public:
	SemanticMaterializationMatcher(ClientContext &context, const SemanticModel &model);

	unique_ptr<SemanticMaterializedRead> Find(const SemanticBlockKey &block, const vector<idx_t> &atoms,
	                                          const vector<SemanticAtom> &all_atoms,
	                                          const vector<const ParsedExpression *> &dimensions,
	                                          optional_ptr<const ParsedExpression> where) override;

	//! Whether a block of the query was read from a materialization
	bool UsedMaterialization() const {
		return used;
	}

private:
	bool IsUsable(const SemanticMaterialization &materialization);

	ClientContext &context;
	const SemanticModel &model;
	bool enabled;
	bool used = false;
	map<const SemanticMaterialization *, bool> usable_cache;
};

//! The extension setting that enables reading materializations
static constexpr const char *SEMANTIC_VIEW_MATERIALIZATIONS_SETTING = "semantic_view_materializations";

} // namespace duckdb
