#include "duckdb/common/exception/binder_exception.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/main/client_context.hpp"
#include "duckdb/main/extension/extension_loader.hpp"
#include "duckdb/parser/expression/constant_expression.hpp"
#include "duckdb/parser/expression/function_expression.hpp"
#include "duckdb/parser/tableref/subqueryref.hpp"
#include "duckdb/parser/tableref/table_function_ref.hpp"
#include "duckdb/planner/binder.hpp"
#include "semantic_lowering.hpp"
#include "semantic_materialization.hpp"
#include "semantic_offsets.hpp"
#include "semantic_query_compiler.hpp"
#include "semantic_sql_rewrite.hpp"
#include "semantic_statement_visitor.hpp"
#include "semantic_view_registry.hpp"
#include "semantic_views_functions.hpp"
#include "semantic_views_parser.hpp"

namespace duckdb {

//! The subquery a SEMANTIC_VIEW(...) call (lowered payload) compiles to
static unique_ptr<TableRef> CompileSemanticQuery(ClientContext &context, SemanticViewsState &state,
                                                 const Value &payload, optional_ptr<Binder> binder) {
	auto query = SemanticLowering::Decode<ParsedSemanticQuery>(payload);
	auto model = state.registry.GetOrThrow(context, query->view);
	SemanticQueryCompiler compiler(*model, context);
	auto spec = compiler.Resolve(query->items, query->where.get());
	SemanticMaterializationMatcher materializations(context, *model);
	SemanticCompileOptions options;
	options.materializations = &materializations;
	auto compiled = compiler.Compile(spec, options);
	CheckSemanticOffsetReads(*model, compiler.Analyzer(), spec);
	if (materializations.UsedMaterialization() && binder) {
		// a prepared statement reading a materialization is planned again (freshness checked) at every execution
		binder->SetAlwaysRequireRebind();
	}
	return make_uniq<SubqueryRef>(std::move(compiled), model->Name().name);
}

static unique_ptr<TableRef> SemanticQueryBindReplace(ClientContext &context, TableFunctionBindInput &input) {
	return CompileSemanticQuery(context, GetSemanticViewsState(input), input.inputs[0], input.binder);
}

//===--------------------------------------------------------------------===//
// semantic_view_sql(query) - the SQL a statement compiles to
//===--------------------------------------------------------------------===//
//! Replaces every SEMANTIC_VIEW(...) call by the query it compiles to
class SemanticQueryExpander : public SemanticStatementVisitor {
public:
	SemanticQueryExpander(ClientContext &context, SemanticViewsState &state) : context(context), state(state) {
	}

protected:
	void VisitTableRef(unique_ptr<TableRef> &ref, bool) override {
		if (ref->type != TableReferenceType::TABLE_FUNCTION) {
			return;
		}
		auto &function_ref = ref->Cast<TableFunctionRef>();
		auto &function = function_ref.function->Cast<FunctionExpression>();
		if (!StringUtil::CIEquals(function.function_name, SEMANTIC_VIEW_QUERY_FUNCTION)) {
			return;
		}
		auto replacement =
		    CompileSemanticQuery(context, state, function.children[0]->Cast<ConstantExpression>().value, nullptr);
		if (!ref->alias.empty()) {
			replacement->alias = ref->alias;
		}
		replacement->column_name_alias = function_ref.column_name_alias;
		ref = std::move(replacement);
	}

private:
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
	auto &state = GetSemanticViewsState(input);
	SemanticQueryExpander expander(context, state);
	auto result = make_uniq<RowsFunctionData>();
	for (auto &statement : SemanticViewsParser::Parse(StringValue::Get(input.inputs[0]), context.GetParserOptions())) {
		RewriteSemanticViewReferences(context, state, *statement);
		expander.VisitStatement(*statement);
		result->rows.push_back({Value(statement->ToString())});
	}
	return std::move(result);
}

//===--------------------------------------------------------------------===//
// semantic_views_peg_parse(sql) - how the semantic-view parser lowers a statement
//===--------------------------------------------------------------------===//
static unique_ptr<FunctionData> PegParseBind(ClientContext &context, TableFunctionBindInput &input,
                                             vector<LogicalType> &return_types, vector<string> &names) {
	names.emplace_back("rewritten_sql");
	return_types.emplace_back(LogicalType::VARCHAR);
	if (input.inputs[0].IsNull()) {
		throw BinderException("semantic_views_peg_parse: the query cannot be NULL");
	}
	auto result = make_uniq<RowsFunctionData>();
	for (auto &statement : SemanticViewsParser::Parse(StringValue::Get(input.inputs[0]), context.GetParserOptions())) {
		result->rows.push_back({Value(statement->ToString())});
	}
	return std::move(result);
}

void RegisterSemanticQueryFunctions(ExtensionLoader &loader, shared_ptr<SemanticViewsState> state) {
	auto query =
	    SemanticViewsTableFunction(SEMANTIC_VIEW_QUERY_FUNCTION, {LogicalType::BLOB}, nullptr, nullptr, nullptr, state);
	query.bind_replace = SemanticQueryBindReplace;
	loader.RegisterFunction(query);
	loader.RegisterFunction(SemanticViewsTableFunction("semantic_view_sql", {LogicalType::VARCHAR}, RowsFunctionScan,
	                                                   SemanticViewSqlBind, RowsFunctionInit, state));
	loader.RegisterFunction(SemanticViewsTableFunction("semantic_views_peg_parse", {LogicalType::VARCHAR},
	                                                   RowsFunctionScan, PegParseBind, RowsFunctionInit, state));
}

} // namespace duckdb
