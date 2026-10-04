#include "semantic_lowering.hpp"

#include "duckdb/common/enum_util.hpp"
#include "duckdb/common/serializer/binary_serializer.hpp"
#include "duckdb/parser/expression/comparison_expression.hpp"
#include "duckdb/parser/expression/constant_expression.hpp"
#include "duckdb/parser/expression/function_expression.hpp"
#include "duckdb/parser/expression/star_expression.hpp"
#include "duckdb/parser/query_node/select_node.hpp"
#include "duckdb/parser/statement/select_statement.hpp"
#include "duckdb/parser/tableref/table_function_ref.hpp"
#include "semantic_ast_helpers.hpp"

namespace duckdb {

static unique_ptr<TableRef> Call(const string &function, vector<Value> args) {
	vector<unique_ptr<ParsedExpression>> children;
	for (auto &arg : args) {
		children.push_back(make_uniq<ConstantExpression>(std::move(arg)));
	}
	auto ref = make_uniq<TableFunctionRef>();
	ref->function = make_uniq<FunctionExpression>(function, std::move(children));
	return std::move(ref);
}

static vector<Value> NameValues(const QualifiedName &name) {
	return {Value(name.catalog), Value(name.schema), Value(name.name)};
}

static vector<Value> NameValues(const QualifiedName &name, vector<Value> extra) {
	auto result = NameValues(name);
	for (auto &value : extra) {
		result.push_back(std::move(value));
	}
	return result;
}

unique_ptr<TableRef> SemanticLowering::Create(const ParsedSemanticView &view, OnCreateConflict on_conflict) {
	return Call(SEMANTIC_VIEW_CREATE_FUNCTION, {Encode(view), Value(EnumUtil::ToString(on_conflict))});
}

unique_ptr<TableRef> SemanticLowering::Drop(const QualifiedName &name, bool if_exists) {
	return Call(SEMANTIC_VIEW_DROP_FUNCTION, NameValues(name, {Value::BOOLEAN(if_exists)}));
}

unique_ptr<TableRef> SemanticLowering::Rename(const QualifiedName &name, bool if_exists, const string &new_name) {
	return Call(SEMANTIC_VIEW_RENAME_FUNCTION, NameValues(name, {Value::BOOLEAN(if_exists), Value(new_name)}));
}

unique_ptr<TableRef> SemanticLowering::SetComment(const QualifiedName &name, bool if_exists, const Value &comment) {
	return Call(SEMANTIC_VIEW_SET_COMMENT_FUNCTION, NameValues(name, {Value::BOOLEAN(if_exists), comment}));
}

unique_ptr<TableRef> SemanticLowering::SetMaxStaleness(const QualifiedName &name, bool if_exists,
                                                       const Value &seconds) {
	return Call(SEMANTIC_VIEW_SET_MAX_STALENESS_FUNCTION, NameValues(name, {Value::BOOLEAN(if_exists), seconds}));
}

unique_ptr<TableRef> SemanticLowering::AddMaterialization(const QualifiedName &name, bool if_exists,
                                                          const ParsedSemanticMaterialization &materialization,
                                                          bool if_not_exists) {
	ParsedSemanticMaterializationPayload payload;
	payload.materialization.name = materialization.name;
	payload.materialization.refresh_mode = materialization.refresh_mode;
	payload.materialization.warehouse = materialization.warehouse;
	for (auto &item : materialization.items) {
		ParsedSemanticQueryItem copy;
		copy.kind = item.kind;
		copy.expression = item.expression ? item.expression->Copy() : nullptr;
		copy.star_table = item.star_table;
		copy.alias = item.alias;
		payload.materialization.items.push_back(std::move(copy));
	}
	return Call(SEMANTIC_VIEW_ADD_MATERIALIZATION_FUNCTION,
	            NameValues(name, {Value::BOOLEAN(if_exists), Encode(payload), Value::BOOLEAN(if_not_exists)}));
}

unique_ptr<TableRef> SemanticLowering::DropMaterialization(const QualifiedName &name, bool if_exists,
                                                           const string &materialization,
                                                           bool materialization_if_exists) {
	return Call(SEMANTIC_VIEW_DROP_MATERIALIZATION_FUNCTION,
	            NameValues(name, {Value::BOOLEAN(if_exists), Value(materialization),
	                              Value::BOOLEAN(materialization_if_exists)}));
}

unique_ptr<TableRef> SemanticLowering::RefreshMaterialization(const QualifiedName &name, bool if_exists,
                                                              const string &materialization, const Value &mode) {
	return Call(SEMANTIC_VIEW_REFRESH_MATERIALIZATION_FUNCTION,
	            NameValues(name, {Value::BOOLEAN(if_exists), Value(materialization), mode}));
}

unique_ptr<TableRef> SemanticLowering::ShowMaterializations(const QualifiedName &view) {
	return Call(SEMANTIC_VIEW_SHOW_MATERIALIZATIONS_FUNCTION, NameValues(view));
}

unique_ptr<TableRef> SemanticLowering::Describe(const QualifiedName &name) {
	return Call(SEMANTIC_VIEW_DESCRIBE_FUNCTION, NameValues(name));
}

unique_ptr<TableRef> SemanticLowering::ShowViews(const QualifiedName &scope) {
	return Call(SEMANTIC_VIEW_SHOW_VIEWS_FUNCTION, {Value(scope.catalog), Value(scope.schema)});
}

unique_ptr<TableRef> SemanticLowering::ShowMembers(SemanticMemberKind kind, const QualifiedName &view) {
	vector<Value> args {Value(SemanticMemberKindToString(kind))};
	for (auto &value : NameValues(view)) {
		args.push_back(value);
	}
	return Call(SEMANTIC_VIEW_SHOW_MEMBERS_FUNCTION, std::move(args));
}

unique_ptr<TableRef> SemanticLowering::ShowDimensionsForMetric(const QualifiedName &view,
                                                               const vector<string> &metric) {
	vector<Value> parts;
	for (auto &part : metric) {
		parts.emplace_back(part);
	}
	return Call(SEMANTIC_VIEW_SHOW_FOR_METRIC_FUNCTION,
	            NameValues(view, {Value::LIST(LogicalType::VARCHAR, std::move(parts))}));
}

unique_ptr<TableRef> SemanticLowering::Query(const ParsedSemanticQuery &query) {
	return Call(SEMANTIC_VIEW_QUERY_FUNCTION, {Encode(query)});
}

unique_ptr<TableRef> SemanticLowering::WhereNameLike(unique_ptr<TableRef> ref, const string &pattern) {
	auto node = make_uniq<SelectNode>();
	node->select_list.push_back(make_uniq<StarExpression>());
	node->from_table = std::move(ref);
	vector<unique_ptr<ParsedExpression>> arguments;
	arguments.push_back(ColumnRef("name"));
	arguments.push_back(make_uniq<ConstantExpression>(Value(pattern)));
	// `name ILIKE pattern`
	node->where_clause = make_uniq<FunctionExpression>("~~*", std::move(arguments), nullptr, nullptr, false, true);
	return MakeSubqueryRef(std::move(node), "__sv_show");
}

unique_ptr<SQLStatement> SemanticLowering::SelectAll(unique_ptr<TableRef> ref) {
	auto node = make_uniq<SelectNode>();
	node->select_list.push_back(make_uniq<StarExpression>());
	node->from_table = std::move(ref);
	return MakeSelectStatement(std::move(node));
}

} // namespace duckdb
