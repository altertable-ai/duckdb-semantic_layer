#include "semantic_definition_exception.hpp"
#include "semantic_analysis.hpp"
#include "semantic_ast_helpers.hpp"

#include "duckdb/catalog/catalog.hpp"
#include "duckdb/catalog/entry_lookup_info.hpp"
#include "duckdb/common/exception.hpp"
#include "duckdb/common/exception/binder_exception.hpp"
#include "duckdb/common/exception/catalog_exception.hpp"
#include "duckdb/common/to_string.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/main/client_context.hpp"
#include "duckdb/parser/expression/window_expression.hpp"
#include "duckdb/parser/parsed_expression_iterator.hpp"

namespace duckdb {

SemanticAnalyzer::SemanticAnalyzer(const SemanticModel &model, ClientContext &context)
    : model(model), context(context) {
}

bool SemanticAnalyzer::IsAggregate(const ParsedExpression &expr) {
	if (expr.GetExpressionClass() != ExpressionClass::FUNCTION) {
		return false;
	}
	auto &function = expr.Cast<FunctionExpression>();
	if (function.is_operator) {
		return false;
	}
	auto key = function.catalog + "." + function.schema + "." + function.function_name;
	auto entry = aggregate_cache.find(key);
	if (entry != aggregate_cache.end()) {
		return entry->second;
	}
	EntryLookupInfo lookup(CatalogType::AGGREGATE_FUNCTION_ENTRY, function.function_name);
	auto catalog_entry =
	    Catalog::GetEntry(context, function.catalog, function.schema, lookup, OnEntryNotFound::RETURN_NULL);
	bool is_aggregate = catalog_entry && catalog_entry->type == CatalogType::AGGREGATE_FUNCTION_ENTRY;
	aggregate_cache[key] = is_aggregate;
	return is_aggregate;
}

bool SemanticAnalyzer::ContainsAggregateOrMetric(const ParsedExpression &expr) {
	if (IsAggregate(expr)) {
		return true;
	}
	auto ref = SemanticMarkers::Classify(expr);
	if (ref.Is(SemanticRefKind::MEMBER)) {
		return model.members[ref.index].Kind() == SemanticMemberKind::METRIC;
	}
	bool found = false;
	ParsedExpressionIterator::EnumerateChildren(expr, [&](const ParsedExpression &child) {
		if (!found && ContainsAggregateOrMetric(child)) {
			found = true;
		}
	});
	return found;
}

void SemanticAnalyzer::RowTables(const ParsedExpression &expr, idx_t context_table, set<idx_t> &tables,
                                 const string &what) {
	auto ref = SemanticMarkers::Classify(expr);
	if (ref.Is(SemanticRefKind::COLUMN)) {
		tables.insert(ref.index);
		return;
	}
	if (ref.Is(SemanticRefKind::MEMBER)) {
		auto &entry = model.members[ref.index];
		if (entry.IsRowLevel()) {
			tables.insert(entry.table);
			return;
		}
		if (context_table == DConstants::INVALID_INDEX) {
			throw SemanticDefinitionException("%s: metric \"%s\" cannot be used in a row-level expression", what,
			                                  model.MemberDisplayName(ref.index));
		}
		tables.insert(context_table);
		return;
	}
	if (expr.GetExpressionClass() == ExpressionClass::WINDOW) {
		throw SemanticDefinitionException("%s: window functions are not allowed in row-level expressions", what);
	}
	if (IsAggregate(expr)) {
		if (context_table == DConstants::INVALID_INDEX) {
			throw SemanticDefinitionException("%s cannot contain an aggregate", what);
		}
		tables.insert(context_table);
		return;
	}
	ParsedExpressionIterator::EnumerateChildren(
	    expr, [&](const ParsedExpression &child) { RowTables(child, context_table, tables, what); });
}

idx_t SemanticAnalyzer::FinestTable(const set<idx_t> &tables, const string &what) {
	if (tables.empty()) {
		return DConstants::INVALID_INDEX;
	}
	auto finest = model.FinestTable(tables);
	if (finest.IsValid()) {
		return finest.GetIndex();
	}
	vector<string> names;
	for (auto table : tables) {
		names.push_back("\"" + model.tables[table].name + "\"");
	}
	throw SemanticDefinitionException("%s: logical tables %s are not related through relationships", what,
	                                  StringUtil::Join(names, ", "));
}

idx_t SemanticAnalyzer::AggregateHome(const FunctionExpression &aggregate, idx_t context_table, const string &what) {
	set<idx_t> tables;
	ParsedExpressionIterator::EnumerateChildren(
	    aggregate, [&](const ParsedExpression &child) { RowTables(child, context_table, tables, what); });
	if (context_table != DConstants::INVALID_INDEX) {
		tables.insert(context_table);
	}
	if (tables.empty()) {
		throw SemanticDefinitionException(
		    "%s: cannot determine the logical table of %s; define it as a metric of a logical "
		    "table instead",
		    what, aggregate.ToString());
	}
	return FinestTable(tables, what);
}

bool SemanticAnalyzer::ContainsWindow(const ParsedExpression &expr) {
	if (expr.GetExpressionClass() == ExpressionClass::WINDOW) {
		return true;
	}
	bool found = false;
	ParsedExpressionIterator::EnumerateChildren(
	    expr, [&](const ParsedExpression &child) { found = found || ContainsWindow(child); });
	return found;
}

void SemanticAnalyzer::WindowDimensions(const ParsedExpression &expr, set<idx_t> &dimensions) const {
	if (expr.GetExpressionClass() == ExpressionClass::WINDOW) {
		auto &window = expr.Cast<WindowExpression>();
		std::function<void(const ParsedExpression &)> collect = [&](const ParsedExpression &key) {
			auto ref = SemanticMarkers::Classify(key);
			if (ref.Is(SemanticRefKind::MEMBER)) {
				if (model.members[ref.index].IsRowLevel()) {
					dimensions.insert(ref.index);
				}
				return;
			}
			ParsedExpressionIterator::EnumerateChildren(key, collect);
		};
		for (auto &partition : window.partitions) {
			collect(*partition);
		}
		for (auto &order : window.orders) {
			collect(*order.expression);
		}
		for (auto &order : window.arg_orders) {
			collect(*order.expression);
		}
	}
	auto ref = SemanticMarkers::Classify(expr);
	if (ref.Is(SemanticRefKind::MEMBER) && model.members[ref.index].Kind() == SemanticMemberKind::METRIC) {
		WindowDimensions(*model.members[ref.index].resolved, dimensions);
		return;
	}
	ParsedExpressionIterator::EnumerateChildren(
	    expr, [&](const ParsedExpression &child) { WindowDimensions(child, dimensions); });
}

SemanticMetricContext SemanticAnalyzer::MetricContext(idx_t metric, const SemanticMetricContext &outer) const {
	auto &member = model.members[metric];
	SemanticMetricContext result;
	result.table = member.table;
	result.using_relationships =
	    member.using_relationships.empty() ? outer.using_relationships : member.using_relationships;
	std::sort(result.using_relationships.begin(), result.using_relationships.end());
	result.non_additive_member = member.non_additive.empty() ? outer.non_additive_member : metric;
	result.filters = outer.filters;
	if (member.filter) {
		result.filters.push_back(metric);
	}
	return result;
}

void SemanticAnalyzer::ApplyMetricContext(SemanticAtom &atom, const string &what) {
	if (atom.context.filters.empty()) {
		return;
	}
	auto &aggregate = atom.aggregate->Cast<FunctionExpression>();
	auto description = aggregate.ToString();
	vector<unique_ptr<ParsedExpression>> conditions;
	if (aggregate.filter) {
		conditions.push_back(std::move(aggregate.filter));
	}
	for (auto metric : atom.context.filters) {
		auto &filter = *model.members[metric].filter;
		set<idx_t> tables;
		RowTables(filter, DConstants::INVALID_INDEX, tables, what);
		for (auto table : tables) {
			if (!model.Reaches(atom.home, table)) {
				throw SemanticDefinitionException(
				    "%s: FILTER of metric \"%s\" references logical table \"%s\", which is not reachable from \"%s\" "
				    "(aggregated by %s)",
				    what, model.MemberDisplayName(metric), model.tables[table].name, model.tables[atom.home].name,
				    description);
			}
		}
		conditions.push_back(filter.Copy());
	}
	aggregate.filter = AndAll(std::move(conditions));
}

unique_ptr<ParsedExpression> SemanticAnalyzer::DecomposeMetric(idx_t metric, vector<SemanticAtom> &atoms,
                                                               const string &what) {
	return Decompose(*model.members[metric].resolved, MetricContext(metric, SemanticMetricContext()), atoms, what);
}

unique_ptr<ParsedExpression> SemanticAnalyzer::Decompose(const ParsedExpression &expr,
                                                         const SemanticMetricContext &context,
                                                         vector<SemanticAtom> &atoms, const string &what) {
	auto result = expr.Copy();
	std::function<void(unique_ptr<ParsedExpression> &)> rewrite_window_key;
	std::function<void(unique_ptr<ParsedExpression> &)> rewrite = [&](unique_ptr<ParsedExpression> &node) {
		auto alias = node->alias;
		if (IsAggregate(*node)) {
			SemanticAtom atom;
			atom.home = AggregateHome(node->Cast<FunctionExpression>(), context.table, what);
			atom.aggregate = node->Copy();
			atom.context = context;
			ApplyMetricContext(atom, what);
			atoms.push_back(std::move(atom));
			node = SemanticMarkers::Atom(atoms.size() - 1);
			node->alias = alias;
			return;
		}
		auto ref = SemanticMarkers::Classify(*node);
		switch (ref.kind) {
		case SemanticRefKind::MEMBER:
			if (model.members[ref.index].Kind() != SemanticMemberKind::METRIC) {
				throw SemanticDefinitionException("%s cannot reference \"%s\" outside of an aggregate", what,
				                                  model.MemberDisplayName(ref.index));
			}
			node = Decompose(*model.members[ref.index].resolved, MetricContext(ref.index, context), atoms, what);
			node->alias = alias;
			return;
		case SemanticRefKind::COLUMN:
			throw SemanticDefinitionException("%s cannot reference \"%s.%s\" outside of an aggregate", what,
			                                  model.tables[ref.index].name, ref.column);
		default:
			break;
		}
		if (node->GetExpressionClass() == ExpressionClass::WINDOW) {
			auto &window = node->Cast<WindowExpression>();
			for (auto &child : window.children) {
				rewrite(child);
			}
			for (auto *argument : {&window.filter_expr, &window.start_expr, &window.end_expr, &window.offset_expr,
			                       &window.default_expr}) {
				if (*argument) {
					rewrite(*argument);
				}
			}
			for (auto &partition : window.partitions) {
				rewrite_window_key(partition);
			}
			for (auto &order : window.orders) {
				rewrite_window_key(order.expression);
			}
			for (auto &order : window.arg_orders) {
				rewrite_window_key(order.expression);
			}
			return;
		}
		ParsedExpressionIterator::EnumerateChildren(*node, rewrite);
	};
	rewrite_window_key = [&](unique_ptr<ParsedExpression> &node) {
		auto ref = SemanticMarkers::Classify(*node);
		if (ref.Is(SemanticRefKind::MEMBER) && model.members[ref.index].IsRowLevel()) {
			return;
		}
		if (SemanticMarkers::IsExcluding(*node)) {
			for (auto &child : node->Cast<FunctionExpression>().children) {
				auto excluded = SemanticMarkers::Classify(*child);
				if (!excluded.Is(SemanticRefKind::MEMBER) || !model.members[excluded.index].IsRowLevel()) {
					throw SemanticDefinitionException("%s: PARTITION BY EXCLUDING only accepts dimensions", what);
				}
			}
			return;
		}
		if (ref.Is(SemanticRefKind::COLUMN)) {
			throw SemanticDefinitionException(
			    "%s: the window clause must reference dimensions or metrics, not column \"%s.%s\"", what,
			    model.tables[ref.index].name, ref.column);
		}
		if (IsAggregate(*node) || ref.Is(SemanticRefKind::MEMBER)) {
			rewrite(node);
			return;
		}
		ParsedExpressionIterator::EnumerateChildren(*node, rewrite_window_key);
	};
	rewrite(result);
	return result;
}

} // namespace duckdb
