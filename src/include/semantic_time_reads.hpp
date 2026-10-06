//===----------------------------------------------------------------------===//
//                         DuckDB - semantic_views
//
// semantic_time_reads.hpp
//
// What the query's dimensions and WHERE read, checked against the metrics' OFFSET and CUMULATIVE clauses: both only
// move their date column (shifted / anchored), so nothing else of that column's table may be read.
//
//===----------------------------------------------------------------------===//

#pragma once

#include "semantic_query_compiler.hpp"

namespace duckdb {

//! Whether row-level expression `expr` reads column `column` of logical table `table` (following row-level members)
bool SemanticReadsColumn(const SemanticModel &model, SemanticAnalyzer &analyzer, const ParsedExpression &expr,
                         idx_t table, const string &column);

//! OFFSET and CUMULATIVE only move their column: a query of an offset metric must read that column in its dimensions
//! or WHERE (otherwise the offset has no effect), and no query may read other columns of an OFFSET / CUMULATIVE
//! column's table unless it is the metric's home table (a stored year of a date table would not move with the date)
void CheckSemanticTimeReads(const SemanticModel &model, SemanticAnalyzer &analyzer, const SemanticQuerySpec &spec);

} // namespace duckdb
