//===----------------------------------------------------------------------===//
//                         DuckDB - semantic_layer
//
// semantic_lowering.hpp
//
// Semantic-view statements are lowered onto internal table functions, so that
// the rewritten statement is an ordinary DuckDB AST (usable in EXPLAIN,
// PREPARE, views, ...). This module defines those functions' arguments: the
// PEG transformer and the standard-SQL rewriter build calls through it, and
// the function implementations read their arguments with the same layout.
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/enums/on_create_conflict.hpp"
#include "duckdb/common/serializer/binary_deserializer.hpp"
#include "duckdb/common/serializer/binary_serializer.hpp"
#include "duckdb/common/serializer/memory_stream.hpp"
#include "duckdb/parser/sql_statement.hpp"
#include "duckdb/parser/tableref.hpp"
#include "parsed_semantic_view.hpp"

namespace duckdb {

//! (definition BLOB, on_conflict VARCHAR)
static constexpr const char *SEMANTIC_VIEW_CREATE_FUNCTION = "__semantic_view_create";
//! (catalog, schema, name, if_exists BOOLEAN)
static constexpr const char *SEMANTIC_VIEW_DROP_FUNCTION = "__semantic_view_drop";
//! (catalog, schema, name, if_exists BOOLEAN, new_name VARCHAR)
static constexpr const char *SEMANTIC_VIEW_RENAME_FUNCTION = "__semantic_view_rename";
//! (catalog, schema, name, if_exists BOOLEAN, comment VARCHAR - NULL unsets it)
static constexpr const char *SEMANTIC_VIEW_SET_COMMENT_FUNCTION = "__semantic_view_set_comment";
//! (catalog, schema, name, if_exists BOOLEAN, seconds BIGINT - NULL unsets it)
static constexpr const char *SEMANTIC_VIEW_SET_MAX_STALENESS_FUNCTION = "__semantic_view_set_max_staleness";
//! (catalog, schema, name, if_exists BOOLEAN, materialization BLOB, if_not_exists BOOLEAN)
static constexpr const char *SEMANTIC_VIEW_ADD_MATERIALIZATION_FUNCTION = "__semantic_view_add_materialization";
//! (catalog, schema, name, if_exists BOOLEAN, materialization VARCHAR, materialization_if_exists BOOLEAN)
static constexpr const char *SEMANTIC_VIEW_DROP_MATERIALIZATION_FUNCTION = "__semantic_view_drop_materialization";
//! (catalog, schema, name, if_exists BOOLEAN, materialization VARCHAR, mode VARCHAR - NULL: its REFRESH_MODE)
static constexpr const char *SEMANTIC_VIEW_REFRESH_MATERIALIZATION_FUNCTION = "__semantic_view_refresh_materialization";
//! (catalog, schema, name): name '' = the views of the current schema
static constexpr const char *SEMANTIC_VIEW_SHOW_MATERIALIZATIONS_FUNCTION = "__semantic_view_show_materializations";
//! (catalog, schema, name)
static constexpr const char *SEMANTIC_VIEW_DESCRIBE_FUNCTION = "__semantic_view_describe";
//! (catalog, schema): '' matches any
static constexpr const char *SEMANTIC_VIEW_SHOW_VIEWS_FUNCTION = "__semantic_view_show_views";
//! (kind VARCHAR, catalog, schema, name): name '' = the views of the current schema
static constexpr const char *SEMANTIC_VIEW_SHOW_MEMBERS_FUNCTION = "__semantic_view_show_members";
//! (catalog, schema, name, metric VARCHAR[] - the name parts)
static constexpr const char *SEMANTIC_VIEW_SHOW_FOR_METRIC_FUNCTION = "__semantic_view_show_dimensions_for_metric";
//! (query BLOB)
static constexpr const char *SEMANTIC_VIEW_QUERY_FUNCTION = "__semantic_view_query";

struct SemanticLowering {
	static unique_ptr<TableRef> Create(const ParsedSemanticView &view, OnCreateConflict on_conflict);
	static unique_ptr<TableRef> Drop(const QualifiedName &name, bool if_exists);
	static unique_ptr<TableRef> Rename(const QualifiedName &name, bool if_exists, const string &new_name);
	static unique_ptr<TableRef> SetComment(const QualifiedName &name, bool if_exists, const Value &comment);
	static unique_ptr<TableRef> SetMaxStaleness(const QualifiedName &name, bool if_exists, const Value &seconds);
	static unique_ptr<TableRef> AddMaterialization(const QualifiedName &name, bool if_exists,
	                                               const ParsedSemanticMaterialization &materialization,
	                                               bool if_not_exists);
	static unique_ptr<TableRef> DropMaterialization(const QualifiedName &name, bool if_exists,
	                                                const string &materialization, bool materialization_if_exists);
	static unique_ptr<TableRef> RefreshMaterialization(const QualifiedName &name, bool if_exists,
	                                                   const string &materialization, const Value &mode);
	static unique_ptr<TableRef> ShowMaterializations(const QualifiedName &view);
	static unique_ptr<TableRef> Describe(const QualifiedName &name);
	static unique_ptr<TableRef> ShowViews(const QualifiedName &scope);
	static unique_ptr<TableRef> ShowMembers(SemanticMemberKind kind, const QualifiedName &view);
	static unique_ptr<TableRef> ShowDimensionsForMetric(const QualifiedName &view, const vector<string> &metric);
	static unique_ptr<TableRef> Query(const ParsedSemanticQuery &query);

	//! Restricts a SHOW / DESCRIBE result to the rows whose `name` matches `pattern` (case-insensitive LIKE)
	static unique_ptr<TableRef> WhereNameLike(unique_ptr<TableRef> ref, const string &pattern);
	//! SELECT * FROM ref
	static unique_ptr<SQLStatement> SelectAll(unique_ptr<TableRef> ref);

	template <class T>
	static Value Encode(const T &payload) {
		MemoryStream stream;
		BinarySerializer::Serialize(payload, stream);
		return Value::BLOB(stream.GetData(), stream.GetPosition());
	}
	template <class T>
	static unique_ptr<T> Decode(const Value &blob) {
		auto data = StringValue::Get(blob);
		MemoryStream stream(data_ptr_cast(&data[0]), data.size());
		return BinaryDeserializer::Deserialize<T>(stream);
	}
};

} // namespace duckdb
