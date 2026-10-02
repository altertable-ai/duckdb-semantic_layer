//===----------------------------------------------------------------------===//
//                         DuckDB - semantic_views
//
// parsed_semantic_view.hpp
//
// Parsed (unvalidated) semantic view definitions and SEMANTIC_VIEW(...) query
// specifications, as produced by the PEG transformers.
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/types/value.hpp"
#include "duckdb/parser/parsed_expression.hpp"
#include "duckdb/parser/qualified_name.hpp"
#include "duckdb/parser/query_node.hpp"
#include "duckdb/parser/result_modifier.hpp"
#include "duckdb/parser/tableref.hpp"

namespace duckdb {

//! Internal table functions the parser lowers semantic-view statements onto
static constexpr const char *SEMANTIC_VIEW_DDL_FUNCTION = "__semantic_view_ddl";
static constexpr const char *SEMANTIC_VIEW_SHOW_FUNCTION = "__semantic_view_show";
static constexpr const char *SEMANTIC_VIEW_QUERY_FUNCTION = "__semantic_view_query";

enum class SemanticMemberKind : uint8_t { FACT = 0, DIMENSION = 1, METRIC = 2 };

string SemanticMemberKindToString(SemanticMemberKind kind);

struct ParsedSemanticRangeConstraint {
	string name;
	string start_column;
	string end_column;
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
	vector<pair<string, string>> tags;

	ParsedSemanticTable Copy() const;
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
	vector<pair<string, string>> tags;
	vector<string> sample_values;
	bool is_enum = false;
	string cortex_search_service;
	string cortex_search_column;

	ParsedSemanticMember Copy() const;
	string FullName() const;
};

struct ParsedSemanticVerifiedQuery {
	string name;
	//! (property keyword, rendered value) pairs in definition order
	vector<pair<string, string>> properties;
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
	vector<pair<string, string>> tags;
	bool copy_grants = false;

	unique_ptr<ParsedSemanticView> Copy() const;
	//! Canonical, re-parseable `CREATE SEMANTIC VIEW` statement (without OR REPLACE / IF NOT EXISTS)
	string ToSQL() const;
};

//! One entry of a DIMENSIONS / METRICS / FACTS list in SEMANTIC_VIEW(...)
struct ParsedSemanticQueryItem {
	SemanticMemberKind kind = SemanticMemberKind::DIMENSION;
	//! null for a wildcard (`table.*`)
	unique_ptr<ParsedExpression> expression;
	//! Logical table of a wildcard
	string star_table;
	string alias;

	ParsedSemanticQueryItem Copy() const;
};

struct ParsedSemanticQuery {
	QualifiedName view;
	vector<ParsedSemanticQueryItem> items;
	unique_ptr<ParsedExpression> where;

	//! Encodes the items as LIST(STRUCT(kind VARCHAR, expr VARCHAR, alias VARCHAR)) for the internal function
	Value ItemsToValue() const;
	//! Decodes ItemsToValue() output (expressions are re-parsed)
	static vector<ParsedSemanticQueryItem> ItemsFromValue(const Value &value);
	static LogicalType ItemsType();
	//! Renders the SEMANTIC_VIEW(...) clause (used in error messages and debugging)
	string ToString() const;
};

//! Quotes an identifier when needed
string SemanticQuoteIdentifier(const string &identifier);
//! Renders a SQL string literal
string SemanticQuoteString(const string &text);

} // namespace duckdb
