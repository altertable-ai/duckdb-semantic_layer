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
    : model(model), context(context), transforms(model) {
}

bool SemanticAnalyzer::IsAggregate(const ParsedExpression &expr) {
	if (expr.GetExpressionClass() != ExpressionClass::FUNCTION) {
		return false;
	}
	auto &function = expr.Cast<FunctionExpression>();
	if (function.IsOperator()) {
		return false;
	}
	auto &function_name = function.GetQualifiedName();
	auto key = function_name.ToString();
	auto entry = aggregate_cache.find(key);
	if (entry != aggregate_cache.end()) {
		return entry->second;
	}
	auto catalog_entry = Catalog::GetEntry(
	    context, EntryLookupInfo(CatalogType::AGGREGATE_FUNCTION_ENTRY, function_name), OnEntryNotFound::RETURN_NULL);
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
		    "%s: cannot determine the logical table of %s; reference a column of a logical table in it, or "
		    "define it as a metric of a logical table instead",
		    what, DisplayText(aggregate));
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
		for (auto &partition : window.Partitions()) {
			collect(*partition);
		}
		for (auto &order : window.OrderBy()) {
			collect(*order.expression);
		}
		for (auto &order : window.ArgOrders()) {
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
	if (member.offset.IsValid()) {
		result.offsets.push_back(member.offset.GetIndex());
	}
	result.offsets.insert(result.offsets.end(), outer.offsets.begin(), outer.offsets.end());
	result.cumulative = member.cumulative.IsValid() ? member.cumulative.GetIndex() : outer.cumulative;
	return result;
}

void SemanticAnalyzer::ApplyMetricContext(SemanticAtom &atom, const string &what) {
	for (auto o : atom.context.offsets) {
		auto &offset = transforms.Offset(o);
		if (!model.Reaches(atom.home, offset.table)) {
			throw SemanticDefinitionException("%s: OFFSET dimension \"%s\" is not reachable from logical table \"%s\"",
			                                  what, model.MemberDisplayName(offset.dimension),
			                                  model.tables[atom.home].name);
		}
	}
	if (atom.context.cumulative != DConstants::INVALID_INDEX) {
		auto &cumulative = transforms.Cumulative(atom.context.cumulative);
		if (!model.Reaches(atom.home, cumulative.table)) {
			throw SemanticDefinitionException(
			    "%s: CUMULATIVE dimension \"%s\" is not reachable from logical table \"%s\"", what,
			    model.MemberDisplayName(cumulative.dimension), model.tables[atom.home].name);
		}
	}
	auto &aggregate = atom.aggregate->Cast<FunctionExpression>();
	for (auto filter : atom.context.filters) {
		set<idx_t> tables;
		RowTables(transforms.Filter(filter), DConstants::INVALID_INDEX, tables, what);
		for (auto table : tables) {
			if (!model.Reaches(atom.home, table)) {
				throw SemanticDefinitionException(
				    "%s: %s references logical table \"%s\", which is not reachable from \"%s\" (aggregated by %s)",
				    what, transforms.FilterDisplayName(filter), model.tables[table].name, model.tables[atom.home].name,
				    SemanticDisplayExpression(model, aggregate)->ToString());
			}
		}
	}
	if (atom.context.non_additive_member != DConstants::INVALID_INDEX) {
		// the last snapshot is picked among the filtered rows; equal conditions (of metrics or query-time filters)
		// filter the same rows, so they share an aggregation
		for (auto filter : atom.context.filters) {
			atom.row_filters.push_back(transforms.CanonicalFilter(filter));
		}
	} else if (!atom.context.filters.empty()) {
		vector<unique_ptr<ParsedExpression>> conditions;
		if (aggregate.Filter()) {
			conditions.push_back(std::move(aggregate.FilterMutable()));
		}
		for (auto filter : atom.context.filters) {
			conditions.push_back(transforms.Filter(filter).Copy());
		}
		aggregate.FilterMutable() = AndAll(std::move(conditions));
	}
	atom.context.filters.clear();
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
		auto alias = node->GetAlias();
		auto transform = SemanticMarkers::ClassifyTransform(*node);
		if (transform.kind != SemanticTransformKind::NONE) {
			// a query-time transform applies like the clause of an inlined metric (innermost first)
			auto inner = context;
			switch (transform.kind) {
			case SemanticTransformKind::OFFSET:
				inner.offsets.insert(inner.offsets.begin(), transform.index);
				break;
			case SemanticTransformKind::CUMULATIVE:
				// cumulative() only wraps plain metrics (SemanticResolveTransforms)
				D_ASSERT(context.cumulative == DConstants::INVALID_INDEX);
				inner.cumulative = transform.index;
				break;
			case SemanticTransformKind::FILTER:
				// outer filters first, as for a metric defined with FILTER (MetricContext)
				inner.filters.push_back(transform.index);
				break;
			default:
				throw InternalException("Unexpected semantic transform kind");
			}
			auto &argument = node->Cast<FunctionExpression>().GetArguments()[1].GetExpression();
			node = Decompose(argument, inner, atoms, what);
			node->SetAlias(Identifier(alias));
			return;
		}
		if (IsAggregate(*node)) {
			SemanticAtom atom;
			atom.home = AggregateHome(node->Cast<FunctionExpression>(), context.table, what);
			atom.aggregate = node->Copy();
			atom.context = context;
			ApplyMetricContext(atom, what);
			atoms.push_back(std::move(atom));
			node = SemanticMarkers::Atom(atoms.size() - 1);
			node->SetAlias(Identifier(alias));
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
			node->SetAlias(Identifier(alias));
			return;
		case SemanticRefKind::COLUMN:
			throw SemanticDefinitionException("%s cannot reference \"%s.%s\" outside of an aggregate", what,
			                                  model.tables[ref.index].name, ref.column);
		default:
			break;
		}
		if (node->GetExpressionClass() == ExpressionClass::WINDOW) {
			auto &window = node->Cast<WindowExpression>();
			for (auto &argument : window.GetArgumentsMutable()) {
				rewrite(argument.GetExpressionMutable());
			}
			for (auto *argument : {&window.FilterMutable(), &window.StartExprMutable(), &window.EndExprMutable()}) {
				if (*argument) {
					rewrite(*argument);
				}
			}
			for (auto &partition : window.PartitionsMutable()) {
				rewrite_window_key(partition);
			}
			for (auto &order : window.OrderByMutable()) {
				rewrite_window_key(order.expression);
			}
			for (auto &order : window.ArgOrdersMutable()) {
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
			for (auto &argument : node->Cast<FunctionExpression>().GetArguments()) {
				auto excluded = SemanticMarkers::Classify(argument.GetExpression());
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
		if (IsAggregate(*node) || ref.Is(SemanticRefKind::MEMBER) ||
		    SemanticMarkers::ClassifyTransform(*node).kind != SemanticTransformKind::NONE) {
			rewrite(node);
			return;
		}
		ParsedExpressionIterator::EnumerateChildren(*node, rewrite_window_key);
	};
	rewrite(result);
	return result;
}

} // namespace duckdb
