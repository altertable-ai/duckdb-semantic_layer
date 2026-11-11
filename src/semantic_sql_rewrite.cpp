#include "semantic_sql_rewrite.hpp"

#include "duckdb/catalog/catalog.hpp"
#include "duckdb/catalog/entry_lookup_info.hpp"
#include "duckdb/common/exception/binder_exception.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/common/to_string.hpp"
#include "duckdb/parser/expression/columnref_expression.hpp"
#include "duckdb/parser/expression/constant_expression.hpp"
#include "duckdb/parser/expression/function_expression.hpp"
#include "duckdb/parser/expression/window_expression.hpp"
#include "duckdb/parser/parsed_expression_iterator.hpp"
#include "duckdb/parser/query_node/select_node.hpp"
#include "duckdb/parser/result_modifier.hpp"
#include "duckdb/parser/tableref/basetableref.hpp"
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
		auto orders = ModifierExpressions(node);
		vector<bool> modifier_aliases;
		for (auto order : orders) {
			modifier_aliases.push_back((*order)->GetExpressionClass() == ExpressionClass::COLUMN_REF &&
			                           !(*order)->Cast<ColumnRefExpression>().IsQualified());
		}
		NormalizeClauses(node, orders);
		CollectGroups(node);
		auto aggregated = HasAggregation(orders);
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
		node.having = nullptr;
		node.groups = GroupByNode();
		node.aggregate_handling = AggregateHandling::STANDARD_HANDLING;
		node.from_table = SemanticLowering::Query(query);
		node.from_table->alias = "__sv_std";
	}

private:
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
		for (auto &expr : node.select_list) {
			SelectItem item;
			item.expr = Normalize(*expr);
			item.expr->SetAlias(expr->GetAlias());
			item.alias = expr->GetAlias().GetIdentifierName();
			item.text = expr->ToString();
			item.has_window = SemanticAnalyzer::ContainsWindow(*item.expr);
			CheckWindowFunctions(*item.expr);
			select.push_back(std::move(item));
		}
		if (node.where_clause) {
			query.where = Normalize(*node.where_clause);
			CheckNoWindow(*query.where, "WHERE");
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
				}
			}
			return;
		}
		for (auto &group : node.groups.group_expressions) {
			groups.push_back(ResolveGroup(*group));
			CheckNoWindow(*groups.back(), "GROUP BY");
		}
	}

	bool HasAggregation(const vector<unique_ptr<ParsedExpression> *> &orders) {
		if (!groups.empty() || (having && ContainsAggregate(*having)) || (qualify && ContainsAggregate(*qualify))) {
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

	//! Row-level query: every select item is a fact, except the items with a window function, which are evaluated by
	//! the outer query over the fact columns
	vector<unique_ptr<ParsedExpression>> FactSelect() {
		for (idx_t i = 0; i < select.size(); i++) {
			if (!select[i].has_window) {
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

	//! The fact column of select item `index`, or its window expression over fact columns
	unique_ptr<ParsedExpression> SelectedFact(idx_t index) {
		if (!select[index].has_window) {
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

	//! Strips qualifiers naming the semantic view itself and splits quoted "entity.name" identifiers
	unique_ptr<ParsedExpression> Normalize(const ParsedExpression &expr) {
		auto result = expr.Copy();
		std::function<void(unique_ptr<ParsedExpression> &)> visit = [&](unique_ptr<ParsedExpression> &node) {
			if (node->GetExpressionClass() == ExpressionClass::COLUMN_REF) {
				auto &colref = node->Cast<ColumnRefExpression>();
				auto names = IdentifiersToStrings(colref.ColumnNames());
				auto view_qualified = names.size() >= 2 && IsViewQualifier(names[0]);
				if (view_qualified) {
					names.erase(names.begin());
				}
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
		if (SemanticAnalyzer::ContainsWindow(expr)) {
			throw BinderException("Window functions are not allowed in %s when querying semantic view \"%s\" by name%s",
			                      clause, model.Name().name, hint);
		}
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

	bool HasInputName(const string &name) const {
		if (!model.FindMembersByName(name).empty()) {
			return true;
		}
		for (auto &table : model.tables) {
			if (table.columns.count(name)) {
				return true;
			}
		}
		return false;
	}

	optional_idx OutputAlias(const ParsedExpression &expr) const {
		if (expr.GetExpressionClass() != ExpressionClass::COLUMN_REF) {
			return optional_idx();
		}
		auto &colref = expr.Cast<ColumnRefExpression>();
		if (!colref.IsQualified()) {
			for (idx_t i = select.size(); i > 0; i--) {
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
		item.display_name = item.expression->ToString();
		query.items.push_back(std::move(item));
	}

	//! Rejects bare metric references outside AGG(...) and aggregate calls, in the metric of a time_shift / cumulative
	//! call too (its other arguments are an interval and named options)
	void CheckMetricsWrapped(const ParsedExpression &expr) {
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
		auto alias = top_level_alias ? OutputAlias(expr) : optional_idx();
		if (alias.IsValid()) {
			return Replace(*select[alias.GetIndex()].expr, false);
		}
		for (idx_t g = 0; g < groups.size(); g++) {
			if (groups[g]->Equals(expr)) {
				return make_uniq<ColumnRefExpression>(Identifier(GroupColumn(g)));
			}
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
		if (expr.GetExpressionClass() == ExpressionClass::SUBQUERY) {
			return expr.Copy();
		}
		auto result = expr.Copy();
		if (result->GetExpressionClass() == ExpressionClass::WINDOW) {
			// the group expressions it partitions by are replaced by their columns below
			ExpandExcluding(result->Cast<WindowExpression>());
		}
		ParsedExpressionIterator::EnumerateChildren(
		    *result, [&](unique_ptr<ParsedExpression> &child) { child = Replace(*child, allow_aliases); });
		return result;
	}

	unique_ptr<ParsedExpression> MapToFacts(const ParsedExpression &expr, bool top_level_alias = false) {
		auto alias = top_level_alias ? OutputAlias(expr) : optional_idx();
		if (alias.IsValid()) {
			return SelectedFact(alias.GetIndex());
		}
		for (idx_t i = 0; i < select.size(); i++) {
			if (!select[i].has_window && select[i].expr->Equals(expr)) {
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
		ParsedExpressionIterator::EnumerateChildren(
		    *result, [&](unique_ptr<ParsedExpression> &child) { child = MapToFacts(*child); });
		return result;
	}

	string OutputName(idx_t index) {
		auto &item = select[index];
		if (!item.alias.empty()) {
			return item.alias;
		}
		if (item.expr->GetExpressionClass() == ExpressionClass::COLUMN_REF) {
			return item.expr->Cast<ColumnRefExpression>().GetColumnName().GetIdentifierName();
		}
		return item.text;
	}

	const SemanticModel &model;
	SemanticAnalyzer analyzer;
	const BaseTableRef &ref;
	//! A select list item
	struct SelectItem {
		//! Normalized, with its alias
		unique_ptr<ParsedExpression> expr;
		string alias;
		//! As written
		string text;
		//! Whether it contains a window function (evaluated by the outer query)
		bool has_window = false;
	};

	vector<SelectItem> select;
	//! The window items being mapped to fact columns, to reject an alias referenced in its own definition
	set<idx_t> expanding_windows;
	unique_ptr<ParsedExpression> having;
	unique_ptr<ParsedExpression> qualify;
	vector<unique_ptr<ParsedExpression>> groups;
	vector<unique_ptr<ParsedExpression>> metrics;
	ParsedSemanticQuery query;
};

//! Rewrites the query nodes that read from a semantic view by name
class SemanticReferenceRewriter : public SemanticStatementVisitor {
public:
	SemanticReferenceRewriter(ClientContext &context, SemanticViewsState &state) : context(context), state(state) {
	}

	bool rewritten = false;

protected:
	void VisitTableRef(unique_ptr<TableRef> &ref, bool in_join) override {
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
