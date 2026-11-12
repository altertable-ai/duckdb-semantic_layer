#include "semantic_definition_exception.hpp"
#include "semantic_expression_compiler.hpp"

#include "duckdb/common/exception/binder_exception.hpp"
#include "duckdb/catalog/catalog.hpp"
#include "duckdb/catalog/entry_lookup_info.hpp"
#include "duckdb/parser/expression/case_expression.hpp"
#include "duckdb/parser/expression/operator_expression.hpp"
#include "duckdb/planner/binder.hpp"
#include "semantic_ast_helpers.hpp"
#include "semantic_offsets.hpp"
#include "semantic_transforms.hpp"

namespace duckdb {

unique_ptr<ParsedExpression> SemanticExpressionCompiler::Row(const ParsedExpression &expr, SemanticJoinScope &scope,
                                                             idx_t context_table, const string &what) {
	SemanticColumnMap identity;
	return CompileRow(expr, scope, context_table, what, identity);
}

unique_ptr<ParsedExpression> SemanticExpressionCompiler::MappedRow(const ParsedExpression &expr,
                                                                   SemanticJoinScope &scope, const string &what,
                                                                   SemanticColumnMap &map) {
	return CompileRow(expr, scope, DConstants::INVALID_INDEX, what, map);
}

unique_ptr<ParsedExpression> SemanticExpressionCompiler::ShiftedRow(const ParsedExpression &expr,
                                                                    SemanticJoinScope &scope, const string &what,
                                                                    const vector<idx_t> &offsets) {
	SemanticOffsetMap map(analyzer.Transforms(), offsets);
	return MappedRow(expr, scope, what, map);
}

unique_ptr<ParsedExpression> SemanticExpressionCompiler::CompileRow(const ParsedExpression &expr,
                                                                    SemanticJoinScope &scope, idx_t context_table,
                                                                    const string &what, SemanticColumnMap &map) {
	auto dependencies = scope.Dependencies();
	if (dependencies && expr.GetExpressionClass() == ExpressionClass::SUBQUERY) {
		dependencies->complete = false;
		dependencies->Unsupported("a metric uses a subquery");
	}
	if (dependencies && expr.GetExpressionClass() == ExpressionClass::FUNCTION) {
		auto &function = expr.Cast<FunctionExpression>();
		auto name = function.GetQualifiedName();
		Binder::BindSchemaOrCatalog(analyzer.Context(), name);
		auto entry = Catalog::GetEntry(analyzer.Context(), EntryLookupInfo(CatalogType::SCALAR_FUNCTION_ENTRY, name),
		                               OnEntryNotFound::RETURN_NULL);
		if (!entry && !name.Schema().empty()) {
			// DuckDB also accepts receiver-style calls, e.g.
			// amount.scale().
			entry = Catalog::GetEntry(analyzer.Context(),
			                          EntryLookupInfo(CatalogType::SCALAR_FUNCTION_ENTRY, QualifiedName(name.Name())),
			                          OnEntryNotFound::RETURN_NULL);
		}
		if (entry && entry->type == CatalogType::MACRO_ENTRY && !entry->internal) {
			// A user macro can be replaced without advancing any source table's snapshot.
			dependencies->complete = false;
			dependencies->Unsupported("a metric uses a replaceable SQL macro");
		}
	}
	auto ref = SemanticMarkers::Classify(expr);
	unique_ptr<ParsedExpression> result;
	if (ref.Is(SemanticRefKind::COLUMN)) {
		result = map.Column(ref.index, ref.column, scope.ColumnReference(ref.index, ref.column, context_table));
	} else if (ref.Is(SemanticRefKind::MEMBER) && model.members[ref.index].IsRowLevel()) {
		auto &member = model.members[ref.index];
		// a time spine dimension read by a row-level expression of another table is that table's time
		auto table = member.table;
		if (model.IsTimeSpine(table) && context_table != DConstants::INVALID_INDEX) {
			table = context_table;
		}
		result = CompileRow(*member.resolved, scope, table, what, map);
	} else if (ref.Is(SemanticRefKind::MEMBER)) {
		// a metric in a row-level expression: aggregate the finer rows belonging to the current row
		vector<SemanticAtom> atoms;
		auto post = analyzer.DecomposeMetric(ref.index, atoms, what);
		result = map.Opaque(
		    ReplaceAtoms(*post, [&](idx_t atom) { return ChildAggregate(atoms[atom], context_table, scope, what); }));
	} else if (analyzer.IsAggregate(expr)) {
		CheckNoMetricArgument(expr.Cast<FunctionExpression>(), what);
		SemanticAtom atom;
		atom.home = analyzer.AggregateHome(expr.Cast<FunctionExpression>(), context_table, what);
		atom.aggregate = expr.Copy();
		atom.context.table = context_table;
		atom.context.using_relationships = scope.UsingRelationships();
		result = map.Opaque(ChildAggregate(atom, context_table, scope, what));
	} else {
		result = expr.Copy();
		ParsedExpressionIterator::EnumerateChildren(*result, [&](unique_ptr<ParsedExpression> &child) {
			child = CompileRow(*child, scope, context_table, what, map);
		});
		return result;
	}
	result->SetAlias(expr.GetAlias());
	return result;
}

//! A row-level aggregate (of finer rows) cannot aggregate a metric, which is already aggregated
void SemanticExpressionCompiler::CheckNoMetricArgument(const FunctionExpression &aggregate, const string &what) {
	std::function<void(const ParsedExpression &)> check = [&](const ParsedExpression &node) {
		auto ref = SemanticMarkers::Classify(node);
		if (ref.Is(SemanticRefKind::MEMBER) && model.members[ref.index].Kind() == SemanticMemberKind::METRIC) {
			auto name = model.MemberDisplayName(ref.index);
			throw SemanticDefinitionException(
			    "%s: \"%s\" is a metric, which cannot be aggregated again in %s; aggregate the column instead "
			    "(qualify it with its logical table if a member has its name)",
			    what, name, DisplayText(*SemanticDisplayExpression(model, aggregate)));
		}
		if (node.GetExpressionClass() != ExpressionClass::SUBQUERY) {
			ParsedExpressionIterator::EnumerateChildren(node, check);
		}
	};
	for (auto &argument : aggregate.GetArguments()) {
		check(argument.GetExpression());
	}
}

//! An aggregate atom over the rows of `scope`'s base
unique_ptr<ParsedExpression> SemanticExpressionCompiler::Aggregate(const SemanticAtom &atom, SemanticJoinScope &scope,
                                                                   const string &what) {
	auto dependencies = scope.Dependencies();
	if (dependencies && analyzer.IsAggregateMacro(*atom.aggregate)) {
		// A user macro can be replaced without advancing any source table's snapshot.
		dependencies->complete = false;
		dependencies->Unsupported("a metric uses a replaceable SQL macro");
	}
	auto result = atom.aggregate->Copy();
	ParsedExpressionIterator::EnumerateChildren(
	    *result, [&](unique_ptr<ParsedExpression> &child) { child = Row(*child, scope, atom.home, what); });
	return result;
}

//! Correlated scalar subquery aggregating the rows of atom.home that belong to the current row of `context`
unique_ptr<ParsedExpression> SemanticExpressionCompiler::ChildAggregate(const SemanticAtom &atom, idx_t context,
                                                                        SemanticJoinScope &outer, const string &what) {
	if (context == DConstants::INVALID_INDEX || atom.home == context || !model.Reaches(atom.home, context)) {
		throw SemanticDefinitionException("%s: cannot aggregate rows of logical table \"%s\" here", what,
		                                  model.tables[atom.home].name);
	}
	auto path = model.Path(atom.home, context, atom.context.using_relationships);
	auto &last = path.back();
	auto &rel = model.relationships[last.relationship];
	if (rel.asof_index != DConstants::INVALID_INDEX || rel.is_range) {
		throw SemanticDefinitionException(
		    "%s: aggregating the rows of \"%s\" through the ASOF / range relationship \"%s\" "
		    "is not supported",
		    what, model.tables[atom.home].name, rel.name);
	}
	auto inner = outer.Nested(atom.home, context);
	auto node = make_uniq<SelectNode>();
	node->select_list.push_back(Aggregate(atom, inner, what));
	// correlate the last hop into `context` with the current row
	auto source = path.size() >= 2 ? path[path.size() - 2].target : atom.home;
	auto &inner_columns = last.reversed ? rel.to_columns : rel.from_columns;
	auto &outer_columns = last.reversed ? rel.from_columns : rel.to_columns;
	auto inner_alias = inner.Ensure(source);
	auto outer_alias = outer.Ensure(context);
	vector<unique_ptr<ParsedExpression>> conditions;
	vector<unique_ptr<ParsedExpression>> missing;
	for (idx_t c = 0; c < inner_columns.size(); c++) {
		conditions.push_back(Compare(ExpressionType::COMPARE_EQUAL, ColumnRef(inner_columns[c], inner_alias),
		                             ColumnRef(outer_columns[c], outer_alias)));
		missing.push_back(
		    make_uniq<OperatorExpression>(ExpressionType::OPERATOR_IS_NULL, ColumnRef(outer_columns[c], outer_alias)));
	}
	node->where_clause = AndAll(std::move(conditions));
	node->from_table = inner.BuildFrom();
	if (auto dependencies = outer.Dependencies()) {
		if (dependencies->in_dimension || outer.Depth() > 0 || path.size() != 1 || !inner.ReadsOnlyBase()) {
			dependencies->Unsupported(
			    "a metric aggregates rows of logical table \"" + model.tables[atom.home].name +
			    "\" in a way incremental refresh does not follow (only aggregations of the columns of a table "
			    "directly referencing a table of the materialization are)");
		} else {
			bool recorded = false;
			for (auto &child : dependencies->children) {
				recorded = recorded || (child.table == atom.home && child.parent == context &&
				                        child.columns == inner_columns && child.parent_columns == outer_columns);
			}
			if (!recorded) {
				dependencies->children.push_back({atom.home, context, inner_columns, outer_columns});
			}
		}
	}
	// a row without a `context` row (an orphan, through a LEFT JOIN) has no aggregation, rather than an empty one
	auto result = make_uniq<CaseExpression>();
	CaseCheck check;
	check.when_expr = OrAll(std::move(missing));
	check.then_expr = make_uniq<ConstantExpression>(Literal::Null());
	result->CaseChecksMutable().push_back(std::move(check));
	result->ElseMutable() = MakeScalarSubquery(std::move(node));
	return std::move(result);
}

} // namespace duckdb
