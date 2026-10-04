//===----------------------------------------------------------------------===//
//                         DuckDB - semantic_views
//
// parsed_semantic_view.hpp
//
// Parsed (unvalidated) semantic view definitions and SEMANTIC_VIEW(...) query
// specifications, as produced by the PEG transformers. Both are serializable:
// statements are lowered onto internal table functions that receive them as
// BLOB payloads (semantic_lowering.hpp).
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/parser/parsed_expression.hpp"
#include "duckdb/parser/qualified_name.hpp"
#include "duckdb/parser/result_modifier.hpp"
#include "duckdb/parser/tableref.hpp"

namespace duckdb {

class Serializer;
class Deserializer;

enum class SemanticMemberKind : uint8_t { FACT = 0, DIMENSION = 1, METRIC = 2 };

string SemanticMemberKindToString(SemanticMemberKind kind);

struct ParsedSemanticTag {
	string name;
	string value;

	void Serialize(Serializer &serializer) const;
	static ParsedSemanticTag Deserialize(Deserializer &deserializer);
};

struct ParsedSemanticRangeConstraint {
	string name;
	string start_column;
	string end_column;

	void Serialize(Serializer &serializer) const;
	static ParsedSemanticRangeConstraint Deserialize(Deserializer &deserializer);
};

struct ParsedSemanticTable {
	//! Logical table name (alias); empty when the definition did not give one
	string alias;
	//! BaseTableRef or SubqueryRef
	unique_ptr<TableRef> source;
	vector<string> primary_key;
	vector<vector<string>> unique_keys;
	vector<ParsedSemanticRangeConstraint> range_constraints;
	vector<string> synonyms;
	string comment;
	vector<ParsedSemanticTag> tags;

	void Serialize(Serializer &serializer) const;
	static ParsedSemanticTable Deserialize(Deserializer &deserializer);
};

struct ParsedSemanticRelationship {
	//! Empty when unnamed
	string name;
	string from_table;
	vector<string> from_columns;
	string to_table;
	//! Empty when the referenced columns are implied (primary key / single unique key)
	vector<string> to_columns;
	//! Index into to_columns of the ASOF column, or DConstants::INVALID_INDEX
	idx_t asof_index = DConstants::INVALID_INDEX;
	//! Range relationship: REFERENCES t (BETWEEN start AND end EXCLUSIVE)
	bool is_range = false;
	string range_start;
	string range_end;

	void Serialize(Serializer &serializer) const;
	static ParsedSemanticRelationship Deserialize(Deserializer &deserializer);
};

struct ParsedSemanticMember {
	SemanticMemberKind kind = SemanticMemberKind::DIMENSION;
	//! Owning logical table; empty for derived metrics
	string table;
	string name;
	bool is_private = false;
	vector<string> labels;
	vector<string> using_relationships;
	vector<OrderByNode> non_additive_by;
	unique_ptr<ParsedExpression> expression;
	vector<string> synonyms;
	string comment;
	vector<ParsedSemanticTag> tags;
	vector<string> sample_values;
	bool is_enum = false;
	string cortex_search_service;
	string cortex_search_column;

	string FullName() const;
	void Serialize(Serializer &serializer) const;
	static ParsedSemanticMember Deserialize(Deserializer &deserializer);
};

struct ParsedSemanticVerifiedQuery {
	string name;
	string question;
	string verified_at;
	bool onboarding_question = false;
	string verified_by;
	string sql;

	void Serialize(Serializer &serializer) const;
	static ParsedSemanticVerifiedQuery Deserialize(Deserializer &deserializer);
};

//! One entry of a DIMENSIONS / METRICS / FACTS list in SEMANTIC_VIEW(...)
struct ParsedSemanticQueryItem {
	SemanticMemberKind kind = SemanticMemberKind::DIMENSION;
	//! null for a wildcard (`table.*`)
	unique_ptr<ParsedExpression> expression;
	//! Logical table of a wildcard
	string star_table;
	string alias;

	void Serialize(Serializer &serializer) const;
	static ParsedSemanticQueryItem Deserialize(Deserializer &deserializer);
};

//! How REFRESH MATERIALIZATION brings a materialization up to date
enum class SemanticRefreshMode : uint8_t {
	//! incremental when the sources and metrics allow it, else a full rebuild
	AUTO = 0,
	FULL = 1,
	//! fails when incremental maintenance is not possible
	INCREMENTAL = 2
};

string SemanticRefreshModeToString(SemanticRefreshMode mode);

//! ALTER SEMANTIC VIEW ... ADD MATERIALIZATION <name> [REFRESH_MODE = ...] AS DIMENSIONS ... METRICS ...
struct ParsedSemanticMaterialization {
	string name;
	//! Dimension and metric items, as in SEMANTIC_VIEW(...)
	vector<ParsedSemanticQueryItem> items;
	SemanticRefreshMode refresh_mode = SemanticRefreshMode::AUTO;
	//! Accepted for compatibility, unused
	string warehouse;
	//! IN <database>[.<schema>]: where its tables are stored (empty: the semantic view's database)
	string storage_catalog;
	string storage_schema;

	ParsedSemanticMaterialization Copy() const;
	void Serialize(Serializer &serializer) const;
	static ParsedSemanticMaterialization Deserialize(Deserializer &deserializer);
};

//! The BLOB payload of ADD MATERIALIZATION
struct ParsedSemanticMaterializationPayload {
	ParsedSemanticMaterialization materialization;

	void Serialize(Serializer &serializer) const;
	static unique_ptr<ParsedSemanticMaterializationPayload> Deserialize(Deserializer &deserializer);
};

struct ParsedSemanticView {
	QualifiedName name;
	vector<ParsedSemanticTable> tables;
	vector<ParsedSemanticRelationship> relationships;
	vector<ParsedSemanticMember> facts;
	vector<ParsedSemanticMember> dimensions;
	vector<ParsedSemanticMember> metrics;
	string comment;
	bool has_max_staleness = false;
	int64_t max_staleness = 0;
	string ai_sql_generation;
	string ai_question_categorization;
	vector<ParsedSemanticVerifiedQuery> verified_queries;
	vector<ParsedSemanticTag> tags;
	bool copy_grants = false;
	vector<ParsedSemanticMaterialization> materializations;

	unique_ptr<ParsedSemanticView> Copy() const;
	void Serialize(Serializer &serializer) const;
	static unique_ptr<ParsedSemanticView> Deserialize(Deserializer &deserializer);
};

struct ParsedSemanticQuery {
	QualifiedName view;
	vector<ParsedSemanticQueryItem> items;
	unique_ptr<ParsedExpression> where;

	void Serialize(Serializer &serializer) const;
	static unique_ptr<ParsedSemanticQuery> Deserialize(Deserializer &deserializer);
};

} // namespace duckdb
