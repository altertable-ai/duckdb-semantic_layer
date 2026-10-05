#pragma once

#include "semantic_analysis.hpp"
#include "semantic_join_scope.hpp"
#include "duckdb/parser/parsed_expression_iterator.hpp"

namespace duckdb {

class SemanticExpressionCompiler {
public:
	SemanticExpressionCompiler(const SemanticModel &model, SemanticAnalyzer &analyzer)
	    : model(model), analyzer(analyzer) {
	}
	unique_ptr<ParsedExpression> Row(const ParsedExpression &expr, SemanticJoinScope &scope, idx_t context_table,
	                                 const string &what);
	unique_ptr<ParsedExpression> Aggregate(const SemanticAtom &atom, SemanticJoinScope &scope, const string &what);

	template <class FUNC>
	static unique_ptr<ParsedExpression> ReplaceAtoms(const ParsedExpression &expr, FUNC replacement) {
		auto ref = SemanticMarkers::Classify(expr);
		if (ref.Is(SemanticRefKind::ATOM)) {
			auto result = replacement(ref.index);
			result->alias = expr.alias;
			return result;
		}
		auto result = expr.Copy();
		ParsedExpressionIterator::EnumerateChildren(
		    *result, [&](unique_ptr<ParsedExpression> &child) { child = ReplaceAtoms(*child, replacement); });
		return result;
	}

private:
	unique_ptr<ParsedExpression> ChildAggregate(const SemanticAtom &atom, idx_t context, SemanticJoinScope &outer,
	                                            const string &what);
	const SemanticModel &model;
	SemanticAnalyzer &analyzer;
};

} // namespace duckdb
