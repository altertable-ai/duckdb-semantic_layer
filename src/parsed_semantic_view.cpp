#include "duckdb/common/string_util.hpp"
#include "parsed_semantic_view.hpp"

#include "duckdb/common/exception.hpp"
#include "duckdb/common/serializer/binary_deserializer.hpp"
#include "duckdb/common/serializer/binary_serializer.hpp"
#include "duckdb/common/serializer/memory_stream.hpp"

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

string ParsedSemanticMember::FullName() const {
	return table.empty() ? name : table + "." + name;
}

string SemanticCumulativePeriodToString(SemanticCumulativePeriod period) {
	switch (period) {
	case SemanticCumulativePeriod::FIRST:
		return "FIRST";
	case SemanticCumulativePeriod::AVERAGE:
		return "AVERAGE";
	default:
		return "LAST";
	}
}

string ParsedSemanticMember::CumulativeToString() const {
	auto result = StringUtil::Join(cumulative_dimension, ".");
	if (cumulative_window) {
		result += " OVER " + cumulative_window->ToString();
	} else if (!cumulative_part.empty()) {
		result += " TO " + StringUtil::Upper(cumulative_part);
	}
	return result + " " + SemanticCumulativePeriodToString(cumulative_period);
}

unique_ptr<ParsedSemanticView> ParsedSemanticView::Copy() const {
	// a serialization round trip: the field list lives in Serialize / Deserialize only
	MemoryStream stream;
	BinarySerializer::Serialize(*this, stream);
	stream.Rewind();
	return BinaryDeserializer::Deserialize<ParsedSemanticView>(stream);
}

//===--------------------------------------------------------------------===//
// Serialization
//===--------------------------------------------------------------------===//
SemanticQualifiedName SemanticQualifiedName::FromQualifiedName(const QualifiedName &qualified_name) {
	if (qualified_name.Path().size() > 3) {
		throw ParserException("Expected a name of the form catalog.schema.name, got \"%s\"", qualified_name.ToString());
	}
	SemanticQualifiedName result;
	result.catalog = qualified_name.Catalog().GetIdentifierName();
	result.schema = qualified_name.Schema().GetIdentifierName();
	result.name = qualified_name.Name().GetIdentifierName();
	return result;
}

static void WriteName(Serializer &serializer, field_id_t first_field, const SemanticQualifiedName &name) {
	serializer.WriteProperty(first_field, "catalog", name.catalog);
	serializer.WriteProperty(first_field + 1, "schema", name.schema);
	serializer.WriteProperty(first_field + 2, "name", name.name);
}

static SemanticQualifiedName ReadName(Deserializer &deserializer, field_id_t first_field) {
	SemanticQualifiedName name;
	name.catalog = deserializer.ReadProperty<string>(first_field, "catalog");
	name.schema = deserializer.ReadProperty<string>(first_field + 1, "schema");
	name.name = deserializer.ReadProperty<string>(first_field + 2, "name");
	return name;
}

void ParsedSemanticTag::Serialize(Serializer &serializer) const {
	serializer.WriteProperty(100, "name", name);
	serializer.WriteProperty(101, "value", value);
}

ParsedSemanticTag ParsedSemanticTag::Deserialize(Deserializer &deserializer) {
	ParsedSemanticTag result;
	result.name = deserializer.ReadProperty<string>(100, "name");
	result.value = deserializer.ReadProperty<string>(101, "value");
	return result;
}

void ParsedSemanticRangeConstraint::Serialize(Serializer &serializer) const {
	serializer.WriteProperty(100, "name", name);
	serializer.WriteProperty(101, "start_column", start_column);
	serializer.WriteProperty(102, "end_column", end_column);
}

ParsedSemanticRangeConstraint ParsedSemanticRangeConstraint::Deserialize(Deserializer &deserializer) {
	ParsedSemanticRangeConstraint result;
	result.name = deserializer.ReadProperty<string>(100, "name");
	result.start_column = deserializer.ReadProperty<string>(101, "start_column");
	result.end_column = deserializer.ReadProperty<string>(102, "end_column");
	return result;
}

void ParsedSemanticTable::Serialize(Serializer &serializer) const {
	serializer.WriteProperty(100, "alias", alias);
	serializer.WritePropertyWithDefault(101, "source", source);
	serializer.WriteProperty(102, "primary_key", primary_key);
	serializer.WriteProperty(103, "unique_keys", unique_keys);
	serializer.WriteProperty(104, "range_constraints", range_constraints);
	serializer.WriteProperty(105, "synonyms", synonyms);
	serializer.WriteProperty(106, "comment", comment);
	serializer.WriteProperty(107, "tags", tags);
	serializer.WritePropertyWithDefault(108, "time_column", time_column);
}

ParsedSemanticTable ParsedSemanticTable::Deserialize(Deserializer &deserializer) {
	ParsedSemanticTable result;
	result.alias = deserializer.ReadProperty<string>(100, "alias");
	result.source = deserializer.ReadPropertyWithDefault<unique_ptr<TableRef>>(101, "source");
	result.primary_key = deserializer.ReadProperty<vector<string>>(102, "primary_key");
	result.unique_keys = deserializer.ReadProperty<vector<vector<string>>>(103, "unique_keys");
	result.range_constraints =
	    deserializer.ReadProperty<vector<ParsedSemanticRangeConstraint>>(104, "range_constraints");
	result.synonyms = deserializer.ReadProperty<vector<string>>(105, "synonyms");
	result.comment = deserializer.ReadProperty<string>(106, "comment");
	result.tags = deserializer.ReadProperty<vector<ParsedSemanticTag>>(107, "tags");
	result.time_column = deserializer.ReadPropertyWithDefault<string>(108, "time_column");
	return result;
}

void ParsedSemanticRelationship::Serialize(Serializer &serializer) const {
	serializer.WriteProperty(100, "name", name);
	serializer.WriteProperty(101, "from_table", from_table);
	serializer.WriteProperty(102, "from_columns", from_columns);
	serializer.WriteProperty(103, "to_table", to_table);
	serializer.WriteProperty(104, "to_columns", to_columns);
	serializer.WriteProperty<uint64_t>(105, "asof_index", asof_index);
	serializer.WriteProperty(106, "is_range", is_range);
	serializer.WriteProperty(107, "range_start", range_start);
	serializer.WriteProperty(108, "range_end", range_end);
}

ParsedSemanticRelationship ParsedSemanticRelationship::Deserialize(Deserializer &deserializer) {
	ParsedSemanticRelationship result;
	result.name = deserializer.ReadProperty<string>(100, "name");
	result.from_table = deserializer.ReadProperty<string>(101, "from_table");
	result.from_columns = deserializer.ReadProperty<vector<string>>(102, "from_columns");
	result.to_table = deserializer.ReadProperty<string>(103, "to_table");
	result.to_columns = deserializer.ReadProperty<vector<string>>(104, "to_columns");
	result.asof_index = deserializer.ReadProperty<uint64_t>(105, "asof_index");
	result.is_range = deserializer.ReadProperty<bool>(106, "is_range");
	result.range_start = deserializer.ReadProperty<string>(107, "range_start");
	result.range_end = deserializer.ReadProperty<string>(108, "range_end");
	return result;
}

void ParsedSemanticMember::Serialize(Serializer &serializer) const {
	serializer.WriteProperty<uint8_t>(100, "kind", static_cast<uint8_t>(kind));
	serializer.WriteProperty(101, "table", table);
	serializer.WriteProperty(102, "name", name);
	serializer.WriteProperty(103, "is_private", is_private);
	serializer.WriteProperty(104, "labels", labels);
	serializer.WriteProperty(105, "using_relationships", using_relationships);
	serializer.WriteProperty(106, "non_additive_by", non_additive_by);
	serializer.WritePropertyWithDefault(107, "expression", expression);
	serializer.WriteProperty(108, "synonyms", synonyms);
	serializer.WriteProperty(109, "comment", comment);
	serializer.WriteProperty(110, "tags", tags);
	serializer.WriteProperty(111, "sample_values", sample_values);
	serializer.WriteProperty(112, "is_enum", is_enum);
	serializer.WriteProperty(113, "cortex_search_service", cortex_search_service);
	serializer.WriteProperty(114, "cortex_search_column", cortex_search_column);
	serializer.WritePropertyWithDefault(115, "filter", filter);
	serializer.WritePropertyWithDefault(116, "offset_dimension", offset_dimension);
	serializer.WritePropertyWithDefault(117, "offset_interval", offset_interval);
	serializer.WritePropertyWithDefault(118, "cumulative_dimension", cumulative_dimension);
	serializer.WritePropertyWithDefault(119, "cumulative_window", cumulative_window);
	serializer.WritePropertyWithDefault(120, "cumulative_part", cumulative_part);
	serializer.WritePropertyWithDefault<uint8_t>(121, "cumulative_period", static_cast<uint8_t>(cumulative_period), 0);
}

ParsedSemanticMember ParsedSemanticMember::Deserialize(Deserializer &deserializer) {
	ParsedSemanticMember result;
	result.kind = static_cast<SemanticMemberKind>(deserializer.ReadProperty<uint8_t>(100, "kind"));
	result.table = deserializer.ReadProperty<string>(101, "table");
	result.name = deserializer.ReadProperty<string>(102, "name");
	result.is_private = deserializer.ReadProperty<bool>(103, "is_private");
	result.labels = deserializer.ReadProperty<vector<string>>(104, "labels");
	result.using_relationships = deserializer.ReadProperty<vector<string>>(105, "using_relationships");
	result.non_additive_by = deserializer.ReadProperty<vector<OrderByNode>>(106, "non_additive_by");
	result.expression = deserializer.ReadPropertyWithDefault<unique_ptr<ParsedExpression>>(107, "expression");
	result.synonyms = deserializer.ReadProperty<vector<string>>(108, "synonyms");
	result.comment = deserializer.ReadProperty<string>(109, "comment");
	result.tags = deserializer.ReadProperty<vector<ParsedSemanticTag>>(110, "tags");
	result.sample_values = deserializer.ReadProperty<vector<string>>(111, "sample_values");
	result.is_enum = deserializer.ReadProperty<bool>(112, "is_enum");
	result.cortex_search_service = deserializer.ReadProperty<string>(113, "cortex_search_service");
	result.cortex_search_column = deserializer.ReadProperty<string>(114, "cortex_search_column");
	result.filter = deserializer.ReadPropertyWithDefault<unique_ptr<ParsedExpression>>(115, "filter");
	result.offset_dimension = deserializer.ReadPropertyWithDefault<vector<string>>(116, "offset_dimension");
	result.offset_interval = deserializer.ReadPropertyWithDefault<unique_ptr<ParsedExpression>>(117, "offset_interval");
	result.cumulative_dimension = deserializer.ReadPropertyWithDefault<vector<string>>(118, "cumulative_dimension");
	result.cumulative_window =
	    deserializer.ReadPropertyWithDefault<unique_ptr<ParsedExpression>>(119, "cumulative_window");
	result.cumulative_part = deserializer.ReadPropertyWithDefault<string>(120, "cumulative_part");
	result.cumulative_period = static_cast<SemanticCumulativePeriod>(
	    deserializer.ReadPropertyWithExplicitDefault<uint8_t>(121, "cumulative_period", 0));
	return result;
}

void ParsedSemanticVerifiedQuery::Serialize(Serializer &serializer) const {
	serializer.WriteProperty(100, "name", name);
	serializer.WriteProperty(101, "question", question);
	serializer.WriteProperty(102, "verified_at", verified_at);
	serializer.WriteProperty(103, "onboarding_question", onboarding_question);
	serializer.WriteProperty(104, "verified_by", verified_by);
	serializer.WriteProperty(105, "sql", sql);
}

ParsedSemanticVerifiedQuery ParsedSemanticVerifiedQuery::Deserialize(Deserializer &deserializer) {
	ParsedSemanticVerifiedQuery result;
	result.name = deserializer.ReadProperty<string>(100, "name");
	result.question = deserializer.ReadProperty<string>(101, "question");
	result.verified_at = deserializer.ReadProperty<string>(102, "verified_at");
	result.onboarding_question = deserializer.ReadProperty<bool>(103, "onboarding_question");
	result.verified_by = deserializer.ReadProperty<string>(104, "verified_by");
	result.sql = deserializer.ReadProperty<string>(105, "sql");
	return result;
}

void ParsedSemanticView::Serialize(Serializer &serializer) const {
	WriteName(serializer, 100, name);
	serializer.WriteProperty(103, "tables", tables);
	serializer.WriteProperty(104, "relationships", relationships);
	serializer.WriteProperty(105, "facts", facts);
	serializer.WriteProperty(106, "dimensions", dimensions);
	serializer.WriteProperty(107, "metrics", metrics);
	serializer.WriteProperty(108, "comment", comment);
	serializer.WriteProperty(109, "has_max_staleness", has_max_staleness);
	serializer.WriteProperty(110, "max_staleness", max_staleness);
	serializer.WriteProperty(111, "ai_sql_generation", ai_sql_generation);
	serializer.WriteProperty(112, "ai_question_categorization", ai_question_categorization);
	serializer.WriteProperty(113, "verified_queries", verified_queries);
	serializer.WriteProperty(114, "tags", tags);
	serializer.WriteProperty(115, "copy_grants", copy_grants);
	serializer.WritePropertyWithDefault(116, "materializations", materializations);
}

unique_ptr<ParsedSemanticView> ParsedSemanticView::Deserialize(Deserializer &deserializer) {
	auto result = make_uniq<ParsedSemanticView>();
	result->name = ReadName(deserializer, 100);
	result->tables = deserializer.ReadProperty<vector<ParsedSemanticTable>>(103, "tables");
	result->relationships = deserializer.ReadProperty<vector<ParsedSemanticRelationship>>(104, "relationships");
	result->facts = deserializer.ReadProperty<vector<ParsedSemanticMember>>(105, "facts");
	result->dimensions = deserializer.ReadProperty<vector<ParsedSemanticMember>>(106, "dimensions");
	result->metrics = deserializer.ReadProperty<vector<ParsedSemanticMember>>(107, "metrics");
	result->comment = deserializer.ReadProperty<string>(108, "comment");
	result->has_max_staleness = deserializer.ReadProperty<bool>(109, "has_max_staleness");
	result->max_staleness = deserializer.ReadProperty<int64_t>(110, "max_staleness");
	result->ai_sql_generation = deserializer.ReadProperty<string>(111, "ai_sql_generation");
	result->ai_question_categorization = deserializer.ReadProperty<string>(112, "ai_question_categorization");
	result->verified_queries = deserializer.ReadProperty<vector<ParsedSemanticVerifiedQuery>>(113, "verified_queries");
	result->tags = deserializer.ReadProperty<vector<ParsedSemanticTag>>(114, "tags");
	result->copy_grants = deserializer.ReadProperty<bool>(115, "copy_grants");
	result->materializations =
	    deserializer.ReadPropertyWithDefault<vector<ParsedSemanticMaterialization>>(116, "materializations");
	return result;
}

string SemanticRefreshModeToString(SemanticRefreshMode mode) {
	switch (mode) {
	case SemanticRefreshMode::FULL:
		return "FULL";
	case SemanticRefreshMode::INCREMENTAL:
		return "INCREMENTAL";
	default:
		return "AUTO";
	}
}

void ParsedSemanticMaterialization::Serialize(Serializer &serializer) const {
	serializer.WriteProperty(100, "name", name);
	serializer.WriteProperty(101, "items", items);
	serializer.WriteProperty<uint8_t>(102, "refresh_mode", static_cast<uint8_t>(refresh_mode));
	serializer.WriteProperty(103, "warehouse", warehouse);
	serializer.WritePropertyWithDefault<string>(104, "storage_catalog", storage_catalog);
	serializer.WritePropertyWithDefault<string>(105, "storage_schema", storage_schema);
}

ParsedSemanticMaterialization ParsedSemanticMaterialization::Copy() const {
	ParsedSemanticMaterialization result;
	result.name = name;
	result.refresh_mode = refresh_mode;
	result.warehouse = warehouse;
	result.storage_catalog = storage_catalog;
	result.storage_schema = storage_schema;
	for (auto &item : items) {
		ParsedSemanticQueryItem copy;
		copy.kind = item.kind;
		copy.expression = item.expression ? item.expression->Copy() : nullptr;
		copy.star_table = item.star_table;
		copy.alias = item.alias;
		result.items.push_back(std::move(copy));
	}
	return result;
}

void ParsedSemanticMaterializationPayload::Serialize(Serializer &serializer) const {
	serializer.WriteProperty(100, "materialization", materialization);
}

unique_ptr<ParsedSemanticMaterializationPayload>
ParsedSemanticMaterializationPayload::Deserialize(Deserializer &deserializer) {
	auto result = make_uniq<ParsedSemanticMaterializationPayload>();
	result->materialization = deserializer.ReadProperty<ParsedSemanticMaterialization>(100, "materialization");
	return result;
}

ParsedSemanticMaterialization ParsedSemanticMaterialization::Deserialize(Deserializer &deserializer) {
	ParsedSemanticMaterialization result;
	result.name = deserializer.ReadProperty<string>(100, "name");
	result.items = deserializer.ReadProperty<vector<ParsedSemanticQueryItem>>(101, "items");
	result.refresh_mode = static_cast<SemanticRefreshMode>(deserializer.ReadProperty<uint8_t>(102, "refresh_mode"));
	result.warehouse = deserializer.ReadProperty<string>(103, "warehouse");
	result.storage_catalog = deserializer.ReadPropertyWithDefault<string>(104, "storage_catalog");
	result.storage_schema = deserializer.ReadPropertyWithDefault<string>(105, "storage_schema");
	return result;
}

void ParsedSemanticQueryItem::Serialize(Serializer &serializer) const {
	serializer.WriteProperty<uint8_t>(100, "kind", static_cast<uint8_t>(kind));
	serializer.WritePropertyWithDefault(101, "expression", expression);
	serializer.WriteProperty(102, "star_table", star_table);
	serializer.WriteProperty(103, "alias", alias);
}

ParsedSemanticQueryItem ParsedSemanticQueryItem::Deserialize(Deserializer &deserializer) {
	ParsedSemanticQueryItem result;
	result.kind = static_cast<SemanticMemberKind>(deserializer.ReadProperty<uint8_t>(100, "kind"));
	result.expression = deserializer.ReadPropertyWithDefault<unique_ptr<ParsedExpression>>(101, "expression");
	result.star_table = deserializer.ReadProperty<string>(102, "star_table");
	result.alias = deserializer.ReadProperty<string>(103, "alias");
	return result;
}

void ParsedSemanticQuery::Serialize(Serializer &serializer) const {
	WriteName(serializer, 100, view);
	serializer.WriteProperty(103, "items", items);
	serializer.WritePropertyWithDefault(104, "where", where);
}

unique_ptr<ParsedSemanticQuery> ParsedSemanticQuery::Deserialize(Deserializer &deserializer) {
	auto result = make_uniq<ParsedSemanticQuery>();
	result->view = ReadName(deserializer, 100);
	result->items = deserializer.ReadProperty<vector<ParsedSemanticQueryItem>>(103, "items");
	result->where = deserializer.ReadPropertyWithDefault<unique_ptr<ParsedExpression>>(104, "where");
	return result;
}

} // namespace duckdb
