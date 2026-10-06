#include "semantic_offsets.hpp"

#include "duckdb/parser/expression/cast_expression.hpp"
#include "duckdb/parser/expression/function_expression.hpp"

namespace duckdb {

unique_ptr<ParsedExpression> SemanticShiftColumn(unique_ptr<ParsedExpression> column, const SemanticOffset &offset) {
	vector<unique_ptr<ParsedExpression>> children;
	children.push_back(std::move(column));
	children.push_back(offset.interval->Copy());
	auto sum = make_uniq<FunctionExpression>("+", std::move(children), nullptr, nullptr, false, true);
	return make_uniq<CastExpression>(offset.type, std::move(sum));
}

unique_ptr<ParsedExpression> SemanticOffsetMap::Column(idx_t table, const string &column,
                                                       unique_ptr<ParsedExpression> compiled) {
	for (auto o : offsets) {
		auto &offset = model.offsets[o];
		if (offset.table == table && StringUtil::CIEquals(offset.column, column)) {
			compiled = SemanticShiftColumn(std::move(compiled), offset);
		}
	}
	return compiled;
}

} // namespace duckdb
