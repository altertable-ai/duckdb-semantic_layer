#include "semantic_layer_functions.hpp"

#include "duckdb/main/extension/extension_loader.hpp"
#include "semantic_catalog.hpp"
#include "semantic_layer_registry.hpp"

namespace duckdb {

TableFunction SemanticViewsTableFunction(const string &name, vector<LogicalType> arguments, table_function_t scan,
                                         table_function_bind_t bind, table_function_init_global_t init,
                                         shared_ptr<SemanticViewsState> state) {
	TableFunction function(Identifier(name), std::move(arguments), scan, bind, init);
	function.function_info = make_shared_ptr<SemanticViewsFunctionInfo>(std::move(state));
	return function;
}

SemanticViewsState &GetSemanticViewsState(TableFunctionBindInput &input) {
	return *input.info->Cast<SemanticViewsFunctionInfo>().state;
}

string StringArgument(const TableFunctionBindInput &input, idx_t index) {
	auto &value = input.inputs[index];
	return value.IsNull() ? string() : StringValue::Get(value);
}

SemanticQualifiedName NameArguments(const TableFunctionBindInput &input, idx_t first) {
	SemanticQualifiedName name;
	name.catalog = StringArgument(input, first);
	name.schema = StringArgument(input, first + 1);
	name.name = StringArgument(input, first + 2);
	return name;
}

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

void RegisterSemanticViewsFunctions(ExtensionLoader &loader, shared_ptr<SemanticViewsState> state) {
	RegisterSemanticDDLFunctions(loader, state);
	RegisterSemanticShowFunctions(loader, state);
	RegisterSemanticQueryFunctions(loader, state);
	RegisterSemanticPlaceholderFunction(loader, state);
}

} // namespace duckdb
