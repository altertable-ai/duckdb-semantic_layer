#include "semantic_markers.hpp"

#include "duckdb/common/exception.hpp"
#include "duckdb/common/numeric_utils.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/common/to_string.hpp"
#include "duckdb/parser/expression/columnref_expression.hpp"
#include "duckdb/parser/expression/constant_expression.hpp"
#include "duckdb/parser/expression/function_expression.hpp"

namespace duckdb {

static constexpr const char *COLUMN_MARKER = "__sv_col__";
static constexpr const char *MEMBER_MARKER = "__sv_mem__";
static constexpr const char *ATOM_MARKER = "__sv_atom__";
static constexpr const char *OFFSET_MARKER = "__sv_offset";
static constexpr const char *CUMULATIVE_MARKER = "__sv_cumulative";

unique_ptr<ParsedExpression> SemanticMarkers::Column(idx_t table, const string &column) {
	vector<string> names {COLUMN_MARKER, to_string(table), column};
	return make_uniq<ColumnRefExpression>(StringsToIdentifiers(names));
}

unique_ptr<ParsedExpression> SemanticMarkers::Member(idx_t member) {
	vector<string> names {MEMBER_MARKER, to_string(member)};
	return make_uniq<ColumnRefExpression>(StringsToIdentifiers(names));
}

unique_ptr<ParsedExpression> SemanticMarkers::Atom(idx_t atom) {
	vector<string> names {ATOM_MARKER, to_string(atom)};
	return make_uniq<ColumnRefExpression>(StringsToIdentifiers(names));
}

SemanticRef SemanticMarkers::Classify(const ParsedExpression &expr) {
	SemanticRef result;
	if (expr.GetExpressionClass() != ExpressionClass::COLUMN_REF) {
		return result;
	}
	auto &names = expr.Cast<ColumnRefExpression>().ColumnNames();
	if (names.size() == 3 && names[0] == COLUMN_MARKER) {
		result.kind = SemanticRefKind::COLUMN;
		result.column = names[2].GetIdentifierName();
	} else if (names.size() == 2 && names[0] == MEMBER_MARKER) {
		result.kind = SemanticRefKind::MEMBER;
	} else if (names.size() == 2 && names[0] == ATOM_MARKER) {
		result.kind = SemanticRefKind::ATOM;
	} else {
		return result;
	}
	result.index = std::stoull(names[1].GetIdentifierName());
	return result;
}

bool SemanticMarkers::IsExcluding(const ParsedExpression &expr) {
	return expr.GetExpressionClass() == ExpressionClass::FUNCTION &&
	       StringUtil::CIEquals(expr.Cast<FunctionExpression>().FunctionName().GetIdentifierName(),
	                            SEMANTIC_EXCLUDING_FUNCTION);
}

unique_ptr<ParsedExpression> SemanticMarkers::Transform(SemanticTransformKind kind, idx_t index,
                                                        unique_ptr<ParsedExpression> argument) {
	D_ASSERT(kind != SemanticTransformKind::NONE);
	vector<unique_ptr<ParsedExpression>> children;
	children.push_back(ConstantExpression::Integer(NumericCast<int64_t>(index)));
	children.push_back(std::move(argument));
	return make_uniq<FunctionExpression>(kind == SemanticTransformKind::OFFSET ? OFFSET_MARKER : CUMULATIVE_MARKER,
	                                     std::move(children));
}

SemanticTransformRef SemanticMarkers::ClassifyTransform(const ParsedExpression &expr) {
	SemanticTransformRef result;
	if (expr.GetExpressionClass() != ExpressionClass::FUNCTION) {
		return result;
	}
	auto &function = expr.Cast<FunctionExpression>();
	auto &name = function.FunctionName().GetIdentifierName();
	if (name == OFFSET_MARKER) {
		result.kind = SemanticTransformKind::OFFSET;
	} else if (name == CUMULATIVE_MARKER) {
		result.kind = SemanticTransformKind::CUMULATIVE;
	} else {
		return result;
	}
	D_ASSERT(function.GetArguments().size() == 2);
	int64_t index;
	auto &literal = function.GetArguments()[0].GetExpression().Cast<ConstantExpression>().GetLiteral();
	if (!literal.TryGetInt64(index)) {
		throw InternalException("invalid semantic transform marker %s", expr.ToString());
	}
	result.index = NumericCast<idx_t>(index);
	return result;
}

} // namespace duckdb
