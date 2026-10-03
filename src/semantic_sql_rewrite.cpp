#include "semantic_sql_rewrite.hpp"

#include "duckdb/catalog/catalog.hpp"
#include "duckdb/catalog/entry_lookup_info.hpp"
#include "duckdb/common/exception/binder_exception.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/common/to_string.hpp"
#include "duckdb/parser/expression/columnref_expression.hpp"
#include "duckdb/parser/expression/constant_expression.hpp"
#include "duckdb/parser/expression/function_expression.hpp"
#include "duckdb/parser/parsed_expression_iterator.hpp"
#include "duckdb/parser/query_node/select_node.hpp"
#include "duckdb/parser/result_modifier.hpp"
#include "duckdb/parser/tableref/basetableref.hpp"
#include "semantic_analysis.hpp"
#include "semantic_ast_helpers.hpp"
#include "semantic_catalog.hpp"
#include "semantic_lowering.hpp"
#include "semantic_statement_visitor.hpp"
#include "semantic_view_registry.hpp"

namespace duckdb {

namespace {

//! Rewrites one `SELECT ... FROM <semantic view>` query node
class StandardFormRewriter {
public:
	StandardFormRewriter(ClientContext &context, const SemanticModel &model, const BaseTableRef &ref)
	    : model(model), analyzer(model, context), ref(ref) {
		query.view.catalog = ref.catalog_name;
		query.view.schema = ref.schema_name;
		query.view.name = ref.table_name;
	}

	void Rewrite(SelectNode &node) {
		CheckSupported(node);
		auto orders = OrderExpressions(node);
		NormalizeClauses(node, orders);
		CollectGroups(node);
		auto aggregated = HasAggregation(orders);
		// the items of the semantic query are registered in this order: groups, select list, HAVING, ORDER BY
		node.select_list = aggregated ? AggregatedSelect() : FactSelect();
		node.where_clause = aggregated && having ? Replace(*having, true) : nullptr;
		for (auto order : orders) {
			*order = aggregated ? Replace(**order, true) : MapToFacts(**order);
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
		if (node.qualify) {
			throw BinderException("QUALIFY is not supported when querying semantic view \"%s\" by name",
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

	static vector<unique_ptr<ParsedExpression> *> OrderExpressions(SelectNode &node) {
		vector<unique_ptr<ParsedExpression> *> result;
		for (auto &modifier : node.modifiers) {
			if (modifier->type == ResultModifierType::ORDER_MODIFIER) {
				for (auto &order : modifier->Cast<OrderModifier>().orders) {
					result.push_back(&order.expression);
				}
			}
		}
		return result;
	}

	//! Normalized copies of the select list, WHERE (the semantic query's filter) and HAVING; ORDER BY in place
	void NormalizeClauses(const SelectNode &node, const vector<unique_ptr<ParsedExpression> *> &orders) {
		for (auto &expr : node.select_list) {
			select.push_back(Normalize(*expr));
			select.back()->alias = expr->alias;
			select_aliases.push_back(expr->alias);
			original_text.push_back(expr->ToString());
			CheckNoWindow(*select.back());
		}
		if (node.where_clause) {
			query.where = Normalize(*node.where_clause);
		}
		if (node.having) {
			having = Normalize(*node.having);
			CheckNoWindow(*having);
		}
		for (auto order : orders) {
			*order = Normalize(**order);
		}
	}

	//! GROUP BY expressions (ordinals and select aliases resolve to select items)
	void CollectGroups(const SelectNode &node) {
		if (node.aggregate_handling == AggregateHandling::FORCE_AGGREGATES) {
			for (auto &expr : select) {
				if (!ContainsAggregate(*expr)) {
					groups.push_back(WithoutAlias(*expr));
				}
			}
			return;
		}
		for (auto &group : node.groups.group_expressions) {
			groups.push_back(ResolveGroup(*group));
		}
	}

	bool HasAggregation(const vector<unique_ptr<ParsedExpression> *> &orders) {
		if (!groups.empty() || (having && ContainsAggregate(*having))) {
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
			expr->alias = OutputName(i);
			result.push_back(std::move(expr));
		}
		return result;
	}

	//! Row-level query: every select item is a fact
	vector<unique_ptr<ParsedExpression>> FactSelect() {
		vector<unique_ptr<ParsedExpression>> result;
		for (idx_t i = 0; i < select.size(); i++) {
			CheckNotMetric(*select[i]);
			AddItem(SemanticMemberKind::FACT, WithoutAlias(*select[i]), FactColumn(i));
			result.push_back(ColumnRef(FactColumn(i)));
			result.back()->alias = OutputName(i);
		}
		return result;
	}

	static unique_ptr<ParsedExpression> WithoutAlias(const ParsedExpression &expr) {
		auto result = expr.Copy();
		result->alias.clear();
		return result;
	}

	bool IsViewQualifier(const string &qualifier) const {
		if (model.FindTable(qualifier).IsValid()) {
			return false;
		}
		return StringUtil::CIEquals(qualifier, ref.alias.empty() ? ref.table_name : ref.alias);
	}

	//! Strips qualifiers naming the semantic view itself and splits quoted "entity.name" identifiers
	unique_ptr<ParsedExpression> Normalize(const ParsedExpression &expr) {
		auto result = expr.Copy();
		std::function<void(unique_ptr<ParsedExpression> &)> visit = [&](unique_ptr<ParsedExpression> &node) {
			if (node->GetExpressionClass() == ExpressionClass::COLUMN_REF) {
				auto &colref = node->Cast<ColumnRefExpression>();
				auto names = colref.column_names;
				if (names.size() >= 2 && IsViewQualifier(names[0])) {
					names.erase(names.begin());
				}
				if (names.size() == 1 && StringUtil::Contains(names[0], ".")) {
					auto dot = names[0].find('.');
					auto table = names[0].substr(0, dot);
					if (model.FindTable(table).IsValid()) {
						names = {table, names[0].substr(dot + 1)};
					}
				}
				auto alias = node->alias;
				node = make_uniq<ColumnRefExpression>(std::move(names));
				node->alias = alias;
				return;
			}
			ParsedExpressionIterator::EnumerateChildren(*node, visit);
		};
		visit(result);
		return result;
	}

	void CheckNoWindow(const ParsedExpression &expr) {
		if (SemanticAnalyzer::ContainsWindow(expr)) {
			throw BinderException("Window functions are not supported when querying semantic view \"%s\" by name; "
			                      "define a window function metric instead",
			                      model.Name().name);
		}
	}

	bool IsAggregateCall(const ParsedExpression &expr) {
		if (expr.GetExpressionClass() != ExpressionClass::FUNCTION) {
			return false;
		}
		auto &function = expr.Cast<FunctionExpression>();
		return StringUtil::CIEquals(function.function_name, "agg") || analyzer.IsAggregate(expr);
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
			auto &value = normalized->Cast<ConstantExpression>().value;
			if (value.type().IsIntegral()) {
				auto index = value.GetValue<int64_t>();
				if (index < 1 || index > int64_t(select.size())) {
					throw BinderException("GROUP BY term out of range - should be between 1 and %d", select.size());
				}
				return WithoutAlias(*select[NumericCast<idx_t>(index - 1)]);
			}
		}
		if (normalized->GetExpressionClass() == ExpressionClass::COLUMN_REF) {
			auto &colref = normalized->Cast<ColumnRefExpression>();
			if (!colref.IsQualified()) {
				for (idx_t i = 0; i < select.size(); i++) {
					if (!select_aliases[i].empty() && StringUtil::CIEquals(select_aliases[i], colref.GetColumnName())) {
						return WithoutAlias(*select[i]);
					}
				}
			}
		}
		return normalized;
	}

	//! Rejects bare references to metrics (they must be wrapped in AGG)
	void CheckNotMetric(const ParsedExpression &expr) {
		if (expr.GetExpressionClass() == ExpressionClass::COLUMN_REF) {
			string ignored;
			auto ref = model.Lookup(expr.Cast<ColumnRefExpression>().column_names, SemanticResolveContext(), ignored);
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

	//! Registers the metric item for an aggregate call (deduplicated) and returns its column
	string MetricFor(const ParsedExpression &aggregate) {
		unique_ptr<ParsedExpression> metric;
		auto &function = aggregate.Cast<FunctionExpression>();
		if (StringUtil::CIEquals(function.function_name, "agg")) {
			if (function.children.size() != 1 || function.distinct || function.filter) {
				throw BinderException("AGG() takes exactly one metric");
			}
			metric = WithoutAlias(*function.children[0]);
			SemanticResolveContext resolve_context;
			resolve_context.allow_private = false;
			resolve_context.what = "AGG(" + metric->ToString() + ")";
			auto resolved = model.Resolve(*metric, resolve_context);
			if (!analyzer.ContainsAggregateOrMetric(*resolved)) {
				throw BinderException("AGG() requires a metric; \"%s\" is not a metric", metric->ToString());
			}
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

	unique_ptr<ParsedExpression> Replace(const ParsedExpression &expr, bool allow_aliases) {
		for (idx_t g = 0; g < groups.size(); g++) {
			if (groups[g]->Equals(expr)) {
				return make_uniq<ColumnRefExpression>(GroupColumn(g));
			}
		}
		if (IsAggregateCall(expr)) {
			return make_uniq<ColumnRefExpression>(MetricFor(expr));
		}
		if (expr.GetExpressionClass() == ExpressionClass::COLUMN_REF) {
			auto &colref = expr.Cast<ColumnRefExpression>();
			if (allow_aliases && !colref.IsQualified()) {
				for (auto &alias : select_aliases) {
					if (!alias.empty() && StringUtil::CIEquals(alias, colref.GetColumnName())) {
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
		ParsedExpressionIterator::EnumerateChildren(
		    *result, [&](unique_ptr<ParsedExpression> &child) { child = Replace(*child, allow_aliases); });
		return result;
	}

	unique_ptr<ParsedExpression> MapToFacts(const ParsedExpression &expr) {
		for (idx_t i = 0; i < select.size(); i++) {
			if (select[i]->Equals(expr)) {
				return make_uniq<ColumnRefExpression>(FactColumn(i));
			}
		}
		return expr.Copy();
	}

	string OutputName(idx_t index) {
		if (!select_aliases[index].empty()) {
			return select_aliases[index];
		}
		if (select[index]->GetExpressionClass() == ExpressionClass::COLUMN_REF) {
			return select[index]->Cast<ColumnRefExpression>().GetColumnName();
		}
		return original_text[index];
	}

	const SemanticModel &model;
	SemanticAnalyzer analyzer;
	const BaseTableRef &ref;
	vector<unique_ptr<ParsedExpression>> select;
	vector<string> select_aliases;
	vector<string> original_text;
	unique_ptr<ParsedExpression> having;
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
			throw BinderException("Semantic view \"%s\" cannot be joined in the FROM clause; use SEMANTIC_VIEW(...) "
			                      "or a subquery instead",
			                      ref->Cast<BaseTableRef>().table_name);
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
		bool unqualified = (ref.catalog_name.empty() || ref.catalog_name == INVALID_CATALOG) &&
		                   (ref.schema_name.empty() || ref.schema_name == INVALID_SCHEMA);
		if (unqualified && IsCTEName(ref.table_name)) {
			return nullptr;
		}
		QualifiedName name;
		name.catalog = ref.catalog_name;
		name.schema = ref.schema_name;
		name.name = ref.table_name;
		auto model = state.registry.Lookup(context, name);
		if (!model) {
			return nullptr;
		}
		// a real table or view of that name (another catalog's, or a CTE) takes precedence over the semantic view; its
		// own catalog entry is a placeholder
		EntryLookupInfo lookup(CatalogType::TABLE_ENTRY, ref.table_name);
		auto entry =
		    Catalog::GetEntry(context, ref.catalog_name, ref.schema_name, lookup, OnEntryNotFound::RETURN_NULL);
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
