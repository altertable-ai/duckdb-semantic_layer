#include "semantic_analysis.hpp"

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

static constexpr const char *SEMANTIC_ATOM_MARKER = "__sv_atom__";

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
	idx_t member;
	if (SemanticModel::IsMemberMarker(expr, member)) {
		return model.members[member].Kind() == SemanticMemberKind::METRIC;
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
	idx_t table;
	string column;
	if (SemanticModel::IsColumnMarker(expr, table, column)) {
		tables.insert(table);
		return;
	}
	idx_t member;
	if (SemanticModel::IsMemberMarker(expr, member)) {
		auto &entry = model.members[member];
		if (entry.IsRowLevel()) {
			tables.insert(entry.table);
			return;
		}
		if (context_table == DConstants::INVALID_INDEX) {
			throw BinderException("%s: metric \"%s\" cannot be used in a row-level expression", what,
			                      model.MemberDisplayName(member));
		}
		tables.insert(context_table);
		return;
	}
	if (expr.GetExpressionClass() == ExpressionClass::WINDOW) {
		throw BinderException("%s: window functions are not allowed in row-level expressions", what);
	}
	if (IsAggregate(expr)) {
		if (context_table == DConstants::INVALID_INDEX) {
			throw BinderException("%s cannot contain an aggregate", what);
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
	for (auto candidate : tables) {
		bool reaches_all = true;
		for (auto other : tables) {
			if (!model.Reaches(candidate, other)) {
				reaches_all = false;
				break;
			}
		}
		if (reaches_all) {
			return candidate;
		}
	}
	vector<string> names;
	for (auto table : tables) {
		names.push_back("\"" + model.tables[table].name + "\"");
	}
	throw BinderException("%s: logical tables %s are not related through relationships", what,
	                      StringUtil::Join(names, ", "));
}

idx_t SemanticAnalyzer::RowHome(const ParsedExpression &expr, const string &what) {
	set<idx_t> tables;
	RowTables(expr, DConstants::INVALID_INDEX, tables, what);
	return FinestTable(tables, what);
}

idx_t SemanticAnalyzer::AggregateHome(const FunctionExpression &aggregate, idx_t context_table, const string &what) {
	set<idx_t> tables;
	ParsedExpressionIterator::EnumerateChildren(
	    aggregate, [&](const ParsedExpression &child) { RowTables(child, context_table, tables, what); });
	if (context_table != DConstants::INVALID_INDEX) {
		tables.insert(context_table);
	}
	if (tables.empty()) {
		throw BinderException("%s: cannot determine the logical table of %s; define it as a metric of a logical "
		                      "table instead",
		                      what, aggregate.ToString());
	}
	return FinestTable(tables, what);
}

bool SemanticAnalyzer::IsExcludingMarker(const ParsedExpression &expr) {
	return expr.GetExpressionClass() == ExpressionClass::FUNCTION &&
	       StringUtil::CIEquals(expr.Cast<FunctionExpression>().function_name, SEMANTIC_EXCLUDING_MARKER);
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
			idx_t member;
			if (SemanticModel::IsMemberMarker(key, member)) {
				if (model.members[member].IsRowLevel()) {
					dimensions.insert(member);
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
	}
	idx_t member;
	if (SemanticModel::IsMemberMarker(expr, member) && model.members[member].Kind() == SemanticMemberKind::METRIC) {
		WindowDimensions(*model.members[member].resolved, dimensions);
		return;
	}
	ParsedExpressionIterator::EnumerateChildren(
	    expr, [&](const ParsedExpression &child) { WindowDimensions(child, dimensions); });
}

unique_ptr<ParsedExpression> SemanticAnalyzer::MakeAtomRef(idx_t atom) {
	vector<string> names {SEMANTIC_ATOM_MARKER, to_string(atom)};
	return make_uniq<ColumnRefExpression>(std::move(names));
}

bool SemanticAnalyzer::IsAtomRef(const ParsedExpression &expr, idx_t &atom) {
	if (expr.GetExpressionClass() != ExpressionClass::COLUMN_REF) {
		return false;
	}
	auto &colref = expr.Cast<ColumnRefExpression>();
	if (colref.column_names.size() != 2 || colref.column_names[0] != SEMANTIC_ATOM_MARKER) {
		return false;
	}
	atom = std::stoull(colref.column_names[1]);
	return true;
}

unique_ptr<ParsedExpression> SemanticAnalyzer::Decompose(const ParsedExpression &expr, idx_t context_table,
                                                         vector<SemanticAtom> &atoms, const string &what,
                                                         const vector<idx_t> &using_relationships,
                                                         idx_t non_additive_member) {
	auto result = expr.Copy();
	std::function<void(unique_ptr<ParsedExpression> &)> rewrite_window_key;
	std::function<void(unique_ptr<ParsedExpression> &)> rewrite = [&](unique_ptr<ParsedExpression> &node) {
		if (IsAggregate(*node)) {
			SemanticAtom atom;
			atom.home = AggregateHome(node->Cast<FunctionExpression>(), context_table, what);
			atom.aggregate = node->Copy();
			atom.context_table = context_table;
			atom.using_relationships = using_relationships;
			atom.non_additive_member = non_additive_member;
			atoms.push_back(std::move(atom));
			auto alias = node->alias;
			node = MakeAtomRef(atoms.size() - 1);
			node->alias = alias;
			return;
		}
		idx_t member;
		if (SemanticModel::IsMemberMarker(*node, member)) {
			auto &entry = model.members[member];
			if (entry.Kind() == SemanticMemberKind::METRIC) {
				auto alias = node->alias;
				auto &member_using =
				    entry.using_relationships.empty() ? using_relationships : entry.using_relationships;
				auto member_non_additive = entry.non_additive.empty() ? non_additive_member : member;
				node = Decompose(*entry.resolved, entry.table, atoms, what, member_using, member_non_additive);
				node->alias = alias;
				return;
			}
			throw BinderException("%s cannot reference \"%s\" outside of an aggregate", what,
			                      model.MemberDisplayName(member));
		}
		idx_t table;
		string column;
		if (SemanticModel::IsColumnMarker(*node, table, column)) {
			throw BinderException("%s cannot reference \"%s.%s\" outside of an aggregate", what,
			                      model.tables[table].name, column);
		}
		if (node->GetExpressionClass() == ExpressionClass::WINDOW) {
			// window-function metric: the window is evaluated over the aggregated rows. Its arguments are
			// metric-level; PARTITION BY / ORDER BY keep their dimension references, which the compiler maps onto
			// the query's dimension columns.
			auto &window = node->Cast<WindowExpression>();
			for (auto &child : window.children) {
				rewrite(child);
			}
			if (window.filter_expr) {
				rewrite(window.filter_expr);
			}
			for (auto &partition : window.partitions) {
				rewrite_window_key(partition);
			}
			for (auto &order : window.orders) {
				rewrite_window_key(order.expression);
			}
			return;
		}
		ParsedExpressionIterator::EnumerateChildren(*node, rewrite);
	};
	rewrite_window_key = [&](unique_ptr<ParsedExpression> &node) {
		idx_t member;
		if (SemanticModel::IsMemberMarker(*node, member) && model.members[member].IsRowLevel()) {
			return;
		}
		if (IsExcludingMarker(*node)) {
			for (auto &child : node->Cast<FunctionExpression>().children) {
				if (!SemanticModel::IsMemberMarker(*child, member) || !model.members[member].IsRowLevel()) {
					throw BinderException("%s: PARTITION BY EXCLUDING only accepts dimensions", what);
				}
			}
			return;
		}
		idx_t table;
		string column;
		if (SemanticModel::IsColumnMarker(*node, table, column)) {
			throw BinderException("%s: the window clause must reference dimensions or metrics, not column \"%s.%s\"",
			                      what, model.tables[table].name, column);
		}
		if (IsAggregate(*node) || SemanticModel::IsMemberMarker(*node, member)) {
			rewrite(node);
			return;
		}
		ParsedExpressionIterator::EnumerateChildren(*node, rewrite_window_key);
	};
	rewrite(result);
	return result;
}

} // namespace duckdb
