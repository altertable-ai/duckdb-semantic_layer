//===----------------------------------------------------------------------===//
//                         DuckDB - semantic_layer
//
// semantic_transforms.hpp
//
// The OFFSETs, CUMULATIVEs and FILTERs a query reads: those of the model (SemanticModel::offsets / cumulatives and the
// members' filters, shared and immutable) followed by the ones the query adds itself. An atom's metric context holds
// indices into this store. Query-time transforms (time_shift(...) / cumulative(...) calls in METRICS) and filters
// (AGG(<metric>) FILTER (WHERE ...)) are validated with the rules of the definition clauses, stored here, and replaced
// by markers (semantic_markers.hpp).
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/optional_ptr.hpp"
#include "semantic_model.hpp"

namespace duckdb {

class ExtensionLoader;
class FunctionExpression;
class SemanticAnalyzer;

//! The OFFSETs, CUMULATIVEs and FILTER conditions a query adds to those of its model. Owned by the resolved query
//! (SemanticQuerySpec::transforms), whose markers address them; boxed so that references handed out stay valid when
//! entries are added
struct SemanticQueryTransforms {
	vector<unique_ptr<SemanticOffset>> offsets;
	vector<unique_ptr<SemanticCumulative>> cumulatives;
	vector<unique_ptr<ParsedExpression>> filters;
};

class SemanticTransforms {
public:
	explicit SemanticTransforms(const SemanticModel &model)
	    : model(model), query(make_shared_ptr<SemanticQueryTransforms>()) {
	}

	//! Reads and adds the query-local entries of `query` from now on: those of the query being resolved or compiled
	void Bind(shared_ptr<SemanticQueryTransforms> query);

	//! Index < model.offsets.size(): the model's; otherwise query-local
	const SemanticOffset &Offset(idx_t index) const;
	const SemanticCumulative &Cumulative(idx_t index) const;
	//! Index of an Equals() entry (model's first, then local), or appends; returns its index
	idx_t AddOffset(SemanticOffset offset);
	idx_t AddCumulative(SemanticCumulative cumulative);
	//! Index < model.members.size(): the FILTER of that member; otherwise a query-local condition
	const ParsedExpression &Filter(idx_t index) const;
	//! Index (>= model.members.size()) of an Equals() query-local condition, or appends; returns its index
	idx_t AddFilter(unique_ptr<ParsedExpression> condition);
	//! The first index (members' FILTERs, then query-local ones) whose condition Equals() that of `index`: filters
	//! with equal conditions filter the same rows
	idx_t CanonicalFilter(idx_t index) const;
	//! For error messages: `FILTER of metric "<name>"` (a member's FILTER) or `FILTER (WHERE <condition>)`
	//! (query-local)
	string FilterDisplayName(idx_t index) const;

private:
	const SemanticModel &model;
	shared_ptr<SemanticQueryTransforms> query;
};

//! The error of a metric FILTER that is not row-level, after "<what>: "
static constexpr const char *SEMANTIC_ROW_LEVEL_FILTER_ERROR =
    "FILTER must be a row-level condition (dimensions, facts and columns only)";
//! Whether a resolved FILTER condition is row-level: no aggregate, metric, window function, AGG() or transform call
bool SemanticIsRowLevelCondition(SemanticAnalyzer &analyzer, const ParsedExpression &condition);

//! Validated offset of dimension member `dimension` (defined as a column) of the analyzer's model; throws
//! SemanticDefinitionException "<what>: ..."
SemanticOffset SemanticMakeOffset(SemanticAnalyzer &analyzer, idx_t dimension, const ParsedExpression &interval,
                                  const string &what);
//! Validated cumulative of dimension member `dimension` (defined as a column); `window` null unless kind WINDOW,
//! `part` (lower case) empty unless GRAIN; throws SemanticDefinitionException "<what>: ..."
SemanticCumulative SemanticMakeCumulative(SemanticAnalyzer &analyzer, idx_t dimension, const ParsedExpression *window,
                                          const string &part, SemanticCumulativePeriod period, const string &what);

//! The query-time forms of a semantic query: AGG(<metric>), time_shift(...) and cumulative(...)
enum class SemanticQueryForm : uint8_t { NONE, AGG, TIME_SHIFT, CUMULATIVE };
//! The query-time form `expr` calls, unqualified, as a function or a window function; NONE otherwise
SemanticQueryForm SemanticQueryFormOf(const ParsedExpression &expr);

//! The first unqualified time_shift / cumulative call in `expr`, or nullptr
optional_ptr<const FunctionExpression> SemanticFindTransformCall(const ParsedExpression &expr);
//! The metric a time_shift / cumulative call applies to: its first positional argument, or nullptr
optional_ptr<const ParsedExpression> SemanticTransformMetricArgument(const FunctionExpression &call);
//! Whether `expr` contains an unqualified AGG(...) call, or AGG(...) OVER (...)
bool SemanticContainsAggCall(const ParsedExpression &expr);
//! A resolved expression as the user could have written it: markers replaced by the names they stand for
unique_ptr<ParsedExpression> SemanticDisplayExpression(const SemanticModel &model, const ParsedExpression &expr);

//! Checks the time_shift / cumulative calls of an unresolved METRICS item for errors that resolution would hide (an
//! unquoted period / to_date value fails as an unknown column)
void SemanticCheckUnresolvedTransforms(const ParsedExpression &expr, const string &what);
//! Replaces the time_shift / cumulative calls of a resolved METRICS item by markers (validated, stored in
//! analyzer.Transforms()), and its AGG(<metric>) calls by their metric (with FILTER (WHERE ...): by a filter marker);
//! throws for calls in other positions
unique_ptr<ParsedExpression> SemanticResolveTransforms(SemanticAnalyzer &analyzer,
                                                       unique_ptr<ParsedExpression> resolved, SemanticMemberKind kind,
                                                       const string &what);

//! Catalog entries (duckdb_functions()) for time_shift and cumulative; binding them outside a semantic query fails
void RegisterSemanticTransformFunctions(ExtensionLoader &loader);

} // namespace duckdb
