#include "duckdb/common/exception/binder_exception.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/common/to_string.hpp"
#include "duckdb/main/client_context.hpp"
#include "duckdb/main/extension/extension_loader.hpp"
#include "semantic_analysis.hpp"
#include "semantic_bridge.hpp"
#include "semantic_lowering.hpp"
#include "semantic_materialization.hpp"
#include "semantic_view_registry.hpp"
#include "semantic_views_functions.hpp"

namespace duckdb {

static Value OptionalString(const string &value) {
	return value.empty() ? Value(LogicalType::VARCHAR) : Value(value);
}

static Value StringList(const vector<string> &values) {
	if (values.empty()) {
		return Value(LogicalType::LIST(LogicalType::VARCHAR));
	}
	vector<Value> list;
	for (auto &value : values) {
		list.emplace_back(value);
	}
	return Value::LIST(LogicalType::VARCHAR, std::move(list));
}

static Value TypeName(const SemanticMember &member) {
	return member.type.id() == LogicalTypeId::UNKNOWN ? Value(LogicalType::VARCHAR) : Value(member.type.ToString());
}

static Value TableName(const SemanticModel &model, const SemanticMember &member) {
	return member.IsDerived() ? Value(LogicalType::VARCHAR) : Value(model.tables[member.table].name);
}

//===--------------------------------------------------------------------===//
// SHOW SEMANTIC VIEWS [IN SCHEMA s | IN DATABASE d]
//===--------------------------------------------------------------------===//
static unique_ptr<FunctionData> ShowViewsBind(ClientContext &context, TableFunctionBindInput &input,
                                              vector<LogicalType> &return_types, vector<string> &names) {
	names = {"created_on", "name", "database_name", "schema_name", "comment"};
	return_types = {LogicalType::TIMESTAMP_TZ, LogicalType::VARCHAR, LogicalType::VARCHAR, LogicalType::VARCHAR,
	                LogicalType::VARCHAR};
	auto catalog = StringArgument(input, 0);
	auto schema = StringArgument(input, 1);
	if (!schema.empty()) {
		auto resolved = SemanticViewRegistry::ResolveSchema(context, catalog, schema);
		catalog = resolved.catalog;
		schema = resolved.schema;
	}
	auto result = make_uniq<RowsFunctionData>();
	for (auto &model : GetSemanticViewsState(input).registry.List()) {
		auto &name = model->Name();
		if ((!catalog.empty() && !StringUtil::CIEquals(name.catalog, catalog)) ||
		    (!schema.empty() && !StringUtil::CIEquals(name.schema, schema))) {
			continue;
		}
		result->rows.push_back({Value::TIMESTAMPTZ(model->created_on), Value(name.name), Value(name.catalog),
		                        Value(name.schema), OptionalString(model->Comment())});
	}
	return std::move(result);
}

//===--------------------------------------------------------------------===//
// SHOW SEMANTIC DIMENSIONS | METRICS | FACTS [IN view]
//===--------------------------------------------------------------------===//
static bool InDefaultSchema(ClientContext &context, const SemanticModel &model) {
	auto current = SemanticViewRegistry::ResolveSchema(context, string(), string());
	return StringUtil::CIEquals(model.Name().catalog, current.catalog) &&
	       StringUtil::CIEquals(model.Name().schema, current.schema);
}

static unique_ptr<FunctionData> ShowMembersBind(ClientContext &context, TableFunctionBindInput &input,
                                                vector<LogicalType> &return_types, vector<string> &names) {
	names = {"database_name", "schema_name", "semantic_view_name", "table_name",
	         "name",          "data_type",   "synonyms",           "comment"};
	return_types = {LogicalType::VARCHAR,
	                LogicalType::VARCHAR,
	                LogicalType::VARCHAR,
	                LogicalType::VARCHAR,
	                LogicalType::VARCHAR,
	                LogicalType::VARCHAR,
	                LogicalType::LIST(LogicalType::VARCHAR),
	                LogicalType::VARCHAR};
	auto kind = StringArgument(input, 0);
	auto view = NameArguments(input, 1);
	auto &registry = GetSemanticViewsState(input).registry;
	vector<shared_ptr<SemanticModel>> models;
	if (!view.name.empty()) {
		models.push_back(registry.GetOrThrow(context, view));
	} else {
		for (auto &model : registry.List()) {
			if (InDefaultSchema(context, *model)) {
				models.push_back(model);
			}
		}
	}
	auto result = make_uniq<RowsFunctionData>();
	for (auto &model : models) {
		auto &name = model->Name();
		for (auto &member : model->members) {
			if (SemanticMemberKindToString(member.Kind()) != kind) {
				continue;
			}
			result->rows.push_back({Value(name.catalog), Value(name.schema), Value(name.name),
			                        TableName(*model, member), Value(member.Name()), TypeName(member),
			                        StringList(member.parsed->synonyms), OptionalString(member.parsed->comment)});
		}
	}
	return std::move(result);
}

//===--------------------------------------------------------------------===//
// SHOW SEMANTIC DIMENSIONS IN view FOR METRIC m: dimensions every aggregate of the metric can be grouped by
//===--------------------------------------------------------------------===//
static unique_ptr<FunctionData> ShowForMetricBind(ClientContext &context, TableFunctionBindInput &input,
                                                  vector<LogicalType> &return_types, vector<string> &names) {
	names = {"table_name", "name", "data_type", "required", "synonyms", "comment"};
	return_types = {LogicalType::VARCHAR,
	                LogicalType::VARCHAR,
	                LogicalType::VARCHAR,
	                LogicalType::BOOLEAN,
	                LogicalType::LIST(LogicalType::VARCHAR),
	                LogicalType::VARCHAR};
	auto model = GetSemanticViewsState(input).registry.GetOrThrow(context, NameArguments(input, 0));
	vector<string> metric_name;
	for (auto &part : ListValue::GetChildren(input.inputs[3])) {
		metric_name.push_back(StringValue::Get(part));
	}
	string error;
	auto ref = model->Lookup(metric_name, SemanticResolveContext(), error);
	if (!ref.Is(SemanticRefKind::MEMBER) || model->members[ref.index].Kind() != SemanticMemberKind::METRIC) {
		throw BinderException("Unknown metric \"%s\" in semantic view \"%s\"%s", StringUtil::Join(metric_name, "."),
		                      model->Name().name, error.empty() ? string() : ": " + error);
	}
	SemanticAnalyzer analyzer(*model, context);
	vector<SemanticAtom> atoms;
	analyzer.DecomposeMetric(ref.index, atoms, "Metric");
	// dimensions used by window clauses must be part of the query
	set<idx_t> required;
	analyzer.WindowDimensions(*SemanticMarkers::Member(ref.index), required);
	// dimensions reached through a single, unambiguous many-to-many bridge are available too
	SemanticBridgeResolver resolver(*model);
	auto result = make_uniq<RowsFunctionData>();
	for (idx_t d = 0; d < model->members.size(); d++) {
		auto &dimension = model->members[d];
		if (dimension.Kind() != SemanticMemberKind::DIMENSION) {
			continue;
		}
		bool valid = true;
		for (auto &atom : atoms) {
			valid = valid &&
			        (model->Reaches(atom.home, dimension.table) ||
			         resolver.Candidates(atom.home, {dimension.table}, atom.context.using_relationships).size() == 1);
		}
		if (valid) {
			result->rows.push_back({TableName(*model, dimension), Value(dimension.Name()), TypeName(dimension),
			                        Value::BOOLEAN(required.count(d) > 0), StringList(dimension.parsed->synonyms),
			                        OptionalString(dimension.parsed->comment)});
		}
	}
	return std::move(result);
}

//===--------------------------------------------------------------------===//
// DESCRIBE SEMANTIC VIEW (Snowflake layout)
//===--------------------------------------------------------------------===//
static string BracketList(const vector<string> &values) {
	return "[" + StringUtil::Join(values, ", ") + "]";
}

static string TagList(const vector<ParsedSemanticTag> &tags) {
	vector<string> values;
	for (auto &tag : tags) {
		values.push_back(tag.name + "=" + tag.value);
	}
	return tags.empty() ? string() : BracketList(values);
}

static unique_ptr<FunctionData> DescribeBind(ClientContext &context, TableFunctionBindInput &input,
                                             vector<LogicalType> &return_types, vector<string> &names) {
	names = {"object_kind", "object_name", "parent_entity", "property", "property_value"};
	return_types = vector<LogicalType>(5, LogicalType::VARCHAR);
	auto model = GetSemanticViewsState(input).registry.GetOrThrow(context, NameArguments(input, 0));
	auto result = make_uniq<RowsFunctionData>();
	Value null_value(LogicalType::VARCHAR);
	auto add = [&](const Value &kind, const Value &object, const Value &parent, const string &property,
	               const string &value) {
		if (!value.empty()) {
			result->rows.push_back({kind, object, parent, Value(property), Value(value)});
		}
	};
	add(null_value, null_value, null_value, "COMMENT", model->Comment());
	auto &definition = *model->definition;
	add(null_value, null_value, null_value, "TAGS", TagList(definition.tags));
	if (definition.has_max_staleness) {
		add(null_value, null_value, null_value, "MAX_STALENESS", to_string(definition.max_staleness));
	}
	if (definition.copy_grants) {
		add(null_value, null_value, null_value, "COPY_GRANTS", "true");
	}
	Value instructions("CUSTOM_INSTRUCTIONS");
	add(instructions, null_value, null_value, "AI_SQL_GENERATION", definition.ai_sql_generation);
	add(instructions, null_value, null_value, "AI_QUESTION_CATEGORIZATION", definition.ai_question_categorization);
	for (auto &query : definition.verified_queries) {
		Value kind("AI_VERIFIED_QUERY");
		Value object(query.name);
		add(kind, object, null_value, "QUESTION", query.question);
		add(kind, object, null_value, "VERIFIED_AT", query.verified_at);
		add(kind, object, null_value, "ONBOARDING_QUESTION", query.onboarding_question ? "true" : "false");
		add(kind, object, null_value, "VERIFIED_BY", query.verified_by);
		add(kind, object, null_value, "SQL", query.sql);
	}
	for (idx_t t = 0; t < model->tables.size(); t++) {
		auto &table = model->tables[t];
		Value kind("TABLE");
		Value object(table.name);
		if (table.IsBaseTable()) {
			add(kind, object, null_value, "BASE_TABLE_DATABASE_NAME", table.catalog_name);
			add(kind, object, null_value, "BASE_TABLE_SCHEMA_NAME", table.schema_name);
			add(kind, object, null_value, "BASE_TABLE_NAME", table.table_name);
		} else {
			add(kind, object, null_value, "BASE_QUERY", table.source->ToString());
		}
		if (!table.primary_key.empty()) {
			add(kind, object, null_value, "PRIMARY_KEY", BracketList(table.primary_key));
		}
		for (auto &unique : table.unique_keys) {
			add(kind, object, null_value, "UNIQUE_KEY", BracketList(unique));
		}
		if (!table.synonyms.empty()) {
			add(kind, object, null_value, "SYNONYMS", BracketList(table.synonyms));
		}
		add(kind, object, null_value, "COMMENT", table.comment);
		add(kind, object, null_value, "TAGS", TagList(definition.tables[t].tags));
	}
	for (auto &rel : model->relationships) {
		Value kind("RELATIONSHIP");
		Value object(rel.name);
		add(kind, object, null_value, "TABLE", model->tables[rel.from_table].name);
		add(kind, object, null_value, "REF_TABLE", model->tables[rel.to_table].name);
		add(kind, object, null_value, "FOREIGN_KEY", BracketList(rel.from_columns));
		if (rel.is_range) {
			add(kind, object, null_value, "REF_RANGE", BracketList({rel.range_start, rel.range_end}));
		} else {
			add(kind, object, null_value, "REF_KEY", BracketList(rel.to_columns));
		}
	}
	for (auto &member : model->members) {
		auto &parsed = *member.parsed;
		Value kind(member.IsDerived() ? "DERIVED_METRIC" : SemanticMemberKindToString(member.Kind()));
		Value object(member.Name());
		auto parent = TableName(*model, member);
		if (!member.IsDerived()) {
			add(kind, object, parent, "TABLE", model->tables[member.table].name);
		}
		add(kind, object, parent, "EXPRESSION", member.Expression().ToString());
		if (parsed.filter) {
			add(kind, object, parent, "FILTER", parsed.filter->ToString());
		}
		if (!parsed.offset_dimension.empty()) {
			add(kind, object, parent, "OFFSET",
			    StringUtil::Join(parsed.offset_dimension, ".") + " BY " + parsed.offset_interval->ToString());
		}
		if (member.type.id() != LogicalTypeId::UNKNOWN) {
			add(kind, object, parent, "DATA_TYPE", member.type.ToString());
		}
		if (!member.parsed->synonyms.empty()) {
			add(kind, object, parent, "SYNONYMS", BracketList(member.parsed->synonyms));
		}
		add(kind, object, parent, "COMMENT", member.parsed->comment);
		add(kind, object, parent, "TAGS", TagList(parsed.tags));
		if (!parsed.labels.empty()) {
			add(kind, object, parent, "LABELS", BracketList(parsed.labels));
		}
		if (!parsed.sample_values.empty()) {
			add(kind, object, parent, "SAMPLE_VALUES", BracketList(parsed.sample_values));
		}
		if (parsed.is_enum) {
			add(kind, object, parent, "IS_ENUM", "true");
		}
		if (!parsed.cortex_search_service.empty()) {
			add(kind, object, parent, "CORTEX_SEARCH_SERVICE", parsed.cortex_search_service);
			add(kind, object, parent, "CORTEX_SEARCH_SERVICE_COLUMN_NAME", parsed.cortex_search_column);
		}
		if (!member.IsDerived()) {
			add(kind, object, parent, "ACCESS_MODIFIER", member.IsPrivate() ? "PRIVATE" : "PUBLIC");
		}
	}
	return std::move(result);
}

//===--------------------------------------------------------------------===//
// SHOW SEMANTIC MATERIALIZATIONS [IN view]
//===--------------------------------------------------------------------===//
static unique_ptr<FunctionData> ShowMaterializationsBind(ClientContext &context, TableFunctionBindInput &input,
                                                         vector<LogicalType> &return_types, vector<string> &names) {
	names = {"database_name",
	         "schema_name",
	         "semantic_view_name",
	         "name",
	         "dimensions",
	         "metrics",
	         "refresh_mode",
	         "incremental",
	         "not_incremental_reason",
	         "storage",
	         "tables",
	         "last_refresh",
	         "last_refresh_detail",
	         "verified_at",
	         "watermark",
	         "usable",
	         "not_usable_reason"};
	auto list = LogicalType::LIST(LogicalType::VARCHAR);
	return_types = {LogicalType::VARCHAR,
	                LogicalType::VARCHAR,
	                LogicalType::VARCHAR,
	                LogicalType::VARCHAR,
	                list,
	                list,
	                LogicalType::VARCHAR,
	                LogicalType::BOOLEAN,
	                LogicalType::VARCHAR,
	                LogicalType::VARCHAR,
	                list,
	                LogicalType::VARCHAR,
	                LogicalType::VARCHAR,
	                LogicalType::TIMESTAMP,
	                LogicalType::VARCHAR,
	                LogicalType::BOOLEAN,
	                LogicalType::VARCHAR};
	auto view = NameArguments(input, 0);
	auto &registry = GetSemanticViewsState(input).registry;
	vector<shared_ptr<SemanticModel>> models;
	if (!view.name.empty()) {
		models.push_back(registry.GetOrThrow(context, view));
	} else {
		for (auto &model : registry.List()) {
			if (InDefaultSchema(context, *model)) {
				models.push_back(model);
			}
		}
	}
	auto result = make_uniq<RowsFunctionData>();
	for (auto &model : models) {
		auto &name = model->Name();
		for (auto &materialization : model->materializations) {
			auto status = GetMaterializationStatus(context, *model, *materialization);
			auto &state = *materialization->state;
			lock_guard<mutex> guard(state.lock);
			vector<string> tables;
			for (idx_t b = 0; state.built && b < state.tables.size(); b++) {
				tables.push_back(materialization->TableName(b));
			}
			vector<string> watermark;
			for (auto &entry : state.watermark) {
				watermark.push_back(entry.first + "@" + to_string(entry.second));
			}
			result->rows.push_back(
			    {Value(name.catalog), Value(name.schema), Value(name.name), Value(materialization->name),
			     StringList(materialization->dimension_names), StringList(materialization->metric_names),
			     Value(SemanticRefreshModeToString(materialization->refresh_mode)),
			     Value::BOOLEAN(materialization->incremental), OptionalString(materialization->incremental_reason),
			     Value(materialization->storage_catalog + "." + materialization->storage_schema), StringList(tables),
			     OptionalString(state.last_refresh), OptionalString(state.last_refresh_detail),
			     state.verified_at.value == 0 ? Value(LogicalType::TIMESTAMP) : Value::TIMESTAMP(state.verified_at),
			     OptionalString(StringUtil::Join(watermark, ", ")), Value::BOOLEAN(status.usable),
			     OptionalString(status.reason)});
		}
	}
	return std::move(result);
}

void RegisterSemanticShowFunctions(ExtensionLoader &loader, shared_ptr<SemanticViewsState> state) {
	vector<LogicalType> name_types(3, LogicalType::VARCHAR);
	loader.RegisterFunction(SemanticViewsTableFunction(SEMANTIC_VIEW_DESCRIBE_FUNCTION, name_types, RowsFunctionScan,
	                                                   DescribeBind, RowsFunctionInit, state));
	loader.RegisterFunction(SemanticViewsTableFunction(SEMANTIC_VIEW_SHOW_VIEWS_FUNCTION,
	                                                   {LogicalType::VARCHAR, LogicalType::VARCHAR}, RowsFunctionScan,
	                                                   ShowViewsBind, RowsFunctionInit, state));
	loader.RegisterFunction(SemanticViewsTableFunction(SEMANTIC_VIEW_SHOW_MEMBERS_FUNCTION,
	                                                   vector<LogicalType>(4, LogicalType::VARCHAR), RowsFunctionScan,
	                                                   ShowMembersBind, RowsFunctionInit, state));
	auto for_metric_types = name_types;
	for_metric_types.push_back(LogicalType::LIST(LogicalType::VARCHAR));
	loader.RegisterFunction(SemanticViewsTableFunction(SEMANTIC_VIEW_SHOW_FOR_METRIC_FUNCTION, for_metric_types,
	                                                   RowsFunctionScan, ShowForMetricBind, RowsFunctionInit, state));
	loader.RegisterFunction(SemanticViewsTableFunction(SEMANTIC_VIEW_SHOW_MATERIALIZATIONS_FUNCTION, name_types,
	                                                   RowsFunctionScan, ShowMaterializationsBind, RowsFunctionInit,
	                                                   state));
}

} // namespace duckdb
