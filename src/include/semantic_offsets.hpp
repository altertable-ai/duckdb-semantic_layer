//===----------------------------------------------------------------------===//
//                         DuckDB - semantic_layer
//
// semantic_offsets.hpp
//
// Metric OFFSETs in queries: in the aggregation blocks of an offset metric,
// the query's dimensions and WHERE read the offset column shifted by the
// interval (aggregate arguments, FILTER and NON ADDITIVE BY read it as is).
//
//===----------------------------------------------------------------------===//

#pragma once

#include "semantic_expression_compiler.hpp"

namespace duckdb {

//! Reads the columns of `offsets` (SemanticModel::offsets) shifted, in order
class SemanticOffsetMap : public SemanticColumnMap {
public:
	SemanticOffsetMap(const SemanticModel &model, const vector<idx_t> &offsets) : model(model), offsets(offsets) {
	}
	unique_ptr<ParsedExpression> Column(idx_t table, const string &column,
	                                    unique_ptr<ParsedExpression> compiled) override;

private:
	const SemanticModel &model;
	const vector<idx_t> &offsets;
};

//! `CAST(<column> + <interval> AS <type>)`: `column` read shifted by `offset`
unique_ptr<ParsedExpression> SemanticShiftColumn(unique_ptr<ParsedExpression> column, const SemanticOffset &offset);

} // namespace duckdb
