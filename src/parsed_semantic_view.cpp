#include "parsed_semantic_view.hpp"

#include "duckdb/common/exception.hpp"
#include "duckdb/common/exception/binder_exception.hpp"
#include "duckdb/common/exception/catalog_exception.hpp"
#include "duckdb/common/to_string.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/common/types/value.hpp"
#include "duckdb/parser/keyword_helper.hpp"
#include "duckdb/parser/parser.hpp"
#include "duckdb/parser/tableref/basetableref.hpp"
#include "duckdb/parser/tableref/subqueryref.hpp"

namespace duckdb {

string SemanticMemberKindToString(SemanticMemberKind kind) {
	switch (kind) {
	case SemanticMemberKind::FACT:
		return "FACT";
	case SemanticMemberKind::DIMENSION:
		return "DIMENSION";
	case SemanticMemberKind::METRIC:
		return "METRIC";
	}
	throw InternalException("unknown SemanticMemberKind");
}

static SemanticMemberKind SemanticMemberKindFromString(const string &text) {
	if (StringUtil::CIEquals(text, "FACT")) {
		return SemanticMemberKind::FACT;
	}
	if (StringUtil::CIEquals(text, "DIMENSION")) {
		return SemanticMemberKind::DIMENSION;
	}
	if (StringUtil::CIEquals(text, "METRIC")) {
		return SemanticMemberKind::METRIC;
	}
	throw InternalException("unknown semantic member kind '%s'", text);
}

string SemanticQuoteIdentifier(const string &identifier) {
	return KeywordHelper::WriteOptionallyQuoted(identifier);
}

string SemanticQuoteString(const string &text) {
	return KeywordHelper::WriteQuoted(text, '\'');
}

//===--------------------------------------------------------------------===//
// Copy
//===--------------------------------------------------------------------===//
ParsedSemanticTable ParsedSemanticTable::Copy() const {
	ParsedSemanticTable result;
	result.alias = alias;
	result.source = source ? source->Copy() : nullptr;
	result.primary_key = primary_key;
	result.unique_keys = unique_keys;
	result.range_constraints = range_constraints;
	result.synonyms = synonyms;
	result.comment = comment;
	result.tags = tags;
	return result;
}

ParsedSemanticMember ParsedSemanticMember::Copy() const {
	ParsedSemanticMember result;
	result.kind = kind;
	result.table = table;
	result.name = name;
	result.is_private = is_private;
	result.labels = labels;
	result.using_relationships = using_relationships;
	for (auto &order : non_additive_by) {
		result.non_additive_by.emplace_back(order.type, order.null_order, order.expression->Copy());
	}
	result.expression = expression ? expression->Copy() : nullptr;
	result.synonyms = synonyms;
	result.comment = comment;
	result.tags = tags;
	result.sample_values = sample_values;
	result.is_enum = is_enum;
	result.cortex_search_service = cortex_search_service;
	result.cortex_search_column = cortex_search_column;
	return result;
}

string ParsedSemanticMember::FullName() const {
	return table.empty() ? name : table + "." + name;
}

unique_ptr<ParsedSemanticView> ParsedSemanticView::Copy() const {
	auto result = make_uniq<ParsedSemanticView>();
	result->name = name;
	for (auto &table : tables) {
		result->tables.push_back(table.Copy());
	}
	result->relationships = relationships;
	for (auto &member : facts) {
		result->facts.push_back(member.Copy());
	}
	for (auto &member : dimensions) {
		result->dimensions.push_back(member.Copy());
	}
	for (auto &member : metrics) {
		result->metrics.push_back(member.Copy());
	}
	result->comment = comment;
	result->has_max_staleness = has_max_staleness;
	result->max_staleness = max_staleness;
	result->ai_sql_generation = ai_sql_generation;
	result->ai_question_categorization = ai_question_categorization;
	result->verified_queries = verified_queries;
	result->tags = tags;
	result->copy_grants = copy_grants;
	return result;
}

ParsedSemanticQueryItem ParsedSemanticQueryItem::Copy() const {
	ParsedSemanticQueryItem result;
	result.kind = kind;
	result.expression = expression ? expression->Copy() : nullptr;
	result.star_table = star_table;
	result.alias = alias;
	return result;
}

//===--------------------------------------------------------------------===//
// ToSQL
//===--------------------------------------------------------------------===//
static string QuotedList(const vector<string> &identifiers) {
	string result = "(";
	for (idx_t i = 0; i < identifiers.size(); i++) {
		if (i > 0) {
			result += ", ";
		}
		result += SemanticQuoteIdentifier(identifiers[i]);
	}
	return result + ")";
}

static string StringList(const vector<string> &values) {
	string result = "(";
	for (idx_t i = 0; i < values.size(); i++) {
		if (i > 0) {
			result += ", ";
		}
		result += SemanticQuoteString(values[i]);
	}
	return result + ")";
}

static string TagList(const vector<pair<string, string>> &tags) {
	string result = "TAG (";
	for (idx_t i = 0; i < tags.size(); i++) {
		if (i > 0) {
			result += ", ";
		}
		result += tags[i].first + " = " + SemanticQuoteString(tags[i].second);
	}
	return result + ")";
}

static string QualifiedNameToSQL(const QualifiedName &name) {
	string result;
	if (!name.catalog.empty() && name.catalog != INVALID_CATALOG) {
		result += SemanticQuoteIdentifier(name.catalog) + ".";
	}
	if (!name.schema.empty() && name.schema != INVALID_SCHEMA) {
		result += SemanticQuoteIdentifier(name.schema) + ".";
	}
	return result + SemanticQuoteIdentifier(name.name);
}

static string TableSourceToSQL(const TableRef &source) {
	if (source.type == TableReferenceType::BASE_TABLE) {
		auto &base = source.Cast<BaseTableRef>();
		QualifiedName name;
		name.catalog = base.catalog_name;
		name.schema = base.schema_name;
		name.name = base.table_name;
		return QualifiedNameToSQL(name);
	}
	if (source.type == TableReferenceType::SUBQUERY) {
		auto &subquery = source.Cast<SubqueryRef>();
		return "(" + subquery.subquery->ToString() + ")";
	}
	throw InternalException("unsupported semantic view table source");
}

static void AppendCommonAnnotations(string &sql, const vector<string> &synonyms, const vector<pair<string, string>> &tags,
                                    const string &comment) {
	if (!synonyms.empty()) {
		sql += " WITH SYNONYMS " + StringList(synonyms);
	}
	if (!tags.empty()) {
		sql += " WITH " + TagList(tags);
	}
	if (!comment.empty()) {
		sql += " COMMENT = " + SemanticQuoteString(comment);
	}
}

static string MemberToSQL(const ParsedSemanticMember &member) {
	string sql;
	if (member.is_private) {
		sql += "PRIVATE ";
	}
	if (!member.table.empty()) {
		sql += SemanticQuoteIdentifier(member.table) + ".";
	}
	sql += SemanticQuoteIdentifier(member.name);
	if (!member.labels.empty()) {
		sql += " LABELS = " + QuotedList(member.labels);
	}
	if (!member.using_relationships.empty()) {
		sql += " USING " + QuotedList(member.using_relationships);
	}
	if (!member.non_additive_by.empty()) {
		sql += " NON ADDITIVE BY (";
		for (idx_t i = 0; i < member.non_additive_by.size(); i++) {
			if (i > 0) {
				sql += ", ";
			}
			sql += member.non_additive_by[i].ToString();
		}
		sql += ")";
	}
	sql += " AS " + member.expression->ToString();
	AppendCommonAnnotations(sql, member.synonyms, member.tags, member.comment);
	if (!member.sample_values.empty()) {
		sql += " SAMPLE_VALUES " + StringList(member.sample_values);
	}
	if (member.is_enum) {
		sql += " IS_ENUM";
	}
	if (!member.cortex_search_service.empty()) {
		sql += " WITH CORTEX SEARCH SERVICE " + member.cortex_search_service;
		if (!member.cortex_search_column.empty()) {
			sql += " USING " + SemanticQuoteIdentifier(member.cortex_search_column);
		}
	}
	return sql;
}

static void AppendMembers(string &sql, const char *clause, const vector<ParsedSemanticMember> &members) {
	if (members.empty()) {
		return;
	}
	sql += "\n  ";
	sql += clause;
	sql += " (";
	for (idx_t i = 0; i < members.size(); i++) {
		sql += i == 0 ? "\n    " : ",\n    ";
		sql += MemberToSQL(members[i]);
	}
	sql += "\n  )";
}

string ParsedSemanticView::ToSQL() const {
	string sql = "CREATE SEMANTIC VIEW " + QualifiedNameToSQL(name);
	sql += "\n  TABLES (";
	for (idx_t i = 0; i < tables.size(); i++) {
		auto &table = tables[i];
		sql += i == 0 ? "\n    " : ",\n    ";
		if (!table.alias.empty()) {
			sql += SemanticQuoteIdentifier(table.alias) + " AS ";
		}
		sql += TableSourceToSQL(*table.source);
		if (!table.primary_key.empty()) {
			sql += " PRIMARY KEY " + QuotedList(table.primary_key);
		}
		for (auto &unique : table.unique_keys) {
			sql += " UNIQUE " + QuotedList(unique);
		}
		for (auto &range : table.range_constraints) {
			sql += " CONSTRAINT ";
			if (!range.name.empty()) {
				sql += SemanticQuoteIdentifier(range.name) + " ";
			}
			sql += "DISTINCT RANGE BETWEEN " + SemanticQuoteIdentifier(range.start_column) + " AND " +
			       SemanticQuoteIdentifier(range.end_column) + " EXCLUSIVE";
		}
		AppendCommonAnnotations(sql, table.synonyms, table.tags, table.comment);
	}
	sql += "\n  )";
	if (!relationships.empty()) {
		sql += "\n  RELATIONSHIPS (";
		for (idx_t i = 0; i < relationships.size(); i++) {
			auto &rel = relationships[i];
			sql += i == 0 ? "\n    " : ",\n    ";
			if (!rel.name.empty()) {
				sql += SemanticQuoteIdentifier(rel.name) + " AS ";
			}
			sql += SemanticQuoteIdentifier(rel.from_table) + " " + QuotedList(rel.from_columns) + " REFERENCES " +
			       SemanticQuoteIdentifier(rel.to_table);
			if (rel.is_range) {
				sql += " (BETWEEN " + SemanticQuoteIdentifier(rel.range_start) + " AND " +
				       SemanticQuoteIdentifier(rel.range_end) + " EXCLUSIVE)";
			} else if (!rel.to_columns.empty()) {
				sql += " (";
				for (idx_t c = 0; c < rel.to_columns.size(); c++) {
					if (c > 0) {
						sql += ", ";
					}
					if (c == rel.asof_index) {
						sql += "ASOF ";
					}
					sql += SemanticQuoteIdentifier(rel.to_columns[c]);
				}
				sql += ")";
			}
		}
		sql += "\n  )";
	}
	AppendMembers(sql, "FACTS", facts);
	AppendMembers(sql, "DIMENSIONS", dimensions);
	AppendMembers(sql, "METRICS", metrics);
	if (!comment.empty()) {
		sql += "\n  COMMENT = " + SemanticQuoteString(comment);
	}
	if (has_max_staleness) {
		sql += "\n  MAX_STALENESS = " + to_string(max_staleness);
	}
	if (!ai_sql_generation.empty()) {
		sql += "\n  AI_SQL_GENERATION " + SemanticQuoteString(ai_sql_generation);
	}
	if (!ai_question_categorization.empty()) {
		sql += "\n  AI_QUESTION_CATEGORIZATION " + SemanticQuoteString(ai_question_categorization);
	}
	if (!verified_queries.empty()) {
		sql += "\n  AI_VERIFIED_QUERIES (";
		for (idx_t i = 0; i < verified_queries.size(); i++) {
			sql += i == 0 ? "\n    " : ",\n    ";
			sql += SemanticQuoteIdentifier(verified_queries[i].name) + " AS (";
			for (idx_t p = 0; p < verified_queries[i].properties.size(); p++) {
				auto &property = verified_queries[i].properties[p];
				sql += (p == 0 ? "" : " ") + property.first + " " + property.second;
			}
			sql += ")";
		}
		sql += "\n  )";
	}
	if (!tags.empty()) {
		sql += "\n  WITH " + TagList(tags);
	}
	if (copy_grants) {
		sql += "\n  COPY GRANTS";
	}
	return sql;
}

//===--------------------------------------------------------------------===//
// Query items <-> Value
//===--------------------------------------------------------------------===//
LogicalType ParsedSemanticQuery::ItemsType() {
	child_list_t<LogicalType> children;
	children.emplace_back("kind", LogicalType::VARCHAR);
	children.emplace_back("expr", LogicalType::VARCHAR);
	children.emplace_back("star_table", LogicalType::VARCHAR);
	children.emplace_back("alias", LogicalType::VARCHAR);
	return LogicalType::LIST(LogicalType::STRUCT(std::move(children)));
}

Value ParsedSemanticQuery::ItemsToValue() const {
	auto item_type = ListType::GetChildType(ItemsType());
	vector<Value> values;
	for (auto &item : items) {
		child_list_t<Value> fields;
		fields.emplace_back("kind", Value(SemanticMemberKindToString(item.kind)));
		fields.emplace_back("expr", item.expression ? Value(item.expression->ToString()) : Value());
		fields.emplace_back("star_table", item.expression ? Value() : Value(item.star_table));
		fields.emplace_back("alias", item.alias.empty() ? Value() : Value(item.alias));
		values.push_back(Value::STRUCT(std::move(fields)));
	}
	return Value::LIST(item_type, std::move(values));
}

vector<ParsedSemanticQueryItem> ParsedSemanticQuery::ItemsFromValue(const Value &value) {
	vector<ParsedSemanticQueryItem> result;
	if (value.IsNull()) {
		return result;
	}
	for (auto &entry : ListValue::GetChildren(value)) {
		auto &fields = StructValue::GetChildren(entry);
		ParsedSemanticQueryItem item;
		item.kind = SemanticMemberKindFromString(fields[0].ToString());
		if (!fields[1].IsNull()) {
			auto expressions = Parser::ParseExpressionList(fields[1].ToString());
			if (expressions.size() != 1) {
				throw InternalException("semantic view query item did not round-trip: %s", fields[1].ToString());
			}
			item.expression = std::move(expressions[0]);
		} else {
			item.star_table = fields[2].ToString();
		}
		if (!fields[3].IsNull()) {
			item.alias = fields[3].ToString();
		}
		result.push_back(std::move(item));
	}
	return result;
}

string ParsedSemanticQuery::ToString() const {
	string result = "SEMANTIC_VIEW(" + QualifiedNameToSQL(view);
	SemanticMemberKind current = SemanticMemberKind::DIMENSION;
	bool first_in_clause = true;
	for (idx_t i = 0; i < items.size(); i++) {
		auto &item = items[i];
		if (i == 0 || item.kind != current) {
			current = item.kind;
			result += " " + SemanticMemberKindToString(item.kind) + "S ";
			first_in_clause = true;
		}
		if (!first_in_clause) {
			result += ", ";
		}
		first_in_clause = false;
		result += item.expression ? item.expression->ToString() : SemanticQuoteIdentifier(item.star_table) + ".*";
		if (!item.alias.empty()) {
			result += " AS " + SemanticQuoteIdentifier(item.alias);
		}
	}
	if (where) {
		result += " WHERE " + where->ToString();
	}
	return result + ")";
}

} // namespace duckdb
