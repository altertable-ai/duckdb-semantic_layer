//===----------------------------------------------------------------------===//
//                         DuckDB - semantic_layer
//
// semantic_markers.hpp
//
// Resolved semantic expressions are ordinary DuckDB ParsedExpressions in which
// every reference has been replaced by a marker, encoded as a column reference
// with a reserved first name part:
//   physical column  : "__sv_col__".<table index>.<column>
//   semantic member  : "__sv_mem__".<member index>
//   aggregate atom   : "__sv_atom__".<atom index>      (metric decomposition)
// and `PARTITION BY EXCLUDING a, b` is parsed as `PARTITION BY __sv_excluding(a, b)`.
// Query-time metric transforms (time_shift / cumulative) and filters (AGG(...) FILTER (WHERE ...)) are resolved to
//   __sv_offset(<index>, <argument>) / __sv_cumulative(<index>, <argument>) / __sv_filter(<index>, <argument>)
// where <index> addresses the query's SemanticTransforms.
// This module is the only place that knows the encoding.
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/parser/parsed_expression.hpp"

namespace duckdb {

//! Function name `PARTITION BY EXCLUDING` lowers to (also used by the PEG transformer)
static constexpr const char *SEMANTIC_EXCLUDING_FUNCTION = "__sv_excluding";

enum class SemanticRefKind : uint8_t { NONE, COLUMN, MEMBER, ATOM };

enum class SemanticTransformKind : uint8_t { NONE, OFFSET, CUMULATIVE, FILTER };

//! A classified transform marker: its argument is child 1
struct SemanticTransformRef {
	SemanticTransformKind kind = SemanticTransformKind::NONE;
	//! Index into SemanticTransforms (offsets, cumulatives or filters)
	idx_t index = DConstants::INVALID_INDEX;
};

//! A classified marker reference
struct SemanticRef {
	SemanticRefKind kind = SemanticRefKind::NONE;
	//! Table index (COLUMN), member index (MEMBER) or atom index (ATOM)
	idx_t index = DConstants::INVALID_INDEX;
	//! Column name (COLUMN)
	string column;

	bool Is(SemanticRefKind expected) const {
		return kind == expected;
	}
};

struct SemanticMarkers {
	static unique_ptr<ParsedExpression> Column(idx_t table, const string &column);
	static unique_ptr<ParsedExpression> Member(idx_t member);
	static unique_ptr<ParsedExpression> Atom(idx_t atom);
	static SemanticRef Classify(const ParsedExpression &expr);
	//! Whether `expr` is the lowered form of PARTITION BY EXCLUDING
	static bool IsExcluding(const ParsedExpression &expr);
	//! __sv_offset(<index>, <argument>) / __sv_cumulative(<index>, <argument>) / __sv_filter(<index>, <argument>)
	static unique_ptr<ParsedExpression> Transform(SemanticTransformKind kind, idx_t index,
	                                              unique_ptr<ParsedExpression> argument);
	static SemanticTransformRef ClassifyTransform(const ParsedExpression &expr);
	//! Whether `expr` calls a function named like a transform marker (any case or qualification): user input must not
	static bool IsTransformMarkerCall(const ParsedExpression &expr);
};

} // namespace duckdb
