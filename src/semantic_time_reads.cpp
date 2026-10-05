#include "semantic_definition_exception.hpp"
#include "semantic_time_reads.hpp"

#include "duckdb/common/string_util.hpp"
#include "duckdb/parser/parsed_expression_iterator.hpp"

namespace duckdb {

namespace {

//! What a row-level expression reads from a row of logical table `table`
struct Read {
	idx_t table;
	//! The column read; empty for an aggregation
	string column;
	//! A child aggregation: the rows of finer tables belonging to the row
	bool aggregation;

	bool IsColumn(idx_t other_table, const string &other_column) const {
		return !aggregation && table == other_table && StringUtil::CIEquals(column, other_column);
	}
	//! "column "dates.d_year"", or "an aggregation on "users""
	string Name(const SemanticModel &model) const {
		auto &name = model.tables[table].name;
		return aggregation ? "an aggregation on \"" + name + "\"" : "column \"" + name + "." + column + "\"";
	}
};

//! What row-level expression `expr` reads, following row-level members
void CollectReads(const SemanticModel &model, SemanticAnalyzer &analyzer, const ParsedExpression &expr,
                  vector<Read> &reads) {
	auto ref = SemanticMarkers::Classify(expr);
	if (ref.Is(SemanticRefKind::COLUMN)) {
		reads.push_back(Read {ref.index, ref.column, false});
		return;
	}
	if (ref.Is(SemanticRefKind::MEMBER)) {
		auto &member = model.members[ref.index];
		if (member.IsRowLevel()) {
			if (analyzer.ContainsAggregateOrMetric(*member.resolved)) {
				reads.push_back(Read {member.table, string(), true});
			}
			CollectReads(model, analyzer, *member.resolved, reads);
		}
		return;
	}
	ParsedExpressionIterator::EnumerateChildren(
	    expr, [&](const ParsedExpression &child) { CollectReads(model, analyzer, child, reads); });
}

//! Whether `read` moves with column `column` of table `table` in a block over `home`: it is that column, or it reads
//! neither that table (other than through the column) nor a table reached through it
bool MovesWith(const SemanticModel &model, const Read &read, idx_t table, const string &column, idx_t home) {
	if (read.table == table) {
		return table == home || read.IsColumn(table, column);
	}
	return read.table == home || !model.Reaches(table, read.table);
}

} // namespace

bool SemanticReadsColumn(const SemanticModel &model, SemanticAnalyzer &analyzer, const ParsedExpression &expr,
                         idx_t table, const string &column) {
	vector<Read> reads;
	CollectReads(model, analyzer, expr, reads);
	return std::any_of(reads.begin(), reads.end(), [&](const Read &read) { return read.IsColumn(table, column); });
}

void CheckSemanticTimeReads(const SemanticModel &model, SemanticAnalyzer &analyzer, const SemanticQuerySpec &spec) {
	struct ItemReads {
		string what;
		vector<Read> reads;
	};
	vector<ItemReads> items;
	for (auto &item : spec.items) {
		if (item.kind == SemanticMemberKind::DIMENSION) {
			items.push_back(ItemReads {SemanticItemWhat(item), {}});
			CollectReads(model, analyzer, *item.resolved, items.back().reads);
		}
	}
	if (spec.where) {
		items.push_back(ItemReads {"Invalid WHERE clause", {}});
		CollectReads(model, analyzer, *spec.where, items.back().reads);
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
				for (auto &read_item : items) {
					for (auto &read : read_item.reads) {
						if (!MovesWith(model, read, cumulative.table, cumulative.column, atom.home)) {
							throw SemanticDefinitionException(
							    "%s: %s would not move with the CUMULATIVE anchor date of metric \"%s\" (only \"%s\" "
							    "and expressions computed from it do)",
							    read_item.what, read.Name(model), item.output_name,
							    model.MemberDisplayName(cumulative.dimension));
						}
					}
				}
			}
			for (auto o : atom.context.offsets) {
				auto &offset = model.offsets[o];
				bool shifted = false;
				for (auto &read_item : items) {
					for (auto &read : read_item.reads) {
						if (read.IsColumn(offset.table, offset.column)) {
							shifted = true;
						} else if (!MovesWith(model, read, offset.table, offset.column, atom.home)) {
							throw SemanticDefinitionException(
							    "%s: %s would not be shifted by the OFFSET of metric \"%s\" (only \"%s\" and "
							    "expressions computed from it are)",
							    read_item.what, read.Name(model), item.output_name,
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
