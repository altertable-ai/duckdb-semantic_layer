//===----------------------------------------------------------------------===//
//                         DuckDB - semantic_views
//
// semantic_offsets.hpp
//
// Metric OFFSETs in queries: in the aggregation blocks of an offset metric,
// the query's dimensions and WHERE read the offset column shifted by the
// interval (aggregate arguments, FILTER and NON ADDITIVE BY read it as is).
//
//===----------------------------------------------------------------------===//

#pragma once

#include "semantic_query_compiler.hpp"

namespace duckdb {

//! `CAST(<column> + <interval> AS <type>)`: `column` read shifted by `offset`
unique_ptr<ParsedExpression> SemanticShiftColumn(unique_ptr<ParsedExpression> column, const SemanticOffset &offset);

//! An OFFSET only shifts its column: a query of an offset metric must read that column in its dimensions or WHERE
//! (otherwise the offset has no effect), and no other column of its table unless it is the metric's home table (a
//! stored year of a date table would not be shifted with the date)
void CheckSemanticOffsetReads(const SemanticModel &model, SemanticAnalyzer &analyzer, const SemanticQuerySpec &spec);

} // namespace duckdb
