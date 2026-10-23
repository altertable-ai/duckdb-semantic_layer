#include "semantic_time_spine.hpp"

#include "duckdb/parser/expression/cast_expression.hpp"
#include "duckdb/parser/expression/constant_expression.hpp"
#include "duckdb/parser/tableref/emptytableref.hpp"
#include "semantic_ast_helpers.hpp"

namespace duckdb {

bool SemanticIsTimeType(const LogicalType &type) {
	return type.id() == LogicalTypeId::DATE || LogicalType::TypeIsTimestamp(type);
}

unique_ptr<ParsedExpression> SemanticTimeSpineDay(unique_ptr<ParsedExpression> value, const LogicalType &type) {
	if (type.id() == LogicalTypeId::TIMESTAMP_TZ) {
		value = make_uniq<CastExpression>(LogicalType::TIMESTAMP, std::move(value));
	}
	return make_uniq<CastExpression>(LogicalType::DATE, std::move(value));
}

SemanticTable SemanticTimeSpineTable() {
	SemanticTable spine;
	spine.name = SEMANTIC_TIME_SPINE_TABLE;
	spine.comment = "Time spine";
	// SELECT CAST(NULL AS DATE) AS day WHERE FALSE
	auto rows = make_uniq<SelectNode>();
	auto day = make_uniq<CastExpression>(LogicalType::DATE, ConstantExpression::FromValue(Value()));
	day->SetAlias(Identifier(SEMANTIC_TIME_SPINE_COLUMN));
	rows->select_list.push_back(std::move(day));
	rows->from_table = make_uniq<EmptyTableRef>();
	rows->where_clause = ConstantExpression::FromValue(Value::BOOLEAN(false));
	spine.source = MakeSubqueryRef(std::move(rows), string());
	spine.column_names = {SEMANTIC_TIME_SPINE_COLUMN};
	spine.column_types = {LogicalType::DATE};
	spine.columns[SEMANTIC_TIME_SPINE_COLUMN] = 0;
	spine.primary_key = {SEMANTIC_TIME_SPINE_COLUMN};
	return spine;
}

vector<unique_ptr<ParsedSemanticMember>> SemanticTimeSpineDimensions() {
	vector<unique_ptr<ParsedSemanticMember>> result;
	for (auto grain : {SEMANTIC_TIME_SPINE_COLUMN, "week", "month", "quarter", "year"}) {
		auto dimension = make_uniq<ParsedSemanticMember>();
		dimension->kind = SemanticMemberKind::DIMENSION;
		dimension->table = SEMANTIC_TIME_SPINE_TABLE;
		dimension->name = grain;
		// day is the column itself (OFFSET and CUMULATIVE need a dimension defined as a column)
		dimension->expression = ColumnRef(SEMANTIC_TIME_SPINE_COLUMN);
		if (dimension->name != SEMANTIC_TIME_SPINE_COLUMN) {
			vector<unique_ptr<ParsedExpression>> arguments;
			arguments.push_back(ConstantExpression::FromValue(Value(grain)));
			arguments.push_back(std::move(dimension->expression));
			dimension->expression =
			    make_uniq<CastExpression>(LogicalType::DATE, Call("date_trunc", std::move(arguments)));
		}
		result.push_back(std::move(dimension));
	}
	return result;
}

} // namespace duckdb
