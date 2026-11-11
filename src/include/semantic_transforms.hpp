//===----------------------------------------------------------------------===//
//                         DuckDB - semantic_layer
//
// semantic_transforms.hpp
//
// The OFFSETs and CUMULATIVEs a query reads: those of the model (SemanticModel::offsets / cumulatives, shared and
// immutable) followed by the ones the query adds itself. An atom's metric context holds indices into this store.
// Query-time transforms (time_shift(...) / cumulative(...) calls in METRICS) are validated with the rules of the
// definition clauses, stored here, and replaced by markers (semantic_markers.hpp).
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/optional_ptr.hpp"
#include "semantic_model.hpp"

namespace duckdb {

class ClientContext;
class ExtensionLoader;
class FunctionExpression;
class SemanticAnalyzer;
struct SemanticAtom;

class SemanticTransforms {
public:
	explicit SemanticTransforms(const SemanticModel &model) : model(model) {
	}

	//! Index < model.offsets.size(): the model's; otherwise query-local
	const SemanticOffset &Offset(idx_t index) const;
	const SemanticCumulative &Cumulative(idx_t index) const;
	//! Index of an Equals() entry (model's first, then local), or appends; returns its index
	idx_t AddOffset(SemanticOffset offset);
	idx_t AddCumulative(SemanticCumulative cumulative);

private:
	const SemanticModel &model;
	//! Boxed so that references handed out stay valid when entries are added
	vector<unique_ptr<SemanticOffset>> offsets;
	vector<unique_ptr<SemanticCumulative>> cumulatives;
};

//! Whether `expr` is a constant: no column, member, aggregate, window or parameter
bool SemanticIsConstant(SemanticAnalyzer &analyzer, const ParsedExpression &expr);
//! Binds SELECT <expr>; the binder's error message, or empty (then `type`, if given, receives the expression's type)
string SemanticBindError(ClientContext &context, unique_ptr<ParsedExpression> expr,
                         optional_ptr<LogicalType> type = nullptr);
//! Validated offset of dimension member `dimension` (defined as a column); throws SemanticDefinitionException
//! "<what>: ..." with today's messages
SemanticOffset SemanticMakeOffset(ClientContext &context, SemanticAnalyzer &analyzer, const SemanticModel &model,
                                  idx_t dimension, const ParsedExpression &interval, const string &what);
//! Validated cumulative of dimension member `dimension` (defined as a column); `window` null unless kind WINDOW,
//! `part` (lower case) empty unless GRAIN; throws SemanticDefinitionException "<what>: ..." with today's messages
SemanticCumulative SemanticMakeCumulative(ClientContext &context, SemanticAnalyzer &analyzer,
                                          const SemanticModel &model, idx_t dimension, const ParsedExpression *window,
                                          const string &part, SemanticCumulativePeriod period, const string &what);

//! The first unqualified time_shift / cumulative call in `expr`, or nullptr
optional_ptr<const FunctionExpression> SemanticFindTransformCall(const ParsedExpression &expr);

//! A call's arguments: unaliased ones in order, aliased ones (`name := value`) by name. Throws when the number of
//! positional arguments differs from `positional`, or a name is unknown or repeated, listing the signature.
struct SemanticTransformArguments {
	vector<const ParsedExpression *> positional;
	case_insensitive_map_t<const ParsedExpression *> named;
};
SemanticTransformArguments SemanticSplitTransformArguments(const FunctionExpression &call, idx_t positional,
                                                           const vector<string> &names, const string &what);
//! The dimension member of `dimension := ...`, or metric_time.day when absent and the view has TIME; throws
//! (listing valid dimensions) otherwise
idx_t SemanticTransformDimension(const SemanticModel &model, optional_ptr<const ParsedExpression> argument,
                                 const vector<SemanticAtom> &atoms, const string &what);
//! Replaces the time_shift / cumulative calls of a resolved METRICS item by markers (validated, stored in
//! analyzer.Transforms()); throws for calls in other positions
unique_ptr<ParsedExpression> SemanticResolveTransforms(ClientContext &context, SemanticAnalyzer &analyzer,
                                                       const SemanticModel &model,
                                                       unique_ptr<ParsedExpression> resolved, SemanticMemberKind kind,
                                                       const string &what);

//! Catalog entries (duckdb_functions()) for time_shift and cumulative; binding them outside a semantic query fails
void RegisterSemanticTransformFunctions(ExtensionLoader &loader);

} // namespace duckdb
