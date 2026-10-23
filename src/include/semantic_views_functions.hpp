//===----------------------------------------------------------------------===//
//                         DuckDB - semantic_views
//
// semantic_views_functions.hpp
//
// The table functions semantic-view statements are lowered onto
// (semantic_lowering.hpp), and helpers shared by their implementations.
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb.hpp"
#include "duckdb/function/table_function.hpp"
#include "duckdb/parser/qualified_name.hpp"
#include "parsed_semantic_view.hpp"

namespace duckdb {

class ExtensionLoader;
struct SemanticViewsState;

void RegisterSemanticViewsFunctions(ExtensionLoader &loader, shared_ptr<SemanticViewsState> state);
void RegisterSemanticDDLFunctions(ExtensionLoader &loader, shared_ptr<SemanticViewsState> state);
void RegisterSemanticShowFunctions(ExtensionLoader &loader, shared_ptr<SemanticViewsState> state);
void RegisterSemanticQueryFunctions(ExtensionLoader &loader, shared_ptr<SemanticViewsState> state);

//! A table function that carries the extension state
TableFunction SemanticViewsTableFunction(const string &name, vector<LogicalType> arguments, table_function_t scan,
                                         table_function_bind_t bind, table_function_init_global_t init,
                                         shared_ptr<SemanticViewsState> state);

SemanticViewsState &GetSemanticViewsState(TableFunctionBindInput &input);
string StringArgument(const TableFunctionBindInput &input, idx_t index);
//! (catalog, schema, name) arguments starting at `first`
SemanticQualifiedName NameArguments(const TableFunctionBindInput &input, idx_t first);

//! Bind data / scan for table functions whose rows are computed at bind time
struct RowsFunctionData : public TableFunctionData {
	vector<vector<Value>> rows;
};
unique_ptr<GlobalTableFunctionState> RowsFunctionInit(ClientContext &context, TableFunctionInitInput &input);
void RowsFunctionScan(ClientContext &context, TableFunctionInput &data_p, DataChunk &output);

} // namespace duckdb
