//===----------------------------------------------------------------------===//
//                         DuckDB - semantic_views
//
// semantic_time_spine.hpp
//
// The built-in time spine: a view whose logical tables declare TIME (<column>)
// gets the logical table metric_time (primary key day) and its grains as
// dimensions. The spine is never scanned or joined: metric_time.day reads the
// TIME column of the table that reaches it (SemanticJoinScope), and cumulative
// anchors over it are generated (semantic_cumulative.hpp).
//
//===----------------------------------------------------------------------===//

#pragma once

#include "semantic_model.hpp"

namespace duckdb {

static constexpr const char *SEMANTIC_TIME_SPINE_TABLE = "metric_time";
static constexpr const char *SEMANTIC_TIME_SPINE_COLUMN = "day";

//! Whether a column of `type` can be a TIME column or reference the spine: DATE or a timestamp
bool SemanticIsTimeType(const LogicalType &type);
//! The spine day of `value`, a TIME column of `type`: CAST(value AS DATE). TIMESTAMPTZ has no direct cast to DATE;
//! it is cast to TIMESTAMP first, in the session time zone.
unique_ptr<ParsedExpression> SemanticTimeSpineDay(unique_ptr<ParsedExpression> value, const LogicalType &type);
//! The metric_time logical table: one DATE column, day, its primary key. Its source is an empty query, only read
//! when probing the types of its dimensions.
SemanticTable SemanticTimeSpineTable();
//! The built-in dimensions of metric_time: day (the column), and week, month, quarter and year (date_trunc of it)
vector<unique_ptr<ParsedSemanticMember>> SemanticTimeSpineDimensions();

} // namespace duckdb
