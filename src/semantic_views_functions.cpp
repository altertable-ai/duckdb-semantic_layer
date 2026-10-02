#include "semantic_views_functions.hpp"

#include "duckdb/common/exception.hpp"
#include "duckdb/common/exception/binder_exception.hpp"
#include "duckdb/common/exception/catalog_exception.hpp"
#include "duckdb/common/to_string.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/function/table_function.hpp"
#include "duckdb/main/client_context.hpp"
#include "duckdb/main/client_data.hpp"
#include "duckdb/catalog/catalog_search_path.hpp"
#include "duckdb/main/database_manager.hpp"
#include "duckdb/main/extension/extension_loader.hpp"
#include "duckdb/parser/parser.hpp"
#include "duckdb/parser/expression/constant_expression.hpp"
#include "duckdb/parser/expression/function_expression.hpp"
#include "duckdb/parser/expression/subquery_expression.hpp"
#include "duckdb/parser/parsed_expression_iterator.hpp"
#include "duckdb/parser/query_node/select_node.hpp"
#include "duckdb/parser/query_node/set_operation_node.hpp"
#include "duckdb/parser/statement/select_statement.hpp"
#include "duckdb/parser/tableref/joinref.hpp"
#include "duckdb/parser/tableref/subqueryref.hpp"
#include "duckdb/parser/tableref/table_function_ref.hpp"
#include "semantic_analysis.hpp"
#include "semantic_query_compiler.hpp"
#include "semantic_view_registry.hpp"
#include "semantic_views_parser.hpp"

namespace duckdb {

//===--------------------------------------------------------------------===//
// Shared helper: a table function that emits pre-computed rows
//===--------------------------------------------------------------------===//
struct RowsFunctionState : public GlobalTableFunctionState {
	idx_t offset = 0;
};

unique_ptr<GlobalTableFunctionState> RowsFunctionInit(ClientContext &, TableFunctionInitInput &) {
	return make_uniq<RowsFunctionState>();
}

void RowsFunctionScan(ClientContext &, TableFunctionInput &data_p, DataChunk &output) {
	auto &data = data_p.bind_data->Cast<RowsFunctionData>();
	auto &state = data_p.global_state->Cast<RowsFunctionState>();
	idx_t count = 0;
	while (state.offset < data.rows.size() && count < STANDARD_VECTOR_SIZE) {
		auto &row = data.rows[state.offset++];
		for (idx_t col = 0; col < row.size(); col++) {
			output.SetValue(col, count, row[col]);
		}
		count++;
	}
	output.SetCardinality(count);
}

static SemanticViewsState &GetState(TableFunctionBindInput &input) {
	auto &info = input.info->Cast<SemanticViewsFunctionInfo>();
	return *info.state;
}

static string StringArg(TableFunctionBindInput &input, idx_t index) {
	auto &value = input.inputs[index];
	return value.IsNull() ? string() : StringValue::Get(value);
}

static QualifiedName NameArgs(TableFunctionBindInput &input, idx_t first) {
	QualifiedName name;
	name.catalog = StringArg(input, first);
	name.schema = StringArg(input, first + 1);
	name.name = StringArg(input, first + 2);
	return name;
}

//===--------------------------------------------------------------------===//
// semantic_views_peg_parse(sql) - debug helper
//===--------------------------------------------------------------------===//
static unique_ptr<FunctionData> PegParseBind(ClientContext &context, TableFunctionBindInput &input,
                                             vector<LogicalType> &return_types, vector<string> &names) {
	names.emplace_back("rewritten_sql");
	return_types.emplace_back(LogicalType::VARCHAR);
	if (input.inputs[0].IsNull()) {
		throw BinderException("semantic_views_peg_parse: the query cannot be NULL");
	}
	auto statements = SemanticViewsRewriter::Parse(StringValue::Get(input.inputs[0]), context.GetParserOptions());
	auto result = make_uniq<RowsFunctionData>();
	for (auto &statement : statements) {
		result->rows.push_back({Value(statement->ToString())});
	}
	return std::move(result);
}

//===--------------------------------------------------------------------===//
// __semantic_view_ddl(action, catalog, schema, name, arg, flag)
//===--------------------------------------------------------------------===//
struct SemanticDDLData : public TableFunctionData {
	shared_ptr<SemanticViewsState> state;
	string action;
	QualifiedName name;
	string arg;
	string flag;
	shared_ptr<SemanticModel> model;
};

struct SemanticDDLState : public GlobalTableFunctionState {
	bool done = false;
};

static unique_ptr<FunctionData> SemanticDDLBind(ClientContext &context, TableFunctionBindInput &input,
                                                vector<LogicalType> &return_types, vector<string> &names) {
	names.emplace_back("status");
	return_types.emplace_back(LogicalType::VARCHAR);
	auto &info = input.info->Cast<SemanticViewsFunctionInfo>();
	auto result = make_uniq<SemanticDDLData>();
	result->state = info.state;
	result->action = StringArg(input, 0);
	result->name = SemanticViewRegistry::ResolveDDLName(context, NameArgs(input, 1));
	result->arg = StringArg(input, 4);
	result->flag = StringArg(input, 5);
	if (result->action == "create") {
		auto exists = result->state->registry.Get(result->name) != nullptr;
		if (!(exists && result->flag == "ignore")) {
			auto definition = SemanticViewsRewriter::ParseDefinition(result->arg);
			result->model = BuildSemanticModel(context, *definition, result->name);
		}
	} else if (result->action != "drop" && result->action != "rename" && result->action != "set_comment" &&
	           result->action != "unset_comment") {
		throw InternalException("unknown semantic view DDL action '%s'", result->action);
	}
	return std::move(result);
}

static unique_ptr<GlobalTableFunctionState> SemanticDDLInit(ClientContext &, TableFunctionInitInput &) {
	return make_uniq<SemanticDDLState>();
}

static string ExecuteSemanticDDL(const SemanticDDLData &data) {
	auto &registry = data.state->registry;
	auto display_name = StringUtil::Upper(data.name.name);
	if (data.action == "create") {
		if (!data.model) {
			return display_name + " already exists, statement succeeded.";
		}
		if (!registry.Create(data.model, data.flag == "replace")) {
			if (data.flag == "ignore") {
				return display_name + " already exists, statement succeeded.";
			}
			throw CatalogException("Semantic view with name \"%s\" already exists", data.name.name);
		}
		return "Semantic view " + display_name + " successfully created.";
	}
	if (data.action == "drop") {
		if (!registry.Drop(data.name)) {
			if (data.flag == "if_exists") {
				return "Drop statement executed successfully (" + display_name + " already dropped).";
			}
			throw CatalogException("Semantic view with name \"%s\" does not exist!", data.name.name);
		}
		return display_name + " successfully dropped.";
	}
	auto existing = registry.Get(data.name);
	if (!existing) {
		if (data.flag == "if_exists") {
			return "Statement executed successfully.";
		}
		throw CatalogException("Semantic view with name \"%s\" does not exist!", data.name.name);
	}
	if (data.action == "rename") {
		registry.Rename(data.name, data.arg);
		return "Statement executed successfully.";
	}
	auto altered = existing->Copy();
	altered->comment = data.action == "set_comment" ? data.arg : string();
	altered->definition->comment = altered->comment;
	altered->definition_sql = altered->definition->ToSQL();
	registry.Replace(std::move(altered));
	return "Statement executed successfully.";
}

static void SemanticDDLScan(ClientContext &, TableFunctionInput &data_p, DataChunk &output) {
	auto &state = data_p.global_state->Cast<SemanticDDLState>();
	if (state.done) {
		output.SetCardinality(0);
		return;
	}
	state.done = true;
	auto &data = data_p.bind_data->Cast<SemanticDDLData>();
	output.SetValue(0, 0, Value(ExecuteSemanticDDL(data)));
	output.SetCardinality(1);
}

//===--------------------------------------------------------------------===//
// __semantic_view_show(kind, catalog, schema, name, like, extra)
//===--------------------------------------------------------------------===//
//! Case-insensitive SQL LIKE (% and _)
static bool LikeMatch(const string &text, const string &pattern) {
	auto t = StringUtil::Lower(text);
	auto p = StringUtil::Lower(pattern);
	std::function<bool(idx_t, idx_t)> match = [&](idx_t ti, idx_t pi) -> bool {
		while (pi < p.size()) {
			if (p[pi] == '%') {
				for (idx_t k = ti; k <= t.size(); k++) {
					if (match(k, pi + 1)) {
						return true;
					}
				}
				return false;
			}
			if (ti >= t.size() || (p[pi] != '_' && p[pi] != t[ti])) {
				return false;
			}
			ti++;
			pi++;
		}
		return ti == t.size();
	};
	return match(0, 0);
}

static Value ListValue(const vector<string> &values) {
	if (values.empty()) {
		return Value(LogicalType::LIST(LogicalType::VARCHAR));
	}
	vector<Value> list;
	for (auto &value : values) {
		list.emplace_back(value);
	}
	return Value::LIST(LogicalType::VARCHAR, std::move(list));
}

static string BracketList(const vector<string> &values) {
	return "[" + StringUtil::Join(values, ", ") + "]";
}

static Value OptionalString(const string &value) {
	return value.empty() ? Value(LogicalType::VARCHAR) : Value(value);
}

static bool InDefaultSchema(ClientContext &context, const SemanticModel &model) {
	auto &search_path = *ClientData::Get(context).catalog_search_path;
	auto &entry = search_path.GetDefault();
	auto catalog = entry.catalog.empty() || entry.catalog == INVALID_CATALOG
	                   ? DatabaseManager::GetDefaultDatabase(context)
	                   : entry.catalog;
	return StringUtil::CIEquals(model.name.catalog, catalog) && StringUtil::CIEquals(model.name.schema, entry.schema);
}

static void ShowViews(ClientContext &context, TableFunctionBindInput &input, RowsFunctionData &result,
                      vector<LogicalType> &return_types, vector<string> &names) {
	names = {"created_on", "name", "database_name", "schema_name", "comment"};
	return_types = {LogicalType::TIMESTAMP_TZ, LogicalType::VARCHAR, LogicalType::VARCHAR, LogicalType::VARCHAR,
	                LogicalType::VARCHAR};
	auto like = StringArg(input, 4);
	auto scope = StringArg(input, 5);
	auto scope_catalog = StringArg(input, 1);
	auto scope_schema = StringArg(input, 2);
	if (scope == "schema" && scope_catalog.empty()) {
		QualifiedName schema_name;
		schema_name.schema = scope_schema;
		schema_name.name = "x";
		auto resolved = SemanticViewRegistry::ResolveDDLName(context, schema_name);
		scope_catalog = resolved.catalog;
		scope_schema = resolved.schema;
	}
	for (auto &model : GetState(input).registry.List()) {
		if (scope == "database" && !StringUtil::CIEquals(model->name.catalog, scope_catalog)) {
			continue;
		}
		if (scope == "schema" && (!StringUtil::CIEquals(model->name.catalog, scope_catalog) ||
		                          !StringUtil::CIEquals(model->name.schema, scope_schema))) {
			continue;
		}
		if (!like.empty() && !LikeMatch(model->name.name, like)) {
			continue;
		}
		result.rows.push_back({Value::TIMESTAMPTZ(model->created_on), Value(model->name.name),
		                       Value(model->name.catalog), Value(model->name.schema), OptionalString(model->comment)});
	}
}

//! Dimensions a window-function metric needs in the query (filled in by window metric support)
static set<idx_t> RequiredWindowDimensions(const SemanticModel &model, idx_t metric) {
	return set<idx_t>();
}

//! SHOW SEMANTIC DIMENSIONS IN <view> FOR METRIC <metric>: dimensions every aggregate of the metric can be grouped by
static void ShowDimensionsForMetric(ClientContext &context, shared_ptr<SemanticModel> model, const string &metric_name,
                                    const string &like, RowsFunctionData &result, vector<LogicalType> &return_types,
                                    vector<string> &names) {
	names = {"table_name", "name", "data_type", "required", "synonyms", "comment"};
	return_types = {LogicalType::VARCHAR, LogicalType::VARCHAR,
	                LogicalType::VARCHAR, LogicalType::BOOLEAN,
	                LogicalType::LIST(LogicalType::VARCHAR), LogicalType::VARCHAR};
	auto parts = StringUtil::Split(metric_name, '.');
	idx_t metric = DConstants::INVALID_INDEX;
	for (idx_t m = 0; m < model->members.size(); m++) {
		auto &member = model->members[m];
		if (member.kind != SemanticMemberKind::METRIC) {
			continue;
		}
		bool matches = parts.size() == 1 ? StringUtil::CIEquals(member.name, parts[0])
		                                 : !member.IsDerived() &&
		                                       StringUtil::CIEquals(model->tables[member.table].name, parts[0]) &&
		                                       StringUtil::CIEquals(member.name, parts[1]);
		if (matches) {
			if (metric != DConstants::INVALID_INDEX) {
				throw BinderException("Ambiguous metric name \"%s\"; qualify it with its logical table", metric_name);
			}
			metric = m;
		}
	}
	if (metric == DConstants::INVALID_INDEX) {
		throw BinderException("Unknown metric \"%s\" in semantic view \"%s\"", metric_name, model->name.name);
	}
	SemanticAnalyzer analyzer(*model, context);
	vector<SemanticAtom> atoms;
	auto &metric_member = model->members[metric];
	analyzer.Decompose(*metric_member.resolved, metric_member.table, atoms, "Metric");
	auto required = RequiredWindowDimensions(*model, metric);
	for (idx_t d = 0; d < model->members.size(); d++) {
		auto &dimension = model->members[d];
		if (dimension.kind != SemanticMemberKind::DIMENSION) {
			continue;
		}
		bool valid = true;
		for (auto &atom : atoms) {
			valid = valid && model->Reaches(atom.home, dimension.table);
		}
		if (!valid || (!like.empty() && !LikeMatch(dimension.name, like))) {
			continue;
		}
		result.rows.push_back({Value(model->tables[dimension.table].name), Value(dimension.name),
		                       dimension.type.id() == LogicalTypeId::UNKNOWN ? Value(LogicalType::VARCHAR)
		                                                                     : Value(dimension.type.ToString()),
		                       Value::BOOLEAN(required.count(d) > 0),
		                       dimension.parsed.synonyms.empty() ? Value(LogicalType::LIST(LogicalType::VARCHAR))
		                                                         : ListValue(dimension.parsed.synonyms),
		                       OptionalString(dimension.parsed.comment)});
	}
}

static void ShowMembers(ClientContext &context, TableFunctionBindInput &input, RowsFunctionData &result,
                        vector<LogicalType> &return_types, vector<string> &names, SemanticMemberKind kind) {
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
	auto &registry = GetState(input).registry;
	auto view_name = NameArgs(input, 1);
	auto like = StringArg(input, 4);
	auto for_metric = StringArg(input, 5);
	if (!for_metric.empty()) {
		ShowDimensionsForMetric(context, registry.GetOrThrow(context, view_name), for_metric, like, result,
		                        return_types, names);
		return;
	}
	vector<shared_ptr<SemanticModel>> models;
	if (!view_name.name.empty()) {
		models.push_back(registry.GetOrThrow(context, view_name));
	} else {
		for (auto &model : registry.List()) {
			if (InDefaultSchema(context, *model)) {
				models.push_back(model);
			}
		}
	}
	for (auto &model : models) {
		for (auto &member : model->members) {
			if (member.kind != kind) {
				continue;
			}
			if (!like.empty() && !LikeMatch(member.name, like)) {
				continue;
			}
			result.rows.push_back(
			    {Value(model->name.catalog), Value(model->name.schema), Value(model->name.name),
			     member.IsDerived() ? Value(LogicalType::VARCHAR) : Value(model->tables[member.table].name),
			     Value(member.name),
			     member.type.id() == LogicalTypeId::UNKNOWN ? Value(LogicalType::VARCHAR)
			                                                : Value(member.type.ToString()),
			     member.parsed.synonyms.empty() ? Value(LogicalType::LIST(LogicalType::VARCHAR))
			                                    : ListValue(member.parsed.synonyms),
			     OptionalString(member.parsed.comment)});
		}
	}
}

static void DescribeView(ClientContext &context, TableFunctionBindInput &input, RowsFunctionData &result,
                         vector<LogicalType> &return_types, vector<string> &names) {
	names = {"object_kind", "object_name", "parent_entity", "property", "property_value"};
	return_types = {LogicalType::VARCHAR, LogicalType::VARCHAR, LogicalType::VARCHAR, LogicalType::VARCHAR,
	                LogicalType::VARCHAR};
	auto model = GetState(input).registry.GetOrThrow(context, NameArgs(input, 1));
	Value null_value(LogicalType::VARCHAR);
	auto add = [&](const Value &kind, const Value &object, const Value &parent, const string &property,
	               const string &value) {
		result.rows.push_back({kind, object, parent, Value(property), Value(value)});
	};
	if (!model->comment.empty()) {
		add(null_value, null_value, null_value, "COMMENT", model->comment);
	}
	for (auto &table : model->tables) {
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
		if (!table.comment.empty()) {
			add(kind, object, null_value, "COMMENT", table.comment);
		}
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
		string kind_name = SemanticMemberKindToString(member.kind);
		if (member.IsDerived()) {
			kind_name = "DERIVED_METRIC";
		}
		Value kind(kind_name);
		Value object(member.name);
		Value parent = member.IsDerived() ? null_value : Value(model->tables[member.table].name);
		if (!member.IsDerived()) {
			add(kind, object, parent, "TABLE", model->tables[member.table].name);
		}
		add(kind, object, parent, "EXPRESSION", member.expression->ToString());
		if (member.type.id() != LogicalTypeId::UNKNOWN) {
			add(kind, object, parent, "DATA_TYPE", member.type.ToString());
		}
		if (!member.parsed.synonyms.empty()) {
			add(kind, object, parent, "SYNONYMS", BracketList(member.parsed.synonyms));
		}
		if (!member.parsed.comment.empty()) {
			add(kind, object, parent, "COMMENT", member.parsed.comment);
		}
		if (!member.IsDerived()) {
			add(kind, object, parent, "ACCESS_MODIFIER", member.is_private ? "PRIVATE" : "PUBLIC");
		}
	}
}

static unique_ptr<FunctionData> SemanticShowBind(ClientContext &context, TableFunctionBindInput &input,
                                                 vector<LogicalType> &return_types, vector<string> &names) {
	auto kind = StringArg(input, 0);
	auto result = make_uniq<RowsFunctionData>();
	if (kind == "views") {
		ShowViews(context, input, *result, return_types, names);
	} else if (kind == "dimensions") {
		ShowMembers(context, input, *result, return_types, names, SemanticMemberKind::DIMENSION);
	} else if (kind == "metrics") {
		ShowMembers(context, input, *result, return_types, names, SemanticMemberKind::METRIC);
	} else if (kind == "facts") {
		ShowMembers(context, input, *result, return_types, names, SemanticMemberKind::FACT);
	} else if (kind == "describe") {
		DescribeView(context, input, *result, return_types, names);
	} else {
		throw InternalException("unknown semantic view SHOW kind '%s'", kind);
	}
	return std::move(result);
}

//===--------------------------------------------------------------------===//
// __semantic_view_query(catalog, schema, name, items, where) - SEMANTIC_VIEW(...)
//===--------------------------------------------------------------------===//
static unique_ptr<SelectStatement> CompileSemanticQuery(ClientContext &context, SemanticViewsState &state,
                                                        const vector<Value> &args, string &view_name) {
	QualifiedName name;
	name.catalog = args[0].IsNull() ? string() : StringValue::Get(args[0]);
	name.schema = args[1].IsNull() ? string() : StringValue::Get(args[1]);
	name.name = args[2].IsNull() ? string() : StringValue::Get(args[2]);
	auto model = state.registry.GetOrThrow(context, name);
	view_name = model->name.name;
	auto items = ParsedSemanticQuery::ItemsFromValue(args[3]);
	unique_ptr<ParsedExpression> where;
	if (!args[4].IsNull() && !StringValue::Get(args[4]).empty()) {
		auto expressions = Parser::ParseExpressionList(StringValue::Get(args[4]));
		if (expressions.size() != 1) {
			throw InternalException("semantic view WHERE clause did not round-trip");
		}
		where = std::move(expressions[0]);
	}
	SemanticQueryCompiler compiler(*model, context);
	auto spec = compiler.Resolve(items, where.get());
	return compiler.Compile(spec);
}

static unique_ptr<TableRef> SemanticQueryBindReplace(ClientContext &context, TableFunctionBindInput &input) {
	string view_name;
	auto select = CompileSemanticQuery(context, GetState(input), input.inputs, view_name);
	return make_uniq<SubqueryRef>(std::move(select), view_name);
}

//===--------------------------------------------------------------------===//
// semantic_view_sql(query) - the SQL a statement compiles to
//===--------------------------------------------------------------------===//
class SemanticQueryExpander {
public:
	SemanticQueryExpander(ClientContext &context, SemanticViewsState &state) : context(context), state(state) {
	}

	void Node(QueryNode &node) {
		for (auto &cte : node.cte_map.map) {
			Node(*cte.second->query->node);
		}
		switch (node.type) {
		case QueryNodeType::SELECT_NODE: {
			auto &select = node.Cast<SelectNode>();
			if (select.from_table) {
				Ref(select.from_table);
			}
			for (auto &expr : select.select_list) {
				Expr(expr);
			}
			if (select.where_clause) {
				Expr(select.where_clause);
			}
			if (select.having) {
				Expr(select.having);
			}
			if (select.qualify) {
				Expr(select.qualify);
			}
			break;
		}
		case QueryNodeType::SET_OPERATION_NODE:
			for (auto &child : node.Cast<SetOperationNode>().children) {
				Node(*child);
			}
			break;
		default:
			break;
		}
	}

private:
	void Ref(unique_ptr<TableRef> &ref) {
		switch (ref->type) {
		case TableReferenceType::TABLE_FUNCTION: {
			auto &function_ref = ref->Cast<TableFunctionRef>();
			auto &function = function_ref.function->Cast<FunctionExpression>();
			if (!StringUtil::CIEquals(function.function_name, SEMANTIC_VIEW_QUERY_FUNCTION)) {
				break;
			}
			vector<Value> args;
			for (auto &child : function.children) {
				args.push_back(child->Cast<ConstantExpression>().value);
			}
			string view_name;
			auto select = CompileSemanticQuery(context, state, args, view_name);
			auto replacement = make_uniq<SubqueryRef>(std::move(select), ref->alias.empty() ? view_name : ref->alias);
			replacement->column_name_alias = function_ref.column_name_alias;
			ref = std::move(replacement);
			break;
		}
		case TableReferenceType::JOIN: {
			auto &join = ref->Cast<JoinRef>();
			Ref(join.left);
			Ref(join.right);
			if (join.condition) {
				Expr(join.condition);
			}
			break;
		}
		case TableReferenceType::SUBQUERY:
			Node(*ref->Cast<SubqueryRef>().subquery->node);
			break;
		default:
			break;
		}
	}

	void Expr(unique_ptr<ParsedExpression> &expr) {
		if (expr->GetExpressionClass() == ExpressionClass::SUBQUERY) {
			Node(*expr->Cast<SubqueryExpression>().subquery->node);
		}
		ParsedExpressionIterator::EnumerateChildren(*expr, [&](unique_ptr<ParsedExpression> &child) { Expr(child); });
	}

	ClientContext &context;
	SemanticViewsState &state;
};

static unique_ptr<FunctionData> SemanticViewSqlBind(ClientContext &context, TableFunctionBindInput &input,
                                                    vector<LogicalType> &return_types, vector<string> &names) {
	names.emplace_back("sql");
	return_types.emplace_back(LogicalType::VARCHAR);
	if (input.inputs[0].IsNull()) {
		throw BinderException("semantic_view_sql: the query cannot be NULL");
	}
	auto statements = SemanticViewsRewriter::Parse(StringValue::Get(input.inputs[0]), context.GetParserOptions());
	SemanticQueryExpander expander(context, GetState(input));
	auto result = make_uniq<RowsFunctionData>();
	for (auto &statement : statements) {
		if (statement->type == StatementType::SELECT_STATEMENT) {
			expander.Node(*statement->Cast<SelectStatement>().node);
		}
		result->rows.push_back({Value(statement->ToString())});
	}
	return std::move(result);
}

//===--------------------------------------------------------------------===//
// Registration
//===--------------------------------------------------------------------===//
void RegisterSemanticViewsFunctions(ExtensionLoader &loader, shared_ptr<SemanticViewsState> state) {
	TableFunction peg_parse("semantic_views_peg_parse", {LogicalType::VARCHAR}, RowsFunctionScan, PegParseBind,
	                        RowsFunctionInit);
	loader.RegisterFunction(peg_parse);

	vector<LogicalType> six_varchars(6, LogicalType::VARCHAR);
	TableFunction ddl(SEMANTIC_VIEW_DDL_FUNCTION, six_varchars, SemanticDDLScan, SemanticDDLBind, SemanticDDLInit);
	ddl.function_info = make_shared_ptr<SemanticViewsFunctionInfo>(state);
	loader.RegisterFunction(ddl);

	TableFunction query(SEMANTIC_VIEW_QUERY_FUNCTION,
	                    {LogicalType::VARCHAR, LogicalType::VARCHAR, LogicalType::VARCHAR,
	                     ParsedSemanticQuery::ItemsType(), LogicalType::VARCHAR},
	                    nullptr, nullptr);
	query.bind_replace = SemanticQueryBindReplace;
	query.function_info = make_shared_ptr<SemanticViewsFunctionInfo>(state);
	loader.RegisterFunction(query);

	TableFunction sql("semantic_view_sql", {LogicalType::VARCHAR}, RowsFunctionScan, SemanticViewSqlBind,
	                  RowsFunctionInit);
	sql.function_info = make_shared_ptr<SemanticViewsFunctionInfo>(state);
	loader.RegisterFunction(sql);

	TableFunction show(SEMANTIC_VIEW_SHOW_FUNCTION, six_varchars, RowsFunctionScan, SemanticShowBind, RowsFunctionInit);
	show.function_info = make_shared_ptr<SemanticViewsFunctionInfo>(state);
	loader.RegisterFunction(show);
}

} // namespace duckdb
