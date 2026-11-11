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
			select.push_back(Normalize(*expr));
			select.back()->SetAlias(expr->GetAlias());
			select_aliases.push_back(expr->GetAlias().GetIdentifierName());
			original_text.push_back(expr->ToString());
			window_items.push_back(SemanticAnalyzer::ContainsWindow(*select.back()));
			CheckWindowFunctions(*select.back());
		}
		if (node.where_clause) {
			query.where = Normalize(*node.where_clause);
			CheckNoWindow(*query.where, "WHERE");
		}
		if (node.having) {
			having = Normalize(*node.having);
			CheckNoWindow(*having, "HAVING");
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
			for (idx_t i = 0; i < select.size(); i++) {
				if (!window_items[i] && !ContainsAggregate(*select[i])) {
					groups.push_back(WithoutAlias(*select[i]));
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
		for (auto &expr : select) {
			if (ContainsAggregate(*expr)) {
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
			auto expr = Replace(*select[i], false);
			expr->SetAlias(Identifier(OutputName(i)));
			result.push_back(std::move(expr));
		}
		return result;
	}

	//! Row-level query: every select item is a fact, except the items with a window function, which are evaluated by
	//! the outer query over the fact columns
	vector<unique_ptr<ParsedExpression>> FactSelect() {
		for (idx_t i = 0; i < select.size(); i++) {
			if (!window_items[i]) {
				CheckNotMetric(*select[i]);
				AddItem(SemanticMemberKind::FACT, WithoutAlias(*select[i]), FactColumn(i));
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
		if (!window_items[index]) {
			return ColumnRef(FactColumn(index));
		}
		if (expanding_windows.count(index)) {
			throw BinderException("Window function alias \"%s\" cannot be referenced in its own definition when "
			                      "querying semantic view \"%s\" by name",
			                      select_aliases[index], model.Name().name);
		}
		expanding_windows.insert(index);
		auto result = MapToFacts(*WithoutAlias(*select[index]));
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

	//! Windows are evaluated by the outer query, so not in clauses evaluated before them
	void CheckNoWindow(const ParsedExpression &expr, const string &clause) {
		if (SemanticAnalyzer::ContainsWindow(expr)) {
			throw BinderException("Window functions are not allowed in %s when querying semantic view \"%s\" by name%s",
			                      clause, model.Name().name, clause == "HAVING" ? "; use QUALIFY" : "");
		}
	}

	//! AGG(...) and time_shift / cumulative calls are metric items, not window functions: windows apply to them
	void CheckWindowFunctions(const ParsedExpression &expr) {
		if (expr.GetExpressionClass() == ExpressionClass::WINDOW) {
			auto &window = expr.Cast<WindowExpression>();
			FunctionExpression call(window.GetQualifiedName(), vector<unique_ptr<ParsedExpression>>());
			if (IsAggCall(call) || IsTransformCall(call)) {
				auto name = IsAggCall(call) ? string("AGG") : window.FunctionName().GetIdentifierName();
				throw BinderException("%s() cannot be used as a window function when querying semantic view \"%s\" "
				                      "by name; apply the window to it instead, as in SUM(%s(...)) OVER (...)",
				                      name, model.Name().name, name);
			}
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
			auto &excluded = partition->Cast<FunctionExpression>().GetArguments();
			for (auto &argument : excluded) {
				if (!IsGroup(argument.GetExpression())) {
					throw BinderException(
					    "PARTITION BY EXCLUDING only accepts grouped dimensions when querying semantic "
					    "view \"%s\" by name; \"%s\" is not in the GROUP BY clause",
					    model.Name().name, argument.GetExpression().ToString());
				}
			}
			for (auto &group : groups) {
				bool is_excluded = false;
				for (auto &argument : excluded) {
					is_excluded = is_excluded || group->Equals(argument.GetExpression());
				}
				if (!is_excluded) {
					partitions.push_back(group->Copy());
				}
			}
		}
		window.PartitionsMutable() = std::move(partitions);
	}

	bool IsGroup(const ParsedExpression &expr) const {
		for (auto &group : groups) {
			if (group->Equals(expr)) {
				return true;
			}
		}
		return false;
	}

	static bool IsAggCall(const ParsedExpression &expr) {
		return expr.GetExpressionClass() == ExpressionClass::FUNCTION &&
		       StringUtil::CIEquals(expr.Cast<FunctionExpression>().FunctionName().GetIdentifierName(), "agg");
	}

	static bool IsTransformCall(const ParsedExpression &expr) {
		return SemanticFindTransformCall(expr).get() == &expr;
	}

	//! AGG(...), an aggregate function, or a time_shift / cumulative call: the boundary of one metric item
	bool IsAggregateCall(const ParsedExpression &expr) {
		if (expr.GetExpressionClass() != ExpressionClass::FUNCTION) {
			return false;
		}
		return IsAggCall(expr) || IsTransformCall(expr) || analyzer.IsAggregate(expr);
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
				return WithoutAlias(*select[NumericCast<idx_t>(index - 1)]);
			}
		}
		if (normalized->GetExpressionClass() == ExpressionClass::COLUMN_REF) {
			auto &colref = normalized->Cast<ColumnRefExpression>();
			if (!colref.IsQualified() && !HasInputName(colref.GetColumnName().GetIdentifierName())) {
				auto alias = OutputAlias(*normalized);
				if (alias.IsValid()) {
					return WithoutAlias(*select[alias.GetIndex()]);
				}
			}
		}
		return normalized;
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
			for (idx_t i = select_aliases.size(); i > 0; i--) {
				if (!select_aliases[i - 1].empty() &&
				    StringUtil::CIEquals(select_aliases[i - 1], colref.GetColumnName().GetIdentifierName())) {
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
		query.items.push_back(std::move(item));
	}

	//! The metric item of an AGG(<metric>) call: its metric, or the call itself with a FILTER, which the semantic query
	//! resolves like a metric defined with that FILTER
	unique_ptr<ParsedExpression> AggMetric(const FunctionExpression &function) {
		// a named argument (`name := value`) may be stored as the value's alias
		if (ArgumentCount(function) != 1 || function.GetArguments()[0].HasName() ||
		    !Argument(function, 0).GetAlias().empty() || function.Distinct() ||
		    (function.OrderBy() && !function.OrderBy()->orders.empty()) || function.ExportState() ||
		    SemanticFindTransformCall(Argument(function, 0))) {
			throw BinderException("AGG() takes exactly one metric");
		}
		if (SemanticAnalyzer::ContainsWindow(Argument(function, 0))) {
			throw BinderException("AGG() takes a metric, not a window function; apply the window to it instead, as in "
			                      "SUM(AGG(...)) OVER (...)");
		}
		auto metric = WithoutAlias(Argument(function, 0));
		SemanticResolveContext resolve_context;
		resolve_context.allow_private = false;
		resolve_context.what = "AGG(" + metric->ToString() + ")";
		auto resolved = model.Resolve(*metric, resolve_context);
		if (!analyzer.ContainsAggregateOrMetric(*resolved)) {
			throw BinderException("AGG() requires a metric; \"%s\" is not a metric", metric->ToString());
		}
		return function.Filter() ? WithoutAlias(function) : std::move(metric);
	}

	//! Replaces the AGG(<metric>) calls in `expr` by their metric item; ad-hoc aggregates are kept, bare metrics
	//! rejected
	void UnwrapAggCalls(unique_ptr<ParsedExpression> &expr) {
		if (IsAggCall(*expr)) {
			auto alias = expr->GetAlias();
			expr = AggMetric(expr->Cast<FunctionExpression>());
			expr->SetAlias(alias);
			return;
		}
		if (analyzer.IsAggregate(*expr)) {
			return;
		}
		if (expr->GetExpressionClass() == ExpressionClass::COLUMN_REF) {
			CheckNotMetric(*expr);
			return;
		}
		ParsedExpressionIterator::EnumerateChildren(
		    *expr, [&](unique_ptr<ParsedExpression> &child) { UnwrapAggCalls(child); });
	}

	//! A time_shift / cumulative call as one metric item: the AGG() calls of its metric (the first positional
	//! argument) are unwrapped; the other arguments, named ones included, are kept as written so that they resolve in
	//! the semantic query
	unique_ptr<ParsedExpression> TransformMetric(const ParsedExpression &call) {
		auto result = WithoutAlias(call);
		for (auto &argument : result->Cast<FunctionExpression>().GetArgumentsMutable()) {
			auto &expr = argument.GetExpressionMutable();
			if (!argument.HasName() && expr->GetAlias().empty()) {
				UnwrapAggCalls(expr);
				break;
			}
		}
		return result;
	}

	//! Registers the metric item for an aggregate or transform call (deduplicated) and returns its column
	string MetricFor(const ParsedExpression &aggregate) {
		unique_ptr<ParsedExpression> metric;
		if (IsAggCall(aggregate)) {
			metric = AggMetric(aggregate.Cast<FunctionExpression>());
		} else if (IsTransformCall(aggregate)) {
			metric = TransformMetric(aggregate);
		} else {
			metric = WithoutAlias(aggregate);
		}
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
			return Replace(*select[alias.GetIndex()], false);
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
				for (auto &alias : select_aliases) {
					if (!alias.empty() && StringUtil::CIEquals(alias, colref.GetColumnName().GetIdentifierName())) {
						return expr.Copy();
					}
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
			if (!window_items[i] && select[i]->Equals(expr)) {
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
		if (!select_aliases[index].empty()) {
			return select_aliases[index];
		}
		if (select[index]->GetExpressionClass() == ExpressionClass::COLUMN_REF) {
			return select[index]->Cast<ColumnRefExpression>().GetColumnName().GetIdentifierName();
		}
		return original_text[index];
	}

	const SemanticModel &model;
	SemanticAnalyzer analyzer;
	const BaseTableRef &ref;
	vector<unique_ptr<ParsedExpression>> select;
	vector<string> select_aliases;
	vector<string> original_text;
	//! Whether each select item contains a window function
	vector<bool> window_items;
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
