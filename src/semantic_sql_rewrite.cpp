#include "semantic_sql_rewrite.hpp"

#include "duckdb/catalog/catalog.hpp"
#include "duckdb/catalog/entry_lookup_info.hpp"
#include "duckdb/common/case_insensitive_map.hpp"
#include "duckdb/common/exception.hpp"
#include "duckdb/common/exception/binder_exception.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/common/to_string.hpp"
#include "duckdb/parser/expression/columnref_expression.hpp"
#include "duckdb/parser/expression/constant_expression.hpp"
#include "duckdb/parser/expression/function_expression.hpp"
#include "duckdb/parser/expression/star_expression.hpp"
#include "duckdb/parser/expression/subquery_expression.hpp"
#include "duckdb/parser/parsed_data/create_table_info.hpp"
#include "duckdb/parser/parsed_expression_iterator.hpp"
#include "duckdb/parser/query_node/select_node.hpp"
#include "duckdb/parser/query_node/set_operation_node.hpp"
#include "duckdb/parser/result_modifier.hpp"
#include "duckdb/parser/statement/create_statement.hpp"
#include "duckdb/parser/statement/explain_statement.hpp"
#include "duckdb/parser/statement/insert_statement.hpp"
#include "duckdb/parser/statement/prepare_statement.hpp"
#include "duckdb/parser/statement/select_statement.hpp"
#include "duckdb/parser/tableref/basetableref.hpp"
#include "duckdb/parser/tableref/joinref.hpp"
#include "duckdb/parser/tableref/showref.hpp"
#include "duckdb/parser/tableref/subqueryref.hpp"
#include "duckdb/parser/tableref/table_function_ref.hpp"
#include "semantic_analysis.hpp"
#include "semantic_view_registry.hpp"

namespace duckdb {

namespace {

bool ContainsWindowFunction(const ParsedExpression &expr) {
	return SemanticAnalyzer::ContainsWindow(expr);
}

//! Rewrites one `SELECT ... FROM <semantic view>` query node
class StandardFormRewriter {
public:
	StandardFormRewriter(ClientContext &context, const SemanticModel &model, const BaseTableRef &ref)
	    : context(context), model(model), analyzer(model, context), ref(ref) {
	}

	void Rewrite(SelectNode &node) {
		if (node.qualify) {
			throw BinderException("QUALIFY is not supported when querying semantic view \"%s\" by name",
			                      model.name.name);
		}
		for (auto &expr : node.select_list) {
			if (expr->GetExpressionClass() == ExpressionClass::STAR) {
				throw BinderException("SELECT * is not supported when querying semantic view \"%s\" by name: list "
				                      "dimensions and facts, and metrics with AGG(...)",
				                      model.name.name);
			}
		}
		// normalized copies of every clause
		for (auto &expr : node.select_list) {
			select.push_back(Normalize(*expr));
			select.back()->alias = expr->alias;
			select_aliases.push_back(expr->alias);
			original_text.push_back(expr->ToString());
		}
		auto having = node.having ? Normalize(*node.having) : nullptr;
		auto where = node.where_clause ? Normalize(*node.where_clause) : nullptr;
		for (auto &modifier : node.modifiers) {
			if (modifier->type == ResultModifierType::ORDER_MODIFIER) {
				for (auto &order : modifier->Cast<OrderModifier>().orders) {
					order.expression = Normalize(*order.expression);
				}
			}
		}
		for (auto &expr : select) {
			CheckNoWindow(*expr);
		}
		if (having) {
			CheckNoWindow(*having);
		}

		// GROUP BY expressions (ordinals and select aliases resolve to select items)
		if (node.aggregate_handling == AggregateHandling::FORCE_AGGREGATES) {
			for (auto &expr : select) {
				if (!ContainsAggregate(*expr)) {
					groups.push_back(WithoutAlias(*expr));
				}
			}
		} else {
			for (auto &group : node.groups.group_expressions) {
				groups.push_back(ResolveGroup(*group));
			}
		}
		bool has_aggregation = !groups.empty() || (having && ContainsAggregate(*having));
		for (auto &expr : select) {
			has_aggregation = has_aggregation || ContainsAggregate(*expr);
		}
		for (auto &modifier : node.modifiers) {
			if (modifier->type == ResultModifierType::ORDER_MODIFIER) {
				for (auto &order : modifier->Cast<OrderModifier>().orders) {
					has_aggregation = has_aggregation || ContainsAggregate(*order.expression);
				}
			}
		}

		vector<unique_ptr<ParsedExpression>> outer_select;
		if (has_aggregation) {
			for (idx_t g = 0; g < groups.size(); g++) {
				CheckNotMetric(*groups[g]);
				AddItem(SemanticMemberKind::DIMENSION, groups[g]->Copy(), GroupColumn(g));
			}
			for (idx_t i = 0; i < select.size(); i++) {
				auto expr = Replace(*select[i], false);
				expr->alias = OutputName(i);
				outer_select.push_back(std::move(expr));
			}
			node.where_clause = having ? Replace(*having, true) : nullptr;
		} else {
			for (idx_t i = 0; i < select.size(); i++) {
				CheckNotMetric(*select[i]);
				AddItem(SemanticMemberKind::FACT, WithoutAlias(*select[i]), FactColumn(i));
				auto expr = make_uniq<ColumnRefExpression>(FactColumn(i));
				expr->alias = OutputName(i);
				outer_select.push_back(std::move(expr));
			}
			node.where_clause = nullptr;
		}
		for (auto &modifier : node.modifiers) {
			if (modifier->type == ResultModifierType::ORDER_MODIFIER) {
				for (auto &order : modifier->Cast<OrderModifier>().orders) {
					order.expression =
					    has_aggregation ? Replace(*order.expression, true) : MapToFacts(*order.expression);
				}
			}
		}
		query.where = std::move(where);
		node.select_list = std::move(outer_select);
		node.having = nullptr;
		node.groups = GroupByNode();
		node.aggregate_handling = AggregateHandling::STANDARD_HANDLING;
		node.from_table = MakeTableFunction();
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
		if (ContainsWindowFunction(expr)) {
			throw BinderException("Window functions are not supported when querying semantic view \"%s\" by name; "
			                      "define a window function metric instead",
			                      model.name.name);
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

	//! Whether `expr` is a bare reference to a metric (which must be wrapped in AGG)
	void CheckNotMetric(const ParsedExpression &expr) {
		if (expr.GetExpressionClass() == ExpressionClass::COLUMN_REF) {
			try {
				SemanticResolveContext resolve_context;
				auto resolved = model.Resolve(expr, resolve_context);
				idx_t member;
				if (SemanticModel::IsMemberMarker(*resolved, member) &&
				    model.members[member].kind == SemanticMemberKind::METRIC) {
					throw BinderException("Metric \"%s\" must be used with AGG()", expr.ToString());
				}
			} catch (BinderException &ex) {
				if (StringUtil::Contains(ex.what(), "must be used with AGG()")) {
					throw;
				}
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
			if (metrics[m]->Equals(*metric) && metrics[m]->ToString() == metric->ToString()) {
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
			if (groups[g]->Equals(expr) && groups[g]->ToString() == WithoutAlias(expr)->ToString()) {
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
			if (select[i]->Equals(expr) && WithoutAlias(*select[i])->ToString() == WithoutAlias(expr)->ToString()) {
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

	unique_ptr<TableRef> MakeTableFunction() {
		vector<unique_ptr<ParsedExpression>> children;
		children.push_back(make_uniq<ConstantExpression>(Value(ref.catalog_name)));
		children.push_back(make_uniq<ConstantExpression>(Value(ref.schema_name)));
		children.push_back(make_uniq<ConstantExpression>(Value(ref.table_name)));
		children.push_back(make_uniq<ConstantExpression>(query.ItemsToValue()));
		children.push_back(make_uniq<ConstantExpression>(Value(query.where ? query.where->ToString() : string())));
		auto result = make_uniq<TableFunctionRef>();
		result->function = make_uniq<FunctionExpression>(SEMANTIC_VIEW_QUERY_FUNCTION, std::move(children));
		result->alias = "__sv_std";
		return std::move(result);
	}

	ClientContext &context;
	const SemanticModel &model;
	SemanticAnalyzer analyzer;
	const BaseTableRef &ref;
	vector<unique_ptr<ParsedExpression>> select;
	vector<string> select_aliases;
	vector<string> original_text;
	vector<unique_ptr<ParsedExpression>> groups;
	vector<unique_ptr<ParsedExpression>> metrics;
	ParsedSemanticQuery query;
};

//! Walks a statement and rewrites the query nodes that read from a semantic view by name
class SemanticReferenceWalker {
public:
	SemanticReferenceWalker(ClientContext &context, SemanticViewsState &state) : context(context), state(state) {
	}

	void Statement(SQLStatement &statement) {
		switch (statement.type) {
		case StatementType::SELECT_STATEMENT:
			Node(*statement.Cast<SelectStatement>().node);
			break;
		case StatementType::EXPLAIN_STATEMENT:
			Statement(*statement.Cast<ExplainStatement>().stmt);
			break;
		case StatementType::PREPARE_STATEMENT:
			Statement(*statement.Cast<PrepareStatement>().statement);
			break;
		case StatementType::CREATE_STATEMENT: {
			auto &create = statement.Cast<CreateStatement>();
			if (create.info && create.info->type == CatalogType::TABLE_ENTRY) {
				auto &info = create.info->Cast<CreateTableInfo>();
				if (info.query) {
					Node(*info.query->node);
				}
			}
			break;
		}
		case StatementType::INSERT_STATEMENT: {
			auto &insert = statement.Cast<InsertStatement>();
			if (insert.select_statement) {
				Node(*insert.select_statement->node);
			}
			break;
		}
		default:
			break;
		}
	}

	bool rewritten = false;

private:
	void Node(QueryNode &node) {
		case_insensitive_set_t ctes;
		for (auto &cte : node.cte_map.map) {
			ctes.insert(cte.first);
		}
		cte_scopes.push_back(std::move(ctes));
		for (auto &cte : node.cte_map.map) {
			Node(*cte.second->query->node);
		}
		switch (node.type) {
		case QueryNodeType::SELECT_NODE: {
			auto &select = node.Cast<SelectNode>();
			if (select.from_table) {
				Ref(select.from_table, false);
			}
			for (auto &expr : select.select_list) {
				Expr(expr);
			}
			if (select.where_clause) {
				Expr(select.where_clause);
			}
			if (select.having) {
				Expr(select.having);
			}
			if (select.from_table && select.from_table->type == TableReferenceType::BASE_TABLE) {
				auto &base = select.from_table->Cast<BaseTableRef>();
				auto model = SemanticViewFor(base);
				if (model) {
					StandardFormRewriter rewriter(context, *model, base);
					rewriter.Rewrite(select);
					rewritten = true;
				}
			}
			break;
		}
		case QueryNodeType::SET_OPERATION_NODE:
			for (auto &child : node.Cast<SetOperationNode>().children) {
				Node(*child);
			}
			break;
		default:
			break;
		}
		cte_scopes.pop_back();
	}

	void Ref(unique_ptr<TableRef> &ref, bool in_join) {
		switch (ref->type) {
		case TableReferenceType::BASE_TABLE:
			if (in_join && SemanticViewFor(ref->Cast<BaseTableRef>())) {
				throw BinderException("Semantic view \"%s\" cannot be joined in the FROM clause; use "
				                      "SEMANTIC_VIEW(...) or a subquery instead",
				                      ref->Cast<BaseTableRef>().table_name);
			}
			break;
		case TableReferenceType::JOIN: {
			auto &join = ref->Cast<JoinRef>();
			Ref(join.left, true);
			Ref(join.right, true);
			if (join.condition) {
				Expr(join.condition);
			}
			break;
		}
		case TableReferenceType::SUBQUERY:
			Node(*ref->Cast<SubqueryRef>().subquery->node);
			break;
		case TableReferenceType::SHOW_REF: {
			auto &show = ref->Cast<ShowRef>();
			if (show.query) {
				Node(*show.query);
			}
			break;
		}
		default:
			break;
		}
	}

	void Expr(unique_ptr<ParsedExpression> &expr) {
		if (expr->GetExpressionClass() == ExpressionClass::SUBQUERY) {
			Node(*expr->Cast<SubqueryExpression>().subquery->node);
		}
		ParsedExpressionIterator::EnumerateChildren(*expr, [&](unique_ptr<ParsedExpression> &child) { Expr(child); });
	}

	shared_ptr<SemanticModel> SemanticViewFor(const BaseTableRef &ref) {
		bool unqualified = (ref.catalog_name.empty() || ref.catalog_name == INVALID_CATALOG) &&
		                   (ref.schema_name.empty() || ref.schema_name == INVALID_SCHEMA);
		if (unqualified) {
			for (auto &scope : cte_scopes) {
				if (scope.count(ref.table_name)) {
					return nullptr;
				}
			}
		}
		QualifiedName name;
		name.catalog = ref.catalog_name;
		name.schema = ref.schema_name;
		name.name = ref.table_name;
		auto model = state.registry.Lookup(context, name);
		if (!model) {
			return nullptr;
		}
		// a real table or view of that name takes precedence
		try {
			EntryLookupInfo lookup(CatalogType::TABLE_ENTRY, ref.table_name);
			if (Catalog::GetEntry(context, ref.catalog_name, ref.schema_name, lookup, OnEntryNotFound::RETURN_NULL)) {
				return nullptr;
			}
		} catch (std::exception &) {
		}
		return model;
	}

	ClientContext &context;
	SemanticViewsState &state;
	vector<case_insensitive_set_t> cte_scopes;
};

} // namespace

bool RewriteSemanticViewReferences(ClientContext &context, SemanticViewsState &state, SQLStatement &statement) {
	SemanticReferenceWalker walker(context, state);
	walker.Statement(statement);
	return walker.rewritten;
}

} // namespace duckdb
