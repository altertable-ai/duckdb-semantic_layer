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
	//! A row-level expression, evaluated per row of `scope`'s base in the row context of `context_table`
	unique_ptr<ParsedExpression> Row(const ParsedExpression &expr, SemanticJoinScope &scope, idx_t context_table,
	                                 const string &what);
	//! A query dimension or WHERE condition in an aggregation block: Row() with the column reads shifted by the block's
	//! `offsets` (SemanticModel::offsets). The block's aggregates, FILTERs and NON ADDITIVE BY are not shifted.
	unique_ptr<ParsedExpression> ShiftedRow(const ParsedExpression &expr, SemanticJoinScope &scope, const string &what,
	                                        const vector<idx_t> &offsets);
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
	unique_ptr<ParsedExpression> CompileRow(const ParsedExpression &expr, SemanticJoinScope &scope, idx_t context_table,
	                                        const string &what, const vector<idx_t> &offsets);
	unique_ptr<ParsedExpression> ChildAggregate(const SemanticAtom &atom, idx_t context, SemanticJoinScope &outer,
	                                            const string &what);
	const SemanticModel &model;
	SemanticAnalyzer &analyzer;
};

} // namespace duckdb
