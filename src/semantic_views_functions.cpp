#include "semantic_views_functions.hpp"

#include "duckdb/function/table_function.hpp"
#include "duckdb/main/client_context.hpp"
#include "duckdb/main/extension/extension_loader.hpp"
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

void RegisterSemanticViewsFunctions(ExtensionLoader &loader) {
	TableFunction peg_parse("semantic_views_peg_parse", {LogicalType::VARCHAR}, RowsFunctionScan, PegParseBind,
	                        RowsFunctionInit);
	loader.RegisterFunction(peg_parse);
}

} // namespace duckdb
