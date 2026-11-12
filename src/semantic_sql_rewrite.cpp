#include "semantic_sql_rewrite.hpp"

#include "duckdb/catalog/catalog.hpp"
#include "duckdb/catalog/entry_lookup_info.hpp"
#include "duckdb/common/exception/binder_exception.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/common/sql_identifier.hpp"
#include "duckdb/common/to_string.hpp"
#include "duckdb/parser/expression/cast_expression.hpp"
#include "duckdb/parser/expression/columnref_expression.hpp"
#include "duckdb/parser/expression/constant_expression.hpp"
#include "duckdb/parser/expression/function_expression.hpp"
#include "duckdb/parser/expression/operator_expression.hpp"
#include "duckdb/parser/expression/subquery_expression.hpp"
#include "duckdb/parser/expression/window_expression.hpp"
#include "duckdb/parser/parsed_expression_iterator.hpp"
#include "duckdb/parser/query_node/select_node.hpp"
#include "duckdb/parser/result_modifier.hpp"
#include "duckdb/parser/tableref/basetableref.hpp"
#include "duckdb/parser/tableref/table_function_ref.hpp"
#include "semantic_analysis.hpp"
#include "semantic_ast_helpers.hpp"
#include "semantic_catalog.hpp"
#include "semantic_lowering.hpp"
#include "semantic_markers.hpp"
#include "semantic_statement_visitor.hpp"
#include "semantic_layer_registry.hpp"
#include "semantic_transforms.hpp"

namespace duckdb {

namespace {

//! Rewrites one `SELECT ... FROM <semantic view>` query node
class StandardFormRewriter {
public:
	StandardFormRewriter(ClientContext &context, const SemanticModel &model, const BaseTableRef &ref)
	    : model(model), analyzer(model, context), ref(ref) {
		query.view = SemanticQualifiedName::FromQualifiedName(ref.GetQualifiedName());
	}

	void Rewrite(SelectNode &node) {
		CheckSupported(node);
		auto qualify_reads_window_metric = false;
		auto orders = ModifierExpressions(node);
		vector<bool> modifier_aliases;
		for (auto order : orders) {
			modifier_aliases.push_back((*order)->GetExpressionClass() == ExpressionClass::COLUMN_REF &&
			                           !(*order)->Cast<ColumnRefExpression>().IsQualified());
		}
		NormalizeClauses(node, orders);
		qualify_reads_window_metric = qualify && ReadsWindowMetric(*qualify);
		CollectGroups(node);
		auto aggregated = HasAggregation(orders);
		CheckSample(node, aggregated);
		// the items of the semantic query are registered in this order: groups, select list, HAVING, QUALIFY, ORDER BY
		node.select_list = aggregated ? AggregatedSelect() : FactSelect();
		// windows are evaluated by the outer query: after its WHERE (the HAVING), before QUALIFY
		node.where_clause = aggregated && having ? Replace(*having, true) : nullptr;
		if (qualify) {
			node.qualify = aggregated ? Replace(*qualify, true) : MapToFacts(*qualify);
		}
		for (idx_t i = 0; i < orders.size(); i++) {
			auto &order = *orders[i];
			order = aggregated ? Replace(*order, true, modifier_aliases[i]) : MapToFacts(*order, modifier_aliases[i]);
		}
		if (node.qualify && qualify_reads_window_metric && !HasOuterWindow(node, orders)) {
			// it only reads window metrics, computed by the semantic query: without windows to evaluate first, the
			// outer query filters with WHERE (DuckDB rejects a QUALIFY without window functions)
			vector<unique_ptr<ParsedExpression>> conditions;
			if (node.where_clause) {
				conditions.push_back(std::move(node.where_clause));
			}
			conditions.push_back(std::move(node.qualify));
			node.where_clause = AndAll(std::move(conditions));
		}
		node.having = nullptr;
		node.groups = GroupByNode();
		node.aggregate_handling = AggregateHandling::STANDARD_HANDLING;
		node.from_table = SemanticLowering::Query(query);
		node.from_table->alias = OUTER_ALIAS;
	}

private:
	//! The alias of the semantic query in the outer query
	static constexpr const char *OUTER_ALIAS = "__sv_std";

	//! A select list item
	struct SelectItem {
		//! Normalized, with its alias
		unique_ptr<ParsedExpression> expr;
		string alias;
		//! As written
		string text;
		//! The name of a column reference as written
		string column_name;
		//! Whether it contains a window function (evaluated by the outer query)
		bool has_window = false;
		//! Whether it contains an aggregate: AGG(...), a time_shift / cumulative call or an aggregate function
		bool has_aggregate = false;
		//! Whether a subquery in it reads the view's columns (evaluated by the outer query)
		bool correlated = false;

		//! Whether a row-level query evaluates it in the outer query, over the fact columns
		bool Outer() const {
			return has_window || correlated;
		}
	};

	static string GroupColumn(idx_t index) {
		return "__sv_g" + to_string(index);
	}
	static string MetricColumn(idx_t index) {
		return "__sv_m" + to_string(index);
	}
	static string FactColumn(idx_t index) {
		return "__sv_f" + to_string(index);
	}

	void CheckSupported(const SelectNode &node) const {
		if (ref.sample) {
			throw BinderException("TABLESAMPLE is not supported when querying semantic view \"%s\" by name",
			                      model.Name().name);
		}
		if (!ref.column_name_alias.empty()) {
			throw BinderException("Column aliases on semantic view \"%s\" are not supported when querying it by "
			                      "name; alias the select items instead",
			                      model.Name().name);
		}
		if (ref.at_clause) {
			throw BinderException("AT clauses are not supported when querying semantic view \"%s\" by name",
			                      model.Name().name);
		}
		if (node.groups.grouping_sets.size() > 1) {
			throw BinderException("Multiple grouping sets are not supported when querying semantic view \"%s\" by "
			                      "name; use UNION ALL of separately grouped queries",
			                      model.Name().name);
		}
		for (auto &expr : node.select_list) {
			if (expr->GetExpressionClass() == ExpressionClass::STAR) {
				throw BinderException("SELECT * is not supported when querying semantic view \"%s\" by name: list "
				                      "dimensions and facts, and metrics with AGG(...)",
				                      model.Name().name);
			}
		}
	}

	//! Whether `expr` reads a window function metric (directly, through another metric or through a select alias)
	bool ReadsWindowMetric(const ParsedExpression &expr) {
		if (expr.GetExpressionClass() == ExpressionClass::COLUMN_REF) {
			auto &colref = expr.Cast<ColumnRefExpression>();
			if (!colref.IsQualified() && !HasInputName(colref.GetColumnName().GetIdentifierName())) {
				auto alias = OutputAlias(expr);
				if (alias.IsValid()) {
					return ReadsWindowMetric(*select[alias.GetIndex()].expr);
				}
			}
			auto ref = NameRef(expr);
			return ref.Is(SemanticRefKind::MEMBER) && MetricHasWindow(ref.index);
		}
		bool found = false;
		ParsedExpressionIterator::EnumerateChildren(
		    expr, [&](const ParsedExpression &child) { found = found || ReadsWindowMetric(child); });
		return found;
	}

	bool MetricHasWindow(idx_t member) const {
		auto &entry = model.members[member];
		if (entry.Kind() != SemanticMemberKind::METRIC) {
			return false;
		}
		std::function<bool(const ParsedExpression &)> visit = [&](const ParsedExpression &node) {
			if (node.GetExpressionClass() == ExpressionClass::WINDOW) {
				return true;
			}
			auto ref = SemanticMarkers::Classify(node);
			if (ref.Is(SemanticRefKind::MEMBER)) {
				return ref.index != member && MetricHasWindow(ref.index);
			}
			bool found = false;
			ParsedExpressionIterator::EnumerateChildren(
			    node, [&](const ParsedExpression &child) { found = found || visit(child); });
			return found;
		};
		return visit(*entry.resolved);
	}

	//! Whether the outer query evaluates window functions
	static bool HasOuterWindow(const SelectNode &node, const vector<unique_ptr<ParsedExpression> *> &orders) {
		for (auto &expr : node.select_list) {
			if (SemanticAnalyzer::ContainsWindow(*expr)) {
				return true;
			}
		}
		for (auto order : orders) {
			if (SemanticAnalyzer::ContainsWindow(**order)) {
				return true;
			}
		}
		return node.qualify && SemanticAnalyzer::ContainsWindow(*node.qualify);
	}

	//! USING SAMPLE samples the rows of the semantic query, which are the input rows only when it neither aggregates
	//! nor filters them
	void CheckSample(const SelectNode &node, bool aggregated) const {
		if (node.sample && (aggregated || query.where)) {
			throw BinderException("USING SAMPLE is only supported in row-level queries without WHERE when querying "
			                      "semantic view \"%s\" by name: it would sample the aggregated or filtered rows",
			                      model.Name().name);
		}
	}

	static vector<unique_ptr<ParsedExpression> *> ModifierExpressions(SelectNode &node) {
		vector<unique_ptr<ParsedExpression> *> result;
		for (auto &modifier : node.modifiers) {
			if (modifier->type == ResultModifierType::ORDER_MODIFIER) {
				for (auto &order : modifier->Cast<OrderModifier>().orders) {
					result.push_back(&order.expression);
				}
			} else if (modifier->type == ResultModifierType::DISTINCT_MODIFIER) {
				for (auto &target : modifier->Cast<DistinctModifier>().distinct_on_targets) {
					result.push_back(&target);
				}
			}
		}
		return result;
	}

	//! Normalized copies of the select list, WHERE (the semantic query's filter), HAVING and QUALIFY; ORDER BY in
	//! place
	void NormalizeClauses(const SelectNode &node, const vector<unique_ptr<ParsedExpression> *> &orders) {
		for (idx_t i = 0; i < node.select_list.size(); i++) {
			auto &expr = node.select_list[i];
			SelectItem item;
			// a select item reads the aliases of the items before it (lateral aliases)
			item.expr = SubstituteAliases(*Normalize(*expr), i, &node.select_list);
			item.expr->SetAlias(expr->GetAlias());
			item.alias = expr->GetAlias().GetIdentifierName();
			item.text = expr->ToString();
			if (expr->GetExpressionClass() == ExpressionClass::COLUMN_REF) {
				// as DuckDB names it: its last name, "a.name" when quoted as such
				item.column_name = expr->Cast<ColumnRefExpression>().GetColumnName().GetIdentifierName();
			}
			item.has_window = SemanticAnalyzer::ContainsWindow(*item.expr);
			item.has_aggregate = ContainsAggregate(*item.expr);
			item.correlated = HasOuterReferences(*item.expr);
			CheckWindowFunctions(*item.expr);
			select.push_back(std::move(item));
		}
		if (node.where_clause) {
			auto where = Normalize(*node.where_clause);
			CheckNoWindow(*where, "WHERE");
			CheckNoAggregate(*where, "WHERE", "; use HAVING");
			CheckNoOuterReferences(*where, "WHERE");
			query.where = SubstituteAliases(*where, select.size());
		}
		if (node.having) {
			having = Normalize(*node.having);
			CheckNoWindow(*having, "HAVING", "; use QUALIFY");
		}
		if (node.qualify) {
			qualify = Normalize(*node.qualify);
			CheckWindowFunctions(*qualify);
		}
		for (auto order : orders) {
			*order = Normalize(**order);
			CheckWindowFunctions(**order);
		}
	}

	//! GROUP BY expressions (ordinals and select aliases resolve to select items)
	void CollectGroups(const SelectNode &node) {
		if (node.aggregate_handling == AggregateHandling::FORCE_AGGREGATES) {
			// like DuckDB's GROUP BY ALL, neither aggregates nor windows are groups
			for (auto &item : select) {
				if (!item.has_window && !ContainsAggregate(*item.expr)) {
					groups.push_back(WithoutAlias(*item.expr));
					CheckNoOuterReferences(*groups.back(), "GROUP BY");
				}
			}
			return;
		}
		for (auto &group : node.groups.group_expressions) {
			groups.push_back(ResolveGroup(*group));
			CheckNoWindow(*groups.back(), "GROUP BY");
			CheckNoAggregate(*groups.back(), "GROUP BY");
			CheckNoOuterReferences(*groups.back(), "GROUP BY");
		}
	}

	//! Like in DuckDB, HAVING makes a query aggregated, without groups if it has no GROUP BY
	bool HasAggregation(const vector<unique_ptr<ParsedExpression> *> &orders) {
		if (!groups.empty() || having || (qualify && ContainsAggregate(*qualify))) {
			return true;
		}
		for (auto &item : select) {
			if (ContainsAggregate(*item.expr)) {
				return true;
			}
		}
		for (auto order : orders) {
			if (ContainsAggregate(**order)) {
				return true;
			}
		}
		return false;
	}

	//! Aggregated query: groups become dimensions, aggregate calls metrics; the select list reads their columns
	vector<unique_ptr<ParsedExpression>> AggregatedSelect() {
		for (idx_t g = 0; g < groups.size(); g++) {
			CheckNotMetric(*groups[g]);
			AddItem(SemanticMemberKind::DIMENSION, groups[g]->Copy(), GroupColumn(g));
		}
		vector<unique_ptr<ParsedExpression>> result;
		for (idx_t i = 0; i < select.size(); i++) {
			auto expr = Replace(*select[i].expr, false);
			expr->SetAlias(Identifier(OutputName(i)));
			result.push_back(std::move(expr));
		}
		return result;
	}

	//! Row-level query: every select item is a fact, except the items with a window function or a subquery reading the
	//! view, which are evaluated by the outer query over the fact columns
	vector<unique_ptr<ParsedExpression>> FactSelect() {
		for (idx_t i = 0; i < select.size(); i++) {
			if (!select[i].Outer()) {
				CheckNotMetric(*select[i].expr);
				AddItem(SemanticMemberKind::FACT, WithoutAlias(*select[i].expr), FactColumn(i));
			}
		}
		vector<unique_ptr<ParsedExpression>> result;
		for (idx_t i = 0; i < select.size(); i++) {
			result.push_back(SelectedFact(i));
			result.back()->SetAlias(Identifier(OutputName(i)));
		}
		return result;
	}

	//! The fact column of select item `index`, or its expression over fact columns
	unique_ptr<ParsedExpression> SelectedFact(idx_t index) {
		if (!select[index].Outer()) {
			return ColumnRef(FactColumn(index));
		}
		if (expanding_windows.count(index)) {
			throw BinderException("Window function alias \"%s\" cannot be referenced in its own definition when "
			                      "querying semantic view \"%s\" by name",
			                      select[index].alias, model.Name().name);
		}
		expanding_windows.insert(index);
		auto result = MapToFacts(*WithoutAlias(*select[index].expr));
		expanding_windows.erase(index);
		return result;
	}

	static unique_ptr<ParsedExpression> WithoutAlias(const ParsedExpression &expr) {
		auto result = expr.Copy();
		result->ClearAlias();
		return result;
	}

	bool IsViewQualifier(const string &qualifier) const {
		if (model.FindTable(qualifier).IsValid()) {
			return false;
		}
		return StringUtil::CIEquals(qualifier, (ref.alias.empty() ? ref.Table() : ref.alias).GetIdentifierName());
	}

	//! The number of leading names qualifying a column with the view: `sales`, `main.sales` or `db.main.sales` (the
	//! latter two only when the view is not aliased, as in DuckDB)
	idx_t ViewQualifiers(const vector<string> &names) const {
		if (names.size() >= 2 && IsViewQualifier(names[0])) {
			return 1;
		}
		if (!ref.alias.empty() || model.FindTable(names[0]).IsValid()) {
			return 0;
		}
		auto &name = model.Name();
		if (names.size() >= 3 && IsViewQualifier(names[1]) && StringUtil::CIEquals(names[0], name.schema)) {
			return 2;
		}
		if (names.size() >= 4 && IsViewQualifier(names[2]) && StringUtil::CIEquals(names[0], name.catalog) &&
		    StringUtil::CIEquals(names[1], name.schema)) {
			return 3;
		}
		return 0;
	}

	//! Strips qualifiers naming the semantic view itself and splits quoted "entity.name" identifiers
	unique_ptr<ParsedExpression> Normalize(const ParsedExpression &expr) {
		auto result = expr.Copy();
		std::function<void(unique_ptr<ParsedExpression> &)> visit = [&](unique_ptr<ParsedExpression> &node) {
			if (node->GetExpressionClass() == ExpressionClass::COLUMN_REF) {
				auto &colref = node->Cast<ColumnRefExpression>();
				auto names = IdentifiersToStrings(colref.ColumnNames());
				auto qualifiers = ViewQualifiers(names);
				auto view_qualified = qualifiers > 0;
				names.erase(names.begin(), names.begin() + NumericCast<int64_t>(qualifiers));
				if (names.size() == 1 && StringUtil::Contains(names[0], ".")) {
					auto dot = names[0].find('.');
					auto table = names[0].substr(0, dot);
					if (model.FindTable(table).IsValid()) {
						names = {table, names[0].substr(dot + 1)};
					}
				}
				if (view_qualified) {
					// Qualifying a name with the view excludes output aliases, including inside scalar expressions.
					string error;
					if (model.Lookup(names, SemanticResolveContext(), error).Is(SemanticRefKind::NONE)) {
						throw BinderException("%s", error);
					}
				}
				auto alias = node->GetAlias();
				node = make_uniq<ColumnRefExpression>(StringsToIdentifiers(names));
				node->SetAlias(alias);
				return;
			}
			ParsedExpressionIterator::EnumerateChildren(*node, visit);
		};
		visit(result);
		return result;
	}

	//! Windows are evaluated by the outer query, so not in clauses evaluated before them; `hint` follows the error
	void CheckNoWindow(const ParsedExpression &expr, const string &clause, const string &hint = string()) {
		if (SemanticAnalyzer::ContainsWindow(expr) || ReadsAlias(expr, &SelectItem::has_window)) {
			throw BinderException("Window functions are not allowed in %s when querying semantic view \"%s\" by name%s",
			                      clause, model.Name().name, hint);
		}
	}

	//! Metrics are computed by the semantic query, so not in the clauses it evaluates before them
	void CheckNoAggregate(const ParsedExpression &expr, const string &clause, const string &hint = string()) {
		if (ContainsAggregate(expr) || ReadsAlias(expr, &SelectItem::has_aggregate)) {
			throw BinderException("Aggregates are not allowed in %s when querying semantic view \"%s\" by name%s",
			                      clause, model.Name().name, hint);
		}
	}

	//! The semantic query evaluates WHERE and GROUP BY, where the view's columns are not visible to subqueries
	void CheckNoOuterReferences(const ParsedExpression &expr, const string &clause) {
		if (HasOuterReferences(expr) || ReadsAlias(expr, &SelectItem::correlated)) {
			throw BinderException("Subqueries in %s cannot reference semantic view \"%s\" when querying it by name",
			                      clause, model.Name().name);
		}
	}

	//! Whether `expr` reads the select alias of an item with `flag` set
	bool ReadsAlias(const ParsedExpression &expr, bool SelectItem::*flag) {
		if (lambdas.IsParameter(expr)) {
			return false;
		}
		LambdaScopes::Guard lambda(lambdas, expr);
		auto alias = OutputAlias(expr);
		if (alias.IsValid() && select[alias.GetIndex()].*flag &&
		    !HasInputName(expr.Cast<ColumnRefExpression>().GetColumnName().GetIdentifierName())) {
			return true;
		}
		bool found = false;
		ParsedExpressionIterator::EnumerateChildren(
		    expr, [&](const ParsedExpression &child) { found = found || ReadsAlias(child, flag); });
		return found;
	}

	//! `expr` with the select aliases of the first `count` items replaced by their expressions; input names take
	//! precedence, as in DuckDB. With `select_list`, a reference to the alias of a later item is an error.
	unique_ptr<ParsedExpression> SubstituteAliases(const ParsedExpression &expr, idx_t count,
	                                               const vector<unique_ptr<ParsedExpression>> *select_list = nullptr) {
		auto result = expr.Copy();
		std::function<void(unique_ptr<ParsedExpression> &)> visit = [&](unique_ptr<ParsedExpression> &node) {
			if (lambdas.IsParameter(*node)) {
				return;
			}
			LambdaScopes::Guard lambda(lambdas, *node);
			if (SemanticMarkers::IsExcluding(*node)) {
				// its arguments name groups, resolving aliases themselves (ExpandExcluding)
				return;
			}
			if (node->GetExpressionClass() != ExpressionClass::COLUMN_REF) {
				ParsedExpressionIterator::EnumerateChildren(*node, visit);
				return;
			}
			auto &colref = node->Cast<ColumnRefExpression>();
			auto name = colref.GetColumnName().GetIdentifierName();
			if (colref.IsQualified() || HasInputName(name)) {
				return;
			}
			auto alias = OutputAlias(*node, count);
			if (alias.IsValid()) {
				auto node_alias = node->GetAlias();
				node = WithoutAlias(*select[alias.GetIndex()].expr);
				node->SetAlias(node_alias);
				return;
			}
			for (idx_t i = count + 1; select_list && i < select_list->size(); i++) {
				if (StringUtil::CIEquals((*select_list)[i]->GetAlias().GetIdentifierName(), name)) {
					throw BinderException("Column \"%s\" referenced that exists in the SELECT clause - but this column "
					                      "cannot be referenced before it is defined",
					                      name);
				}
			}
		};
		visit(result);
		return result;
	}

	//! Calls `callback` on the columns of the semantic view (qualified by its name or alias) read by the subqueries
	//! in `expr`
	void VisitOuterReferences(unique_ptr<ParsedExpression> &expr,
	                          const std::function<void(unique_ptr<ParsedExpression> &)> &callback,
	                          bool in_subquery = false) {
		if (in_subquery && expr->GetExpressionClass() == ExpressionClass::COLUMN_REF) {
			auto &colref = expr->Cast<ColumnRefExpression>();
			if (colref.ColumnNames().size() >= 2 && IsViewQualifier(colref.ColumnNames()[0].GetIdentifierName())) {
				callback(expr);
			}
			return;
		}
		if (expr->GetExpressionClass() == ExpressionClass::SUBQUERY) {
			ParsedExpressionIterator::EnumerateQueryNodeChildren(
			    *expr->Cast<SubqueryExpression>().SubqueryMutable()->node,
			    [&](unique_ptr<ParsedExpression> &child) { VisitOuterReferences(child, callback, true); });
		}
		ParsedExpressionIterator::EnumerateChildren(
		    *expr, [&](unique_ptr<ParsedExpression> &child) { VisitOuterReferences(child, callback, in_subquery); });
	}

	bool HasOuterReferences(const ParsedExpression &expr) {
		auto copy = expr.Copy();
		bool found = false;
		VisitOuterReferences(copy, [&](unique_ptr<ParsedExpression> &) { found = true; });
		return found;
	}

	//! Points the subqueries of `expr` reading view columns to the columns `map` gives them in the outer query
	void MapOuterReferences(unique_ptr<ParsedExpression> &expr,
	                        const std::function<unique_ptr<ParsedExpression>(const ParsedExpression &)> &map) {
		VisitOuterReferences(expr, [&](unique_ptr<ParsedExpression> &column) {
			auto mapped = map(*Normalize(*column));
			D_ASSERT(mapped->GetExpressionClass() == ExpressionClass::COLUMN_REF);
			column = ColumnRef(mapped->Cast<ColumnRefExpression>().GetColumnName().GetIdentifierName(), OUTER_ALIAS);
		});
	}

	//! AGG(...) and time_shift / cumulative calls are metric items, not window functions: windows apply to them
	void CheckWindowFunctions(const ParsedExpression &expr) {
		auto form = SemanticQueryFormOf(expr);
		if (expr.GetExpressionClass() == ExpressionClass::WINDOW && form != SemanticQueryForm::NONE) {
			auto name = form == SemanticQueryForm::AGG
			                ? string("AGG")
			                : expr.Cast<WindowExpression>().FunctionName().GetIdentifierName();
			throw BinderException("%s() cannot be used as a window function when querying semantic view \"%s\" "
			                      "by name; apply the window to it instead, as in SUM(%s(...)) OVER (...)",
			                      name, model.Name().name, name);
		}
		ParsedExpressionIterator::EnumerateChildren(
		    expr, [&](const ParsedExpression &child) { CheckWindowFunctions(child); });
	}

	//! Replaces the PARTITION BY EXCLUDING keys of `window` by the expressions of the other groups
	void ExpandExcluding(WindowExpression &window) {
		vector<unique_ptr<ParsedExpression>> partitions;
		for (auto &partition : window.PartitionsMutable()) {
			if (!SemanticMarkers::IsExcluding(*partition)) {
				partitions.push_back(std::move(partition));
				continue;
			}
			// the excluded groups (an argument names a group as GROUP BY does, in any spelling of its expression)
			set<idx_t> excluded;
			for (auto &argument : partition->Cast<FunctionExpression>().GetArguments()) {
				auto group = FindGroup(*ResolveAlias(argument.GetExpression()));
				if (!group.IsValid()) {
					throw BinderException(
					    "PARTITION BY EXCLUDING only accepts grouped dimensions when querying semantic "
					    "view \"%s\" by name; \"%s\" is not in the GROUP BY clause",
					    model.Name().name, argument.GetExpression().ToString());
				}
				excluded.insert(group.GetIndex());
			}
			for (idx_t g = 0; g < groups.size(); g++) {
				if (!excluded.count(g)) {
					partitions.push_back(groups[g]->Copy());
				}
			}
		}
		window.PartitionsMutable() = std::move(partitions);
	}

	//! The group `expr` is: written the same way, or naming the same dimension or column (`segment` and
	//! `account.segment`)
	optional_idx FindGroup(const ParsedExpression &expr) const {
		for (idx_t g = 0; g < groups.size(); g++) {
			if (groups[g]->Equals(expr)) {
				return g;
			}
		}
		auto ref = NameRef(expr);
		if (ref.Is(SemanticRefKind::NONE)) {
			return optional_idx();
		}
		for (idx_t g = 0; g < groups.size(); g++) {
			auto group = NameRef(*groups[g]);
			if (group.kind == ref.kind && group.index == ref.index && StringUtil::CIEquals(group.column, ref.column)) {
				return g;
			}
		}
		return optional_idx();
	}

	//! What a name stands for (NONE for an unknown name or another expression)
	SemanticRef NameRef(const ParsedExpression &expr) const {
		if (expr.GetExpressionClass() != ExpressionClass::COLUMN_REF) {
			return SemanticRef();
		}
		SemanticResolveContext resolve_context;
		resolve_context.allow_private = false;
		string ignored;
		return model.Lookup(IdentifiersToStrings(expr.Cast<ColumnRefExpression>().ColumnNames()), resolve_context,
		                    ignored);
	}

	//! AGG(...), a time_shift / cumulative call, or an aggregate function: the boundary of one metric item
	bool IsAggregateCall(const ParsedExpression &expr) {
		if (expr.GetExpressionClass() != ExpressionClass::FUNCTION) {
			return false;
		}
		return SemanticQueryFormOf(expr) != SemanticQueryForm::NONE || analyzer.IsAggregate(expr);
	}

	bool ContainsAggregate(const ParsedExpression &expr) {
		if (IsAggregateCall(expr)) {
			return true;
		}
		bool found = false;
		ParsedExpressionIterator::EnumerateChildren(
		    expr, [&](const ParsedExpression &child) { found = found || ContainsAggregate(child); });
		return found;
	}

	unique_ptr<ParsedExpression> ResolveGroup(const ParsedExpression &group) {
		auto normalized = Normalize(group);
		if (normalized->GetExpressionClass() == ExpressionClass::CONSTANT) {
			auto value = ConstantValue(*normalized);
			if (value.type().IsIntegral()) {
				auto index = value.GetValue<int64_t>();
				if (index < 1 || index > int64_t(select.size())) {
					throw BinderException("GROUP BY term out of range - "
					                      "should be between 1 and %d",
					                      select.size());
				}
				return WithoutAlias(*select[NumericCast<idx_t>(index - 1)].expr);
			}
		}
		return ResolveAlias(*normalized);
	}

	//! A (normalized) select alias as its select item's expression; other expressions as they are
	unique_ptr<ParsedExpression> ResolveAlias(const ParsedExpression &expr) const {
		if (expr.GetExpressionClass() == ExpressionClass::COLUMN_REF) {
			auto &colref = expr.Cast<ColumnRefExpression>();
			if (!colref.IsQualified() && !HasInputName(colref.GetColumnName().GetIdentifierName())) {
				auto alias = OutputAlias(expr);
				if (alias.IsValid()) {
					return WithoutAlias(*select[alias.GetIndex()].expr);
				}
			}
		}
		return expr.Copy();
	}

	//! Whether `name` is a column of the view (a member): it takes precedence over select aliases. Physical columns
	//! of the logical tables are not columns of the view.
	bool HasInputName(const string &name) const {
		return !model.FindMembersByName(name).empty();
	}

	//! The (last) select item among the first `count` that `expr` names by its alias
	optional_idx OutputAlias(const ParsedExpression &expr, optional_idx count = optional_idx()) const {
		if (expr.GetExpressionClass() != ExpressionClass::COLUMN_REF) {
			return optional_idx();
		}
		auto &colref = expr.Cast<ColumnRefExpression>();
		if (!colref.IsQualified()) {
			for (idx_t i = count.IsValid() ? count.GetIndex() : select.size(); i > 0; i--) {
				auto &alias = select[i - 1].alias;
				if (!alias.empty() && StringUtil::CIEquals(alias, colref.GetColumnName().GetIdentifierName())) {
					return i - 1;
				}
			}
		}
		return optional_idx();
	}

	//! Rejects bare references to metrics (they must be wrapped in AGG)
	void CheckNotMetric(const ParsedExpression &expr) {
		if (lambdas.IsParameter(expr)) {
			return;
		}
		LambdaScopes::Guard lambda(lambdas, expr);
		if (expr.GetExpressionClass() == ExpressionClass::COLUMN_REF) {
			string ignored;
			auto ref = model.Lookup(IdentifiersToStrings(expr.Cast<ColumnRefExpression>().ColumnNames()),
			                        SemanticResolveContext(), ignored);
			if (ref.Is(SemanticRefKind::MEMBER) && model.members[ref.index].Kind() == SemanticMemberKind::METRIC) {
				throw BinderException("Metric \"%s\" must be used with AGG()", expr.ToString());
			}
		}
		ParsedExpressionIterator::EnumerateChildren(expr,
		                                            [&](const ParsedExpression &child) { CheckNotMetric(child); });
	}

	void AddItem(SemanticMemberKind kind, unique_ptr<ParsedExpression> expr, const string &column) {
		ParsedSemanticQueryItem item;
		item.kind = kind;
		item.expression = std::move(expr);
		item.alias = column;
		// errors name the item as written, not by its internal alias
		item.display_name = DisplayText(*item.expression);
		query.items.push_back(std::move(item));
	}

	//! Rejects bare metric references outside AGG(...) and aggregate calls, in the metric of a time_shift / cumulative
	//! call too (its other arguments are an interval and named options)
	void CheckMetricsWrapped(const ParsedExpression &expr) {
		LambdaScopes::Guard lambda(lambdas, expr);
		if (expr.GetExpressionClass() == ExpressionClass::COLUMN_REF) {
			CheckNotMetric(expr);
			return;
		}
		auto form = SemanticQueryFormOf(expr);
		if (expr.GetExpressionClass() == ExpressionClass::FUNCTION &&
		    (form == SemanticQueryForm::AGG || analyzer.IsAggregate(expr))) {
			return;
		}
		if (expr.GetExpressionClass() == ExpressionClass::FUNCTION && form != SemanticQueryForm::NONE) {
			auto metric = SemanticTransformMetricArgument(expr.Cast<FunctionExpression>());
			if (metric) {
				CheckMetricsWrapped(*metric);
			}
			return;
		}
		ParsedExpressionIterator::EnumerateChildren(expr,
		                                            [&](const ParsedExpression &child) { CheckMetricsWrapped(child); });
	}

	//! Registers the metric item of an AGG(...), time_shift / cumulative or aggregate call (deduplicated) and returns
	//! its column. The call is kept as written: the semantic query resolves and validates it (semantic_transforms.hpp).
	string MetricFor(const ParsedExpression &call) {
		CheckMetricsWrapped(call);
		auto metric = WithoutAlias(call);
		for (idx_t m = 0; m < metrics.size(); m++) {
			if (metrics[m]->Equals(*metric)) {
				return MetricColumn(m);
			}
		}
		auto column = MetricColumn(metrics.size());
		AddItem(SemanticMemberKind::METRIC, metric->Copy(), column);
		metrics.push_back(std::move(metric));
		return column;
	}

	unique_ptr<ParsedExpression> Replace(const ParsedExpression &expr, bool allow_aliases,
	                                     bool top_level_alias = false) {
		if (lambdas.IsParameter(expr)) {
			return expr.Copy();
		}
		LambdaScopes::Guard lambda(lambdas, expr);
		auto alias = top_level_alias ? OutputAlias(expr) : optional_idx();
		if (alias.IsValid()) {
			return Replace(*select[alias.GetIndex()].expr, false);
		}
		if (expr.GetExpressionType() == ExpressionType::GROUPING_FUNCTION) {
			return Grouping(expr.Cast<OperatorExpression>());
		}
		auto group = FindGroup(expr);
		if (group.IsValid()) {
			return make_uniq<ColumnRefExpression>(Identifier(GroupColumn(group.GetIndex())));
		}
		if (IsAggregateCall(expr)) {
			return make_uniq<ColumnRefExpression>(Identifier(MetricFor(expr)));
		}
		if (expr.GetExpressionClass() == ExpressionClass::COLUMN_REF) {
			auto &colref = expr.Cast<ColumnRefExpression>();
			if (allow_aliases && !colref.IsQualified() && !HasInputName(colref.GetColumnName().GetIdentifierName())) {
				if (OutputAlias(expr).IsValid()) {
					return expr.Copy();
				}
			}
			CheckNotMetric(expr);
			throw BinderException(
			    "column \"%s\" must appear in the GROUP BY clause or be used in an aggregate function",
			    expr.ToString());
		}
		auto result = expr.Copy();
		if (result->GetExpressionClass() == ExpressionClass::WINDOW) {
			// the group expressions it partitions by are replaced by their columns below
			ExpandExcluding(result->Cast<WindowExpression>());
		}
		if (result->GetExpressionClass() == ExpressionClass::SUBQUERY) {
			MapOuterReferences(result, [&](const ParsedExpression &column) { return Replace(column, false); });
		}
		ParsedExpressionIterator::EnumerateChildren(
		    *result, [&](unique_ptr<ParsedExpression> &child) { child = Replace(*child, allow_aliases); });
		return result;
	}

	//! GROUPING(...) of grouped dimensions: 0, as the query has a single grouping set
	unique_ptr<ParsedExpression> Grouping(const OperatorExpression &grouping) {
		for (auto &argument : grouping.GetChildren()) {
			if (!FindGroup(*ResolveAlias(*argument)).IsValid()) {
				throw BinderException("GROUPING() only accepts grouped dimensions when querying semantic view \"%s\" "
				                      "by name; \"%s\" is not in the GROUP BY clause",
				                      model.Name().name, argument->ToString());
			}
		}
		return make_uniq<CastExpression>(LogicalType::BIGINT, make_uniq<ConstantExpression>(Literal::Integer(0)));
	}

	unique_ptr<ParsedExpression> MapToFacts(const ParsedExpression &expr, bool top_level_alias = false) {
		if (lambdas.IsParameter(expr)) {
			return expr.Copy();
		}
		LambdaScopes::Guard lambda(lambdas, expr);
		auto alias = top_level_alias ? OutputAlias(expr) : optional_idx();
		if (alias.IsValid()) {
			return SelectedFact(alias.GetIndex());
		}
		for (idx_t i = 0; i < select.size(); i++) {
			if (!select[i].Outer() && select[i].expr->Equals(expr)) {
				return make_uniq<ColumnRefExpression>(Identifier(FactColumn(i)));
			}
		}
		if (expr.GetExpressionClass() == ExpressionClass::COLUMN_REF) {
			auto &colref = expr.Cast<ColumnRefExpression>();
			if (!HasInputName(colref.GetColumnName().GetIdentifierName())) {
				auto output = OutputAlias(expr);
				if (output.IsValid()) {
					return SelectedFact(output.GetIndex());
				}
			}
			CheckNotMetric(expr);
			for (auto &item : query.items) {
				if (item.expression->Equals(expr)) {
					return ColumnRef(item.alias);
				}
			}
			// A sort, DISTINCT ON or window key can reference a fact that is not selected. Keep it in the
			// semantic query's projection so the outer query can read it (numbered after the select items, which
			// window items leave gaps in).
			auto column = FactColumn(select.size() + query.items.size());
			AddItem(SemanticMemberKind::FACT, WithoutAlias(expr), column);
			return ColumnRef(column);
		}
		auto result = expr.Copy();
		if (result->GetExpressionClass() == ExpressionClass::WINDOW) {
			// no groups to partition by: rejects any PARTITION BY EXCLUDING
			ExpandExcluding(result->Cast<WindowExpression>());
		}
		if (result->GetExpressionClass() == ExpressionClass::SUBQUERY) {
			MapOuterReferences(result, [&](const ParsedExpression &column) { return MapToFacts(column); });
		}
		ParsedExpressionIterator::EnumerateChildren(
		    *result, [&](unique_ptr<ParsedExpression> &child) { child = MapToFacts(*child); });
		return result;
	}

	string OutputName(idx_t index) {
		auto &item = select[index];
		if (!item.alias.empty()) {
			return item.alias;
		}
		if (!item.column_name.empty()) {
			return item.column_name;
		}
		if (item.expr->GetExpressionClass() == ExpressionClass::COLUMN_REF) {
			return item.expr->Cast<ColumnRefExpression>().GetColumnName().GetIdentifierName();
		}
		return item.text;
	}

	const SemanticModel &model;
	SemanticAnalyzer analyzer;
	const BaseTableRef &ref;

	vector<SelectItem> select;
	//! The lambda parameters in scope in the expression being walked
	LambdaScopes lambdas;
	//! The window items being mapped to fact columns, to reject an alias referenced in its own definition
	set<idx_t> expanding_windows;
	unique_ptr<ParsedExpression> having;
	unique_ptr<ParsedExpression> qualify;
	vector<unique_ptr<ParsedExpression>> groups;
	vector<unique_ptr<ParsedExpression>> metrics;
	ParsedSemanticQuery query;
};

//! SEMANTIC_VIEW(...) as written (the parser lowers it to a table function with an encoded argument)
string SemanticQueryText(const ParsedSemanticQuery &query) {
	auto &view = query.view;
	string result = "SEMANTIC_VIEW(";
	for (auto *part : {&view.catalog, &view.schema}) {
		if (!part->empty()) {
			result += SQLIdentifier::ToString(*part) + ".";
		}
	}
	result += SQLIdentifier::ToString(view.name);
	for (auto kind : {SemanticMemberKind::DIMENSION, SemanticMemberKind::METRIC, SemanticMemberKind::FACT}) {
		string clause;
		for (auto &item : query.items) {
			if (item.kind != kind) {
				continue;
			}
			clause += clause.empty() ? " " + SemanticMemberKindToString(kind) + "S " : ", ";
			clause += item.expression ? item.expression->ToString() : item.star_table + ".*";
			if (!item.alias.empty()) {
				clause += " AS " + SQLIdentifier::ToString(item.alias);
			}
		}
		result += clause;
	}
	if (query.where) {
		result += " WHERE " + query.where->ToString();
	}
	return result + ")";
}

//! The SEMANTIC_VIEW(...) calls of an expression's subqueries, as lowered and as written
class SemanticQueryTextCollector : public SemanticStatementVisitor {
public:
	vector<pair<string, string>> replacements;

	void Collect(unique_ptr<ParsedExpression> &expr) {
		VisitExpression(expr);
	}

protected:
	void VisitTableRef(unique_ptr<TableRef> &ref, bool) override {
		if (ref->type != TableReferenceType::TABLE_FUNCTION) {
			return;
		}
		auto &function = ref->Cast<TableFunctionRef>().function->Cast<FunctionExpression>();
		if (!StringUtil::CIEquals(function.FunctionName().GetIdentifierName(), SEMANTIC_VIEW_QUERY_FUNCTION) ||
		    function.GetArguments().size() != 1 ||
		    function.GetArguments()[0].GetExpression().GetExpressionClass() != ExpressionClass::CONSTANT) {
			return;
		}
		auto query =
		    SemanticLowering::Decode<ParsedSemanticQuery>(ConstantValue(function.GetArguments()[0].GetExpression()));
		replacements.emplace_back(function.ToString(), SemanticQueryText(*query));
	}
};

bool ContainsSubquery(const ParsedExpression &expr) {
	if (expr.GetExpressionClass() == ExpressionClass::SUBQUERY) {
		return true;
	}
	bool found = false;
	ParsedExpressionIterator::EnumerateChildren(
	    expr, [&](const ParsedExpression &child) { found = found || ContainsSubquery(child); });
	return found;
}

//! Rewrites the query nodes that read from a semantic view by name
class SemanticReferenceRewriter : public SemanticStatementVisitor {
public:
	SemanticReferenceRewriter(ClientContext &context, SemanticViewsState &state) : context(context), state(state) {
	}

	bool rewritten = false;

protected:
	void VisitTableRef(unique_ptr<TableRef> &ref, bool in_join) override {
		if (ref->type == TableReferenceType::TABLE_FUNCTION) {
			VisitSemanticQuery(ref->Cast<TableFunctionRef>());
		}
		if (in_join && ref->type == TableReferenceType::BASE_TABLE && SemanticViewFor(ref->Cast<BaseTableRef>())) {
			throw BinderException("Semantic view \"%s\" cannot be joined in the FROM "
			                      "clause; use SEMANTIC_VIEW(...) "
			                      "or a subquery instead",
			                      ref->Cast<BaseTableRef>().Table().GetIdentifierName());
		}
	}

	//! DESCRIBE <semantic view>: the columns of its catalog placeholder
	bool VisitShowQuery(ShowRef &show) override {
		if (show.show_type != ShowType::DESCRIBE || show.query->type != QueryNodeType::SELECT_NODE) {
			return false;
		}
		auto &select = show.query->Cast<SelectNode>();
		if (select.select_list.size() != 1 || select.select_list[0]->GetExpressionClass() != ExpressionClass::STAR ||
		    !select.from_table || select.from_table->type != TableReferenceType::BASE_TABLE) {
			return false;
		}
		auto model = SemanticViewFor(select.from_table->Cast<BaseTableRef>());
		if (!model) {
			return false;
		}
		show.query = SemanticDescribeNode(*model);
		rewritten = true;
		return true;
	}

	//! Before its subqueries are rewritten, names an unaliased select item reading a subquery as written, as DuckDB
	//! would
	void EnterSelect(SelectNode &select) override {
		for (auto &expr : select.select_list) {
			if (!expr->GetAlias().empty() || expr->GetExpressionClass() == ExpressionClass::COLUMN_REF ||
			    !ContainsSubquery(*expr)) {
				continue;
			}
			auto name = expr->ToString();
			auto copy = expr->Copy();
			SemanticQueryTextCollector collector;
			collector.Collect(copy);
			for (auto &replacement : collector.replacements) {
				name = StringUtil::Replace(name, replacement.first, replacement.second);
			}
			expr->SetAlias(Identifier(name));
		}
	}

	void VisitSelect(SelectNode &select) override {
		if (!select.from_table || select.from_table->type != TableReferenceType::BASE_TABLE) {
			return;
		}
		auto &base = select.from_table->Cast<BaseTableRef>();
		auto model = SemanticViewFor(base);
		if (model) {
			StandardFormRewriter(context, *model, base).Rewrite(select);
			rewritten = true;
		}
	}

private:
	//! SEMANTIC_VIEW(...), lowered by the parser: rewrites the queries by name in its expressions (subqueries of its
	//! WHERE clause)
	void VisitSemanticQuery(TableFunctionRef &function_ref) {
		auto &function = function_ref.function->Cast<FunctionExpression>();
		if (!StringUtil::CIEquals(function.FunctionName().GetIdentifierName(), SEMANTIC_VIEW_QUERY_FUNCTION) ||
		    function.GetArguments().size() != 1 ||
		    function.GetArguments()[0].GetExpression().GetExpressionClass() != ExpressionClass::CONSTANT) {
			return;
		}
		auto &blob = function.GetArgumentsMutable()[0].GetExpressionMutable();
		auto query = SemanticLowering::Decode<ParsedSemanticQuery>(ConstantValue(*blob));
		auto was_rewritten = rewritten;
		rewritten = false;
		if (query->where) {
			VisitExpression(query->where);
		}
		for (auto &item : query->items) {
			if (item.expression) {
				VisitExpression(item.expression);
			}
		}
		if (rewritten) {
			blob = ConstantExpression::FromValue(SemanticLowering::Encode(*query));
		}
		rewritten = rewritten || was_rewritten;
	}

	shared_ptr<SemanticModel> SemanticViewFor(const BaseTableRef &ref) {
		auto &qualified_name = ref.GetQualifiedName();
		if (qualified_name.Path().size() > 3) {
			// a table in a nested schema: semantic views live in catalog.schema
			return nullptr;
		}
		bool unqualified = qualified_name.Path().size() == 1;
		if (unqualified && IsCTEName(ref.Table().GetIdentifierName())) {
			return nullptr;
		}
		auto name = SemanticQualifiedName::FromQualifiedName(qualified_name);
		auto model = state.registry.Lookup(context, name);
		if (!model) {
			return nullptr;
		}
		// a real table or view of that name (another catalog's, or a CTE) takes precedence over the semantic view; its
		// own catalog entry is a placeholder
		auto entry = Catalog::GetEntry(context, EntryLookupInfo(CatalogType::TABLE_ENTRY, qualified_name),
		                               OnEntryNotFound::RETURN_NULL);
		if (entry && !IsSemanticPlaceholder(*entry)) {
			return nullptr;
		}
		return model;
	}

	ClientContext &context;
	SemanticViewsState &state;
};

} // namespace

bool RewriteSemanticViewReferences(ClientContext &context, SemanticViewsState &state, SQLStatement &statement) {
	SemanticReferenceRewriter rewriter(context, state);
	rewriter.VisitStatement(statement);
	return rewriter.rewritten;
}

} // namespace duckdb
