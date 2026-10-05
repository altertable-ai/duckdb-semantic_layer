#include "semantic_definition_exception.hpp"
#include "semantic_offsets.hpp"

#include "duckdb/parser/expression/cast_expression.hpp"
#include "duckdb/parser/expression/function_expression.hpp"
#include "duckdb/parser/parsed_expression_iterator.hpp"

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

//! Columns read by a row-level expression, following row-level members
static void CollectColumns(const SemanticModel &model, const ParsedExpression &expr, vector<SemanticRef> &columns) {
	auto ref = SemanticMarkers::Classify(expr);
	if (ref.Is(SemanticRefKind::COLUMN)) {
		columns.push_back(ref);
		return;
	}
	if (ref.Is(SemanticRefKind::MEMBER)) {
		if (model.members[ref.index].IsRowLevel()) {
			CollectColumns(model, *model.members[ref.index].resolved, columns);
		}
		return;
	}
	ParsedExpressionIterator::EnumerateChildren(
	    expr, [&](const ParsedExpression &child) { CollectColumns(model, child, columns); });
}

bool SemanticReadsColumn(const SemanticModel &model, const ParsedExpression &expr, idx_t table, const string &column) {
	vector<SemanticRef> columns;
	CollectColumns(model, expr, columns);
	for (auto &read : columns) {
		if (read.index == table && StringUtil::CIEquals(read.column, column)) {
			return true;
		}
	}
	return false;
}

void CheckSemanticTimeReads(const SemanticModel &model, SemanticAnalyzer &analyzer, const SemanticQuerySpec &spec) {
	struct Read {
		string what;
		vector<SemanticRef> columns;
	};
	vector<Read> reads;
	for (auto &item : spec.items) {
		if (item.kind == SemanticMemberKind::DIMENSION) {
			reads.push_back(Read {SemanticItemWhat(item), {}});
			CollectColumns(model, *item.resolved, reads.back().columns);
		}
	}
	if (spec.where) {
		reads.push_back(Read {"Invalid WHERE clause", {}});
		CollectColumns(model, *spec.where, reads.back().columns);
	}
	for (auto &item : spec.items) {
		if (item.kind != SemanticMemberKind::METRIC) {
			continue;
		}
		vector<SemanticAtom> atoms;
		analyzer.Decompose(*item.resolved, SemanticMetricContext(), atoms, SemanticItemWhat(item));
		for (auto &atom : atoms) {
			if (atom.context.cumulative != DConstants::INVALID_INDEX) {
				auto &cumulative = model.cumulatives[atom.context.cumulative];
				for (auto &read : reads) {
					for (auto &column : read.columns) {
						if (column.index == cumulative.table && cumulative.table != atom.home &&
						    !StringUtil::CIEquals(column.column, cumulative.column)) {
							throw SemanticDefinitionException(
							    "%s: column \"%s.%s\" would not move with the CUMULATIVE anchor date of metric \"%s\" "
							    "(only \"%s\" and expressions computed from it do)",
							    read.what, model.tables[cumulative.table].name, column.column, item.output_name,
							    model.MemberDisplayName(cumulative.dimension));
						}
					}
				}
			}
			for (auto o : atom.context.offsets) {
				auto &offset = model.offsets[o];
				bool shifted = false;
				for (auto &read : reads) {
					for (auto &column : read.columns) {
						if (column.index != offset.table) {
							continue;
						}
						if (StringUtil::CIEquals(column.column, offset.column)) {
							shifted = true;
						} else if (offset.table != atom.home) {
							throw SemanticDefinitionException(
							    "%s: column \"%s.%s\" would not be shifted by the OFFSET of metric \"%s\" (only "
							    "\"%s\" and expressions computed from it are)",
							    read.what, model.tables[offset.table].name, column.column, item.output_name,
							    model.MemberDisplayName(offset.dimension));
						}
					}
				}
				if (!shifted) {
					throw SemanticDefinitionException(
					    "%s: must be queried with a dimension or WHERE condition computed from \"%s\" (its OFFSET "
					    "dimension)",
					    SemanticItemWhat(item), model.MemberDisplayName(offset.dimension));
				}
			}
		}
	}
}

} // namespace duckdb
