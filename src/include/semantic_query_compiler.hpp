//===----------------------------------------------------------------------===//
//                         DuckDB - semantic_layer
//
// semantic_query_compiler.hpp
//
// Compiles a semantic query (dimensions / metrics / facts / filter) into a
// plain DuckDB SELECT statement:
//   - one aggregation block per metric home table, each joining only the
//     many-to-one tables it needs (no fan-out),
//   - blocks combined on the dimension values (UNION ALL + GROUP BY),
//   - metric post-aggregation expressions evaluated on top.
//
//===----------------------------------------------------------------------===//

#pragma once

#include <functional>
#include "duckdb/parser/statement/select_statement.hpp"
#include "semantic_analysis.hpp"
#include "semantic_join_scope.hpp"

namespace duckdb {

class SemanticExpressionCompiler;
struct SemanticAliasGenerator;

struct SemanticQueryItem {
	SemanticMemberKind kind = SemanticMemberKind::DIMENSION;
	//! Resolved expression (marker references)
	unique_ptr<ParsedExpression> resolved;
	string output_name;

	SemanticQueryItem() = default;
	SemanticQueryItem(SemanticMemberKind kind, unique_ptr<ParsedExpression> resolved, string output_name)
	    : kind(kind), resolved(std::move(resolved)), output_name(std::move(output_name)) {
	}
	SemanticQueryItem(const SemanticQueryItem &) = delete;
	SemanticQueryItem &operator=(const SemanticQueryItem &) = delete;
	SemanticQueryItem(SemanticQueryItem &&) noexcept = default;
	SemanticQueryItem &operator=(SemanticQueryItem &&) noexcept = default;
};

//! Error message prefix for item `item`: Invalid <kind> "<output name>"
string SemanticItemWhat(const SemanticQueryItem &item);

struct SemanticQuerySpec {
	vector<SemanticQueryItem> items;
	//! Resolved filter, may be null
	unique_ptr<ParsedExpression> where;
};

//! Columns of materialized block tables: one per dimension, the number of home rows of each group, then atom states
string SemanticDimensionColumn(idx_t dimension);
static constexpr const char *MATERIALIZED_ROWS_COLUMN = "__sv_rows";

//! An aggregation block: the atoms aggregated together
struct SemanticBlockPlan {
	SemanticBlockKey key;
	vector<SemanticAtom> atoms;
	//! Result type of every atom
	vector<LogicalType> atom_types;
	SemanticBlockDependencies dependencies;

	SemanticBlockPlan() = default;
	SemanticBlockPlan(const SemanticBlockPlan &) = delete;
	SemanticBlockPlan &operator=(const SemanticBlockPlan &) = delete;
	SemanticBlockPlan(SemanticBlockPlan &&) noexcept = default;
	SemanticBlockPlan &operator=(SemanticBlockPlan &&) noexcept = default;
};

//! A block of a query read from a materialization: SELECT <dimensions>, <aggregates> FROM table WHERE where GROUP BY
struct SemanticMaterializedRead {
	unique_ptr<TableRef> table;
	//! Per query dimension, its expression over the table's columns
	vector<unique_ptr<ParsedExpression>> dimensions;
	//! The query's filter over the table's columns, or null
	unique_ptr<ParsedExpression> where;
	//! Per atom of the block (in the order given to Find), the aggregate computing it from the stored state
	vector<unique_ptr<ParsedExpression>> aggregates;
};

//! Materializations a query may read blocks from (semantic_materialization.hpp)
class SemanticMaterializationProvider {
public:
	virtual ~SemanticMaterializationProvider() = default;
	//! A fresh materialization holding `atoms` (all of block `block`) at a grain from which `dimensions` and `where`
	//! can be computed; nullptr when there is none
	virtual unique_ptr<SemanticMaterializedRead> Find(const SemanticBlockKey &block, const vector<idx_t> &atoms,
	                                                  const vector<SemanticAtom> &all_atoms,
	                                                  const vector<const ParsedExpression *> &dimensions,
	                                                  optional_ptr<const ParsedExpression> where) = 0;
};

//! Extra row filter of a state query, built in its block's scope from its compiled dimensions
using SemanticRowFilter =
    std::function<unique_ptr<ParsedExpression>(SemanticJoinScope &scope, const vector<unique_ptr<ParsedExpression>> &)>;
//! The state columns (aliased) of atom `atom` of a block, from its compiled aggregate call
using SemanticStateBuilder =
    std::function<vector<unique_ptr<ParsedExpression>>(idx_t atom, const FunctionExpression &)>;

struct SemanticCompileOptions {
	//! Where logical tables are read from (default: their definitions)
	optional_ptr<const SemanticTableSources> sources;
	//! Materializations blocks can be read from (may be null)
	SemanticMaterializationProvider *materializations = nullptr;
	//! State queries only: an extra row filter
	SemanticRowFilter row_filter;
};

class SemanticQueryCompiler {
public:
	SemanticQueryCompiler(const SemanticModel &model, ClientContext &context);

	SemanticAnalyzer &Analyzer() {
		return analyzer;
	}

	//! Resolves the items and filter of a SEMANTIC_VIEW(...) clause (wildcards, names, PRIVATE checks)
	SemanticQuerySpec Resolve(const vector<ParsedSemanticQueryItem> &items, optional_ptr<ParsedExpression> where);
	//! Compiles a resolved query into a SELECT statement
	unique_ptr<SelectStatement> Compile(const SemanticQuerySpec &spec,
	                                    const SemanticCompileOptions &options = SemanticCompileOptions());
	//! The aggregation blocks of `spec`'s metrics, with their atom types
	vector<SemanticBlockPlan> PlanBlocks(const SemanticQuerySpec &spec);
	//! The rows of a materialization of block `block` of `spec` (dimensions and metrics only):
	//!   SELECT <dimensions AS __sv_d<i>>, count(*) AS __sv_rows, <states of each atom> FROM ... GROUP BY ALL
	unique_ptr<SelectStatement> CompileStates(const SemanticQuerySpec &spec, idx_t block,
	                                          const SemanticStateBuilder &states,
	                                          const SemanticCompileOptions &options = SemanticCompileOptions());
	//! Compiles a single member (including PRIVATE ones) for type probing; `column` is its output column
	unique_ptr<SelectStatement> CompileMemberProbe(idx_t member, idx_t &column);

private:
	unique_ptr<SelectStatement> CompileRowQuery(SemanticExpressionCompiler &compiler, SemanticAliasGenerator &aliases,
	                                            const SemanticQuerySpec &spec, bool has_dimensions, bool has_facts,
	                                            const set<idx_t> &row_tables, const SemanticCompileOptions &options);
	//! Classifies the items of `spec` and checks their kinds; fills the row tables of dimensions / facts / WHERE
	void SplitItems(const SemanticQuerySpec &spec, vector<const SemanticQueryItem *> &dimensions,
	                vector<const SemanticQueryItem *> &facts, vector<const SemanticQueryItem *> &metrics,
	                set<idx_t> &row_tables);

	const SemanticModel &model;
	SemanticAnalyzer analyzer;
};

//! Binds a probe query per member and records its output type (called while creating a semantic view)
void ProbeSemanticModelTypes(ClientContext &context, SemanticModel &model);

} // namespace duckdb
