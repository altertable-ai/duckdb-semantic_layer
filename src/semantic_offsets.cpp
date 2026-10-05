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

//! Columns read by a row-level expression, following row-level members. With `analyzer`, a member aggregating the
//! rows that belong to its row also reads that row: a column without name of the member's table.
static void CollectColumns(const SemanticModel &model, const ParsedExpression &expr, vector<SemanticRef> &columns,
                           optional_ptr<SemanticAnalyzer> analyzer = nullptr) {
	auto ref = SemanticMarkers::Classify(expr);
	if (ref.Is(SemanticRefKind::COLUMN)) {
		columns.push_back(ref);
		return;
	}
	if (ref.Is(SemanticRefKind::MEMBER)) {
		auto &member = model.members[ref.index];
		if (member.IsRowLevel()) {
			if (analyzer && analyzer->ContainsAggregateOrMetric(*member.resolved)) {
				SemanticRef row;
				row.kind = SemanticRefKind::COLUMN;
				row.index = member.table;
				columns.push_back(row);
			}
			CollectColumns(model, *member.resolved, columns, analyzer);
		}
		return;
	}
	ParsedExpressionIterator::EnumerateChildren(
	    expr, [&](const ParsedExpression &child) { CollectColumns(model, child, columns, analyzer); });
}

//! Whether `read` moves with column `column` of table `table` in a block over `home`: it is that column, or it reads
//! neither that table (other than through the column) nor a table reached through it
static bool MovesWith(const SemanticModel &model, const SemanticRef &read, idx_t table, const string &column,
                      idx_t home) {
	if (read.index == table) {
		return table == home || (!read.column.empty() && StringUtil::CIEquals(read.column, column));
	}
	return read.index == home || !model.Reaches(table, read.index);
}

//! "dates.d_year", or the table of an aggregation
static string ReadName(const SemanticModel &model, const SemanticRef &read) {
	auto &table = model.tables[read.index].name;
	return read.column.empty() ? "an aggregation on \"" + table + "\"" : "column \"" + table + "." + read.column + "\"";
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
			CollectColumns(model, *item.resolved, reads.back().columns, analyzer);
		}
	}
	if (spec.where) {
		reads.push_back(Read {"Invalid WHERE clause", {}});
		CollectColumns(model, *spec.where, reads.back().columns, analyzer);
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
						if (!MovesWith(model, column, cumulative.table, cumulative.column, atom.home)) {
							throw SemanticDefinitionException(
							    "%s: %s would not move with the CUMULATIVE anchor date of metric \"%s\" (only \"%s\" "
							    "and expressions computed from it do)",
							    read.what, ReadName(model, column), item.output_name,
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
						if (column.index == offset.table && StringUtil::CIEquals(column.column, offset.column)) {
							shifted = true;
						} else if (!MovesWith(model, column, offset.table, offset.column, atom.home)) {
							throw SemanticDefinitionException(
							    "%s: %s would not be shifted by the OFFSET of metric \"%s\" (only \"%s\" and "
							    "expressions computed from it are)",
							    read.what, ReadName(model, column), item.output_name,
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
