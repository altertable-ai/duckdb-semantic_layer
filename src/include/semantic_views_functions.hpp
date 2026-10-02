#pragma once

#include "duckdb.hpp"
#include "duckdb/function/table_function.hpp"

namespace duckdb {

class ExtensionLoader;

//! Bind data for table functions that emit rows computed at bind time
struct RowsFunctionData : public TableFunctionData {
	vector<vector<Value>> rows;
};

unique_ptr<GlobalTableFunctionState> RowsFunctionInit(ClientContext &context, TableFunctionInitInput &input);
void RowsFunctionScan(ClientContext &context, TableFunctionInput &data_p, DataChunk &output);

void RegisterSemanticViewsFunctions(ExtensionLoader &loader);

} // namespace duckdb
