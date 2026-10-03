#include "semantic_markers.hpp"

#include "duckdb/common/string_util.hpp"
#include "duckdb/common/to_string.hpp"
#include "duckdb/parser/expression/columnref_expression.hpp"
#include "duckdb/parser/expression/function_expression.hpp"

namespace duckdb {

static constexpr const char *COLUMN_MARKER = "__sv_col__";
static constexpr const char *MEMBER_MARKER = "__sv_mem__";
static constexpr const char *ATOM_MARKER = "__sv_atom__";

unique_ptr<ParsedExpression> SemanticMarkers::Column(idx_t table, const string &column) {
	vector<string> names {COLUMN_MARKER, to_string(table), column};
	return make_uniq<ColumnRefExpression>(std::move(names));
}

unique_ptr<ParsedExpression> SemanticMarkers::Member(idx_t member) {
	vector<string> names {MEMBER_MARKER, to_string(member)};
	return make_uniq<ColumnRefExpression>(std::move(names));
}

unique_ptr<ParsedExpression> SemanticMarkers::Atom(idx_t atom) {
	vector<string> names {ATOM_MARKER, to_string(atom)};
	return make_uniq<ColumnRefExpression>(std::move(names));
}

SemanticRef SemanticMarkers::Classify(const ParsedExpression &expr) {
	SemanticRef result;
	if (expr.GetExpressionClass() != ExpressionClass::COLUMN_REF) {
		return result;
	}
	auto &names = expr.Cast<ColumnRefExpression>().column_names;
	if (names.size() == 3 && names[0] == COLUMN_MARKER) {
		result.kind = SemanticRefKind::COLUMN;
		result.column = names[2];
	} else if (names.size() == 2 && names[0] == MEMBER_MARKER) {
		result.kind = SemanticRefKind::MEMBER;
	} else if (names.size() == 2 && names[0] == ATOM_MARKER) {
		result.kind = SemanticRefKind::ATOM;
	} else {
		return result;
	}
	result.index = std::stoull(names[1]);
	return result;
}

bool SemanticMarkers::IsExcluding(const ParsedExpression &expr) {
	return expr.GetExpressionClass() == ExpressionClass::FUNCTION &&
	       StringUtil::CIEquals(expr.Cast<FunctionExpression>().function_name, SEMANTIC_EXCLUDING_FUNCTION);
}

} // namespace duckdb
