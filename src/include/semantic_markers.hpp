//===----------------------------------------------------------------------===//
//                         DuckDB - semantic_views
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
// This module is the only place that knows the encoding.
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/parser/parsed_expression.hpp"

namespace duckdb {

//! Function name `PARTITION BY EXCLUDING` lowers to (also used by the PEG transformer)
static constexpr const char *SEMANTIC_EXCLUDING_FUNCTION = "__sv_excluding";

enum class SemanticRefKind : uint8_t { NONE, COLUMN, MEMBER, ATOM };

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
};

} // namespace duckdb
