#include "semantic_query_compiler.hpp"

#include "duckdb/common/error_data.hpp"
#include "duckdb/common/exception.hpp"
#include "duckdb/common/exception/binder_exception.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/common/to_string.hpp"
#include "duckdb/parser/expression/case_expression.hpp"
#include "duckdb/parser/expression/columnref_expression.hpp"
#include "duckdb/parser/expression/comparison_expression.hpp"
#include "duckdb/parser/expression/conjunction_expression.hpp"
#include "duckdb/parser/expression/constant_expression.hpp"
#include "duckdb/parser/expression/function_expression.hpp"
#include "duckdb/parser/expression/operator_expression.hpp"
#include "duckdb/parser/expression/star_expression.hpp"
#include "duckdb/parser/expression/subquery_expression.hpp"
#include "duckdb/parser/expression/window_expression.hpp"
#include "duckdb/parser/parsed_expression_iterator.hpp"
#include "duckdb/parser/query_node/select_node.hpp"
#include "duckdb/parser/query_node/set_operation_node.hpp"
#include "duckdb/parser/result_modifier.hpp"
#include "duckdb/parser/tableref/joinref.hpp"
#include "duckdb/parser/tableref/subqueryref.hpp"
#include "duckdb/planner/binder.hpp"

namespace duckdb {

static constexpr const char *BLOCK_COLUMN = "__sv_block";
static constexpr const char *RANGE_KEY_COLUMN = "__sv_key";
static constexpr const char *AGGREGATED_ALIAS = "__sv_agg";

static string DimensionColumn(idx_t index) {
	return "__sv_d" + to_string(index);
}

static string AtomColumn(idx_t index) {
	return "__sv_a" + to_string(index);
}

static unique_ptr<ParsedExpression> AndAll(vector<unique_ptr<ParsedExpression>> conditions) {
	if (conditions.empty()) {
		return nullptr;
	}
	auto result = std::move(conditions[0]);
	for (idx_t i = 1; i < conditions.size(); i++) {
		result = make_uniq<ConjunctionExpression>(ExpressionType::CONJUNCTION_AND, std::move(result),
		                                          std::move(conditions[i]));
	}
	return result;
}

static unique_ptr<ParsedExpression> Equals(const string &left_alias, const string &left_column,
                                           const string &right_alias, const string &right_column) {
	return make_uniq<ComparisonExpression>(ExpressionType::COMPARE_EQUAL,
	                                       make_uniq<ColumnRefExpression>(left_column, left_alias),
	                                       make_uniq<ColumnRefExpression>(right_column, right_alias));
}

//===--------------------------------------------------------------------===//
// Join scope: the tables joined (LEFT, along unique many-to-one paths) from a base table
//===--------------------------------------------------------------------===//
class SemanticJoinScope {
public:
	SemanticJoinScope(const SemanticModel &model, idx_t base, string prefix, vector<idx_t> using_relationships = {},
	                  optional_ptr<SemanticJoinScope> parent = nullptr, idx_t outer_context = DConstants::INVALID_INDEX)
	    : model(model), base(base), using_relationships(std::move(using_relationships)), prefix(std::move(prefix)),
	      parent(parent), outer_context(outer_context) {
		present.insert(base);
	}

	string AliasOf(idx_t table) const {
		return prefix + model.tables[table].name;
	}

	//! Makes `table` available in this scope and returns its alias
	string Ensure(idx_t table) {
		if (parent && outer_context != DConstants::INVALID_INDEX && model.Reaches(outer_context, table)) {
			// values of the outer row (and of the tables it reaches) come from the enclosing query
			return parent->Ensure(table);
		}
		if (present.count(table)) {
			return AliasOf(table);
		}
		idx_t current = base;
		for (auto &edge : model.Path(base, table, using_relationships)) {
			if (!present.count(edge.target)) {
				joins.push_back(Joined {edge.target, edge, current});
				present.insert(edge.target);
			}
			current = edge.target;
		}
		return AliasOf(table);
	}

	unique_ptr<TableRef> BuildFrom() const {
		auto result = model.tables[base].source->Copy();
		result->alias = AliasOf(base);
		for (auto &join : joins) {
			auto &rel = model.relationships[join.edge.relationship];
			auto right = model.tables[join.table].source->Copy();
			right->alias = AliasOf(join.table);
			auto &parent_columns = join.edge.reversed ? rel.to_columns : rel.from_columns;
			auto &child_columns = join.edge.reversed ? rel.from_columns : rel.to_columns;
			if (rel.is_range) {
				auto join_ref = make_uniq<JoinRef>(JoinRefType::REGULAR);
				join_ref->type = JoinType::LEFT;
				join_ref->left = std::move(result);
				join_ref->right = RangeLookup(rel, AliasOf(join.table));
				join_ref->condition =
				    Equals(AliasOf(join.parent_table), rel.from_columns[0], AliasOf(join.table), RANGE_KEY_COLUMN);
				result = std::move(join_ref);
				continue;
			}
			vector<unique_ptr<ParsedExpression>> conditions;
			for (idx_t c = 0; c < parent_columns.size(); c++) {
				if (c == rel.asof_index) {
					// ASOF: the latest row of the referenced table at or before the referencing value
					conditions.push_back(make_uniq<ComparisonExpression>(
					    ExpressionType::COMPARE_GREATERTHANOREQUALTO,
					    make_uniq<ColumnRefExpression>(parent_columns[c], AliasOf(join.parent_table)),
					    make_uniq<ColumnRefExpression>(child_columns[c], AliasOf(join.table))));
					continue;
				}
				conditions.push_back(
				    Equals(AliasOf(join.parent_table), parent_columns[c], AliasOf(join.table), child_columns[c]));
			}
			auto join_ref = make_uniq<JoinRef>(rel.asof_index != DConstants::INVALID_INDEX ? JoinRefType::ASOF
			                                                                               : JoinRefType::REGULAR);
			join_ref->type = JoinType::LEFT;
			join_ref->left = std::move(result);
			join_ref->right = std::move(right);
			join_ref->condition = AndAll(std::move(conditions));
			result = std::move(join_ref);
		}
		return result;
	}

	const SemanticModel &model;
	idx_t base;
	vector<idx_t> using_relationships;

private:
	//! Range relationship `from(c) REFERENCES to(BETWEEN s AND e EXCLUSIVE)`: maps every distinct value of `c` to its
	//! range row, so that the fact table joins the result on equality. Ranges with a start are matched with a
	//! sort-merge ASOF join; the (at most one, ranges being distinct) range without a start matches values below its
	//! end:
	//!   SELECT k.__sv_key, r.* FROM (SELECT DISTINCT c AS __sv_key FROM from) k
	//!     ASOF JOIN (SELECT * FROM to WHERE s IS NOT NULL) r ON k.__sv_key >= r.s
	//!   WHERE r.e IS NULL OR k.__sv_key < r.e
	//!   UNION ALL
	//!   SELECT k.__sv_key, r.* FROM (...) k JOIN (SELECT * FROM to WHERE s IS NULL) r
	//!     ON r.e IS NULL OR k.__sv_key < r.e
	unique_ptr<TableRef> RangeLookup(const SemanticRelationship &rel, const string &alias) const {
		auto &from = model.tables[rel.from_table];
		auto &to = model.tables[rel.to_table];
		auto keys = [&]() {
			auto node = make_uniq<SelectNode>();
			auto key = make_uniq<ColumnRefExpression>(rel.from_columns[0]);
			key->alias = RANGE_KEY_COLUMN;
			node->select_list.push_back(std::move(key));
			node->from_table = from.source->Copy();
			node->modifiers.push_back(make_uniq<DistinctModifier>());
			auto statement = make_uniq<SelectStatement>();
			statement->node = std::move(node);
			return make_uniq<SubqueryRef>(std::move(statement), "__sv_k");
		};
		auto ranges = [&](bool open_start) {
			auto node = make_uniq<SelectNode>();
			node->select_list.push_back(make_uniq<StarExpression>());
			node->from_table = to.source->Copy();
			node->where_clause = make_uniq<OperatorExpression>(open_start ? ExpressionType::OPERATOR_IS_NULL
			                                                              : ExpressionType::OPERATOR_IS_NOT_NULL,
			                                                   make_uniq<ColumnRefExpression>(rel.range_start));
			auto statement = make_uniq<SelectStatement>();
			statement->node = std::move(node);
			return make_uniq<SubqueryRef>(std::move(statement), "__sv_r");
		};
		auto before_end = [&]() {
			auto open_end = make_uniq<OperatorExpression>(ExpressionType::OPERATOR_IS_NULL,
			                                              make_uniq<ColumnRefExpression>(rel.range_end, "__sv_r"));
			auto less = make_uniq<ComparisonExpression>(ExpressionType::COMPARE_LESSTHAN,
			                                            make_uniq<ColumnRefExpression>(RANGE_KEY_COLUMN, "__sv_k"),
			                                            make_uniq<ColumnRefExpression>(rel.range_end, "__sv_r"));
			return make_uniq<ConjunctionExpression>(ExpressionType::CONJUNCTION_OR, std::move(open_end),
			                                        std::move(less));
		};
		auto mapping = [&](unique_ptr<TableRef> from_table, unique_ptr<ParsedExpression> where) {
			auto node = make_uniq<SelectNode>();
			node->select_list.push_back(make_uniq<ColumnRefExpression>(RANGE_KEY_COLUMN, "__sv_k"));
			node->select_list.push_back(make_uniq<StarExpression>("__sv_r"));
			node->from_table = std::move(from_table);
			node->where_clause = std::move(where);
			return node;
		};

		auto asof = make_uniq<JoinRef>(JoinRefType::ASOF);
		asof->type = JoinType::INNER;
		asof->left = keys();
		asof->right = ranges(false);
		asof->condition = make_uniq<ComparisonExpression>(ExpressionType::COMPARE_GREATERTHANOREQUALTO,
		                                                  make_uniq<ColumnRefExpression>(RANGE_KEY_COLUMN, "__sv_k"),
		                                                  make_uniq<ColumnRefExpression>(rel.range_start, "__sv_r"));
		auto open = make_uniq<JoinRef>(JoinRefType::REGULAR);
		open->type = JoinType::INNER;
		open->left = keys();
		open->right = ranges(true);
		open->condition = before_end();

		auto union_node = make_uniq<SetOperationNode>();
		union_node->setop_type = SetOperationType::UNION;
		union_node->setop_all = true;
		union_node->children.push_back(mapping(std::move(asof), before_end()));
		union_node->children.push_back(mapping(std::move(open), nullptr));
		auto statement = make_uniq<SelectStatement>();
		statement->node = std::move(union_node);
		return make_uniq<SubqueryRef>(std::move(statement), alias);
	}

	struct Joined {
		idx_t table;
		SemanticEdge edge;
		idx_t parent_table;
	};

	string prefix;
	optional_ptr<SemanticJoinScope> parent;
	idx_t outer_context;
	vector<Joined> joins;
	set<idx_t> present;
};

//===--------------------------------------------------------------------===//
// Expression compilation
//===--------------------------------------------------------------------===//
namespace {

class ExpressionCompiler {
public:
	ExpressionCompiler(const SemanticModel &model, SemanticAnalyzer &analyzer, idx_t &scope_counter)
	    : model(model), analyzer(analyzer), scope_counter(scope_counter) {
	}

	//! Compiles a row-level expression evaluated per row of `scope`'s base, in the row context of `context_table`
	unique_ptr<ParsedExpression> Row(const ParsedExpression &expr, SemanticJoinScope &scope, idx_t context_table,
	                                 const string &what) {
		idx_t table;
		string column;
		if (SemanticModel::IsColumnMarker(expr, table, column)) {
			auto result = make_uniq<ColumnRefExpression>(column, scope.Ensure(table));
			result->alias = expr.alias;
			return std::move(result);
		}
		idx_t member;
		if (SemanticModel::IsMemberMarker(expr, member)) {
			auto &entry = model.members[member];
			unique_ptr<ParsedExpression> result;
			if (entry.IsRowLevel()) {
				result = Row(*entry.resolved, scope, entry.table, what);
			} else {
				// a metric used in a row-level expression: aggregate the finer rows of the current row
				vector<SemanticAtom> atoms;
				auto post = analyzer.Decompose(*entry.resolved, entry.table, atoms, what, entry.using_relationships);
				result = ReplaceAtoms(
				    *post, [&](idx_t atom) { return ChildAggregate(atoms[atom], context_table, scope, what); });
			}
			result->alias = expr.alias;
			return result;
		}
		if (analyzer.IsAggregate(expr)) {
			if (context_table == DConstants::INVALID_INDEX) {
				throw BinderException("%s cannot contain an aggregate", what);
			}
			SemanticAtom atom;
			atom.home = analyzer.AggregateHome(expr.Cast<FunctionExpression>(), context_table, what);
			atom.aggregate = expr.Copy();
			atom.context_table = context_table;
			atom.using_relationships = scope.using_relationships;
			auto result = ChildAggregate(atom, context_table, scope, what);
			result->alias = expr.alias;
			return result;
		}
		auto result = expr.Copy();
		ParsedExpressionIterator::EnumerateChildren(
		    *result, [&](unique_ptr<ParsedExpression> &child) { child = Row(*child, scope, context_table, what); });
		return result;
	}

	//! Compiles an aggregate atom evaluated over the rows of `scope`'s base
	unique_ptr<ParsedExpression> Aggregate(const SemanticAtom &atom, SemanticJoinScope &scope, const string &what) {
		auto result = atom.aggregate->Copy();
		ParsedExpressionIterator::EnumerateChildren(
		    *result, [&](unique_ptr<ParsedExpression> &child) { child = Row(*child, scope, atom.home, what); });
		return result;
	}

	template <class FUNC>
	unique_ptr<ParsedExpression> ReplaceAtoms(const ParsedExpression &expr, FUNC replacement) {
		idx_t atom;
		if (SemanticAnalyzer::IsAtomRef(expr, atom)) {
			auto result = replacement(atom);
			result->alias = expr.alias;
			return result;
		}
		auto result = expr.Copy();
		ParsedExpressionIterator::EnumerateChildren(
		    *result, [&](unique_ptr<ParsedExpression> &child) { child = ReplaceAtoms(*child, replacement); });
		return result;
	}

private:
	//! Correlated scalar subquery aggregating the rows of atom.home that belong to the current row of `context`
	unique_ptr<ParsedExpression> ChildAggregate(const SemanticAtom &atom, idx_t context, SemanticJoinScope &outer,
	                                            const string &what) {
		if (context == DConstants::INVALID_INDEX || atom.home == context || !model.Reaches(atom.home, context)) {
			throw BinderException("%s: cannot aggregate rows of logical table \"%s\" here", what,
			                      model.tables[atom.home].name);
		}
		auto prefix = "__sv" + to_string(++scope_counter) + "_";
		SemanticJoinScope inner(model, atom.home, prefix, atom.using_relationships, &outer, context);
		auto aggregate = Aggregate(atom, inner, what);

		auto path = model.Path(atom.home, context, atom.using_relationships);
		auto &last = path.back();
		auto source = path.size() >= 2 ? path[path.size() - 2].target : atom.home;
		auto &rel = model.relationships[last.relationship];
		if (rel.asof_index != DConstants::INVALID_INDEX || rel.is_range) {
			throw BinderException("%s: aggregating the rows of \"%s\" through the ASOF / range relationship \"%s\" "
			                      "is not supported",
			                      what, model.tables[atom.home].name, rel.name);
		}
		auto &inner_columns = last.reversed ? rel.to_columns : rel.from_columns;
		auto &outer_columns = last.reversed ? rel.from_columns : rel.to_columns;
		auto inner_alias = inner.Ensure(source);
		auto outer_alias = outer.Ensure(context);
		vector<unique_ptr<ParsedExpression>> conditions;
		for (idx_t c = 0; c < inner_columns.size(); c++) {
			conditions.push_back(Equals(inner_alias, inner_columns[c], outer_alias, outer_columns[c]));
		}
		auto node = make_uniq<SelectNode>();
		node->select_list.push_back(std::move(aggregate));
		node->from_table = inner.BuildFrom();
		node->where_clause = AndAll(std::move(conditions));
		auto subquery = make_uniq<SubqueryExpression>();
		subquery->subquery_type = SubqueryType::SCALAR;
		subquery->subquery = make_uniq<SelectStatement>();
		subquery->subquery->node = std::move(node);
		return std::move(subquery);
	}

	const SemanticModel &model;
	SemanticAnalyzer &analyzer;
	idx_t &scope_counter;
};

} // namespace

//===--------------------------------------------------------------------===//
// Compiler
//===--------------------------------------------------------------------===//
static unique_ptr<ParsedExpression> ColumnRef(const string &name) {
	return make_uniq<ColumnRefExpression>(name);
}

//! Semi-additive block: rank the rows of every group (of query dimension values) by the
//! NON ADDITIVE BY dimensions, keep the last snapshot (rank 1 in reversed order), then aggregate.
static unique_ptr<SelectNode> BuildNonAdditiveBlock(ExpressionCompiler &compiler, SemanticJoinScope &scope,
                                                    const SemanticMember &metric,
                                                    const vector<const SemanticQueryItem *> &dimensions,
                                                    const vector<SemanticAtom> &atoms, const vector<idx_t> &owned_atoms,
                                                    optional_idx pad_block, optional_ptr<ParsedExpression> where) {
	auto inner = make_uniq<SelectNode>();
	auto outer = make_uniq<SelectNode>();
	auto rank = make_uniq<WindowExpression>(ExpressionType::WINDOW_RANK, INVALID_CATALOG, INVALID_SCHEMA, "rank");
	rank->start = WindowBoundary::UNBOUNDED_PRECEDING;
	rank->end = WindowBoundary::CURRENT_ROW_RANGE;
	for (idx_t d = 0; d < dimensions.size(); d++) {
		auto expr = compiler.Row(*dimensions[d]->resolved, scope, DConstants::INVALID_INDEX, "Invalid dimension");
		// every group picks its own last snapshot; when a non-additive dimension is itself grouped on, all rows
		// of a group share its value and the aggregation is a plain one
		rank->partitions.push_back(expr->Copy());
		expr->alias = DimensionColumn(d);
		inner->select_list.push_back(std::move(expr));
		outer->select_list.push_back(ColumnRef(DimensionColumn(d)));
		outer->groups.group_expressions.push_back(ColumnRef(DimensionColumn(d)));
	}
	idx_t extracted = 0;
	auto extract = [&](unique_ptr<ParsedExpression> &expr) {
		auto name = "__sv_x" + to_string(extracted++);
		expr->alias = name;
		inner->select_list.push_back(std::move(expr));
		expr = ColumnRef(name);
	};
	for (idx_t a = 0; a < atoms.size(); a++) {
		bool owned = std::find(owned_atoms.begin(), owned_atoms.end(), a) != owned_atoms.end();
		if (!owned && !pad_block.IsValid()) {
			continue;
		}
		unique_ptr<ParsedExpression> expr;
		if (owned) {
			expr = compiler.Aggregate(atoms[a], scope, "Invalid metric");
			auto &aggregate = expr->Cast<FunctionExpression>();
			for (auto &child : aggregate.children) {
				extract(child);
			}
			if (aggregate.filter) {
				extract(aggregate.filter);
			}
			if (aggregate.order_bys) {
				for (auto &order : aggregate.order_bys->orders) {
					extract(order.expression);
				}
			}
		} else {
			expr = make_uniq<ConstantExpression>(Value());
		}
		expr->alias = AtomColumn(a);
		outer->select_list.push_back(std::move(expr));
	}
	unique_ptr<ParsedExpression> keep_condition;
	if (metric.non_additive.size() == 1) {
		// single non-additive dimension: compare every row with the last value of its group, computed with
		// unordered window aggregates (no sort): max (ASC) / min (DESC), or NULL when NULLs sort last and exist
		auto &entry = metric.non_additive[0];
		auto value = compiler.Row(*entry.resolved, scope, DConstants::INVALID_INDEX, "NON ADDITIVE BY");
		auto window = [&](const string &function, unique_ptr<ParsedExpression> child) {
			auto result = make_uniq<WindowExpression>(ExpressionType::WINDOW_AGGREGATE, INVALID_CATALOG, INVALID_SCHEMA,
			                                          function);
			if (child) {
				result->children.push_back(std::move(child));
			}
			for (auto &partition : rank->partitions) {
				result->partitions.push_back(partition->Copy());
			}
			result->start = WindowBoundary::UNBOUNDED_PRECEDING;
			result->end = WindowBoundary::UNBOUNDED_FOLLOWING;
			return result;
		};
		unique_ptr<ParsedExpression> last = window(entry.type == OrderType::ASCENDING ? "max" : "min", value->Copy());
		if (entry.null_order == OrderByNullType::NULLS_LAST) {
			auto any_null = make_uniq<ComparisonExpression>(
			    ExpressionType::COMPARE_GREATERTHAN, window("count_star", nullptr), window("count", value->Copy()));
			auto case_expr = make_uniq<CaseExpression>();
			CaseCheck check;
			check.when_expr = std::move(any_null);
			check.then_expr = make_uniq<ConstantExpression>(Value());
			case_expr->case_checks.push_back(std::move(check));
			case_expr->else_expr = std::move(last);
			last = std::move(case_expr);
		}
		last->alias = "__sv_last";
		value->alias = "__sv_na";
		inner->select_list.push_back(std::move(value));
		inner->select_list.push_back(std::move(last));
		keep_condition = make_uniq<ComparisonExpression>(ExpressionType::COMPARE_NOT_DISTINCT_FROM,
		                                                 ColumnRef("__sv_na"), ColumnRef("__sv_last"));
	} else {
		for (auto &entry : metric.non_additive) {
			// the last row in the requested order is the first one in the reversed order
			auto type = entry.type == OrderType::ASCENDING ? OrderType::DESCENDING : OrderType::ASCENDING;
			auto nulls = entry.null_order == OrderByNullType::NULLS_LAST ? OrderByNullType::NULLS_FIRST
			                                                             : OrderByNullType::NULLS_LAST;
			rank->orders.emplace_back(
			    type, nulls, compiler.Row(*entry.resolved, scope, DConstants::INVALID_INDEX, "NON ADDITIVE BY"));
		}
		rank->alias = "__sv_rank";
		inner->select_list.push_back(std::move(rank));
		keep_condition = make_uniq<ComparisonExpression>(ExpressionType::COMPARE_EQUAL, ColumnRef("__sv_rank"),
		                                                 make_uniq<ConstantExpression>(Value::BIGINT(1)));
	}
	if (where) {
		inner->where_clause = compiler.Row(*where, scope, DConstants::INVALID_INDEX, "Invalid WHERE clause");
	}
	inner->from_table = scope.BuildFrom();
	if (pad_block.IsValid()) {
		auto tag = make_uniq<ConstantExpression>(Value::BIGINT(NumericCast<int64_t>(pad_block.GetIndex())));
		tag->alias = BLOCK_COLUMN;
		outer->select_list.push_back(std::move(tag));
	}
	if (!outer->groups.group_expressions.empty()) {
		GroupingSet set;
		for (idx_t i = 0; i < outer->groups.group_expressions.size(); i++) {
			set.insert(i);
		}
		outer->groups.grouping_sets.push_back(std::move(set));
	}
	outer->where_clause = std::move(keep_condition);
	auto inner_statement = make_uniq<SelectStatement>();
	inner_statement->node = std::move(inner);
	outer->from_table = make_uniq<SubqueryRef>(std::move(inner_statement), "__sv_snapshot");
	return outer;
}

//! Window-function metrics run over the merged rows: their dimension references become the query's dimension
//! columns (which must be requested), and PARTITION BY EXCLUDING expands to every other query dimension.
void SemanticQueryCompiler::MapWindowDimensions(unique_ptr<ParsedExpression> &expr,
                                                const vector<const SemanticQueryItem *> &dimensions) {
	auto dimension_column = [&](const ParsedExpression &reference) -> unique_ptr<ParsedExpression> {
		for (idx_t d = 0; d < dimensions.size(); d++) {
			if (dimensions[d]->resolved->Equals(reference)) {
				return ColumnRef(DimensionColumn(d));
			}
		}
		idx_t member;
		auto name =
		    SemanticModel::IsMemberMarker(reference, member) ? model.MemberDisplayName(member) : reference.ToString();
		throw BinderException("Invalid semantic view query: Dimension '%s' used in a window function metric must be "
		                      "requested in the query.",
		                      name);
	};
	std::function<void(unique_ptr<ParsedExpression> &)> map_key = [&](unique_ptr<ParsedExpression> &key) {
		idx_t member;
		if (SemanticModel::IsMemberMarker(*key, member)) {
			key = dimension_column(*key);
			return;
		}
		ParsedExpressionIterator::EnumerateChildren(*key, map_key);
	};
	if (expr->GetExpressionClass() == ExpressionClass::WINDOW) {
		auto &window = expr->Cast<WindowExpression>();
		vector<unique_ptr<ParsedExpression>> partitions;
		for (auto &partition : window.partitions) {
			if (!SemanticAnalyzer::IsExcludingMarker(*partition)) {
				map_key(partition);
				partitions.push_back(std::move(partition));
				continue;
			}
			auto &excluded = partition->Cast<FunctionExpression>().children;
			for (auto &dimension : excluded) {
				dimension_column(*dimension);
			}
			for (idx_t d = 0; d < dimensions.size(); d++) {
				bool is_excluded = false;
				for (auto &dimension : excluded) {
					is_excluded = is_excluded || dimensions[d]->resolved->Equals(*dimension);
				}
				if (!is_excluded) {
					partitions.push_back(ColumnRef(DimensionColumn(d)));
				}
			}
		}
		window.partitions = std::move(partitions);
		for (auto &order : window.orders) {
			map_key(order.expression);
		}
		for (auto &child : window.children) {
			MapWindowDimensions(child, dimensions);
		}
		return;
	}
	ParsedExpressionIterator::EnumerateChildren(
	    *expr, [&](unique_ptr<ParsedExpression> &child) { MapWindowDimensions(child, dimensions); });
}

SemanticQueryCompiler::SemanticQueryCompiler(const SemanticModel &model, ClientContext &context)
    : model(model), context(context), analyzer(model, context) {
}

static string ItemWhat(const SemanticQueryItem &item) {
	switch (item.kind) {
	case SemanticMemberKind::DIMENSION:
		return "Invalid dimension \"" + item.output_name + "\"";
	case SemanticMemberKind::FACT:
		return "Invalid fact \"" + item.output_name + "\"";
	default:
		return "Invalid metric \"" + item.output_name + "\"";
	}
}

SemanticQuerySpec SemanticQueryCompiler::Resolve(const vector<ParsedSemanticQueryItem> &items,
                                                 optional_ptr<ParsedExpression> where) {
	SemanticQuerySpec spec;
	for (auto &item : items) {
		if (!item.expression) {
			auto table = model.FindTable(item.star_table);
			if (!table.IsValid()) {
				throw BinderException("Unknown logical table \"%s\" in \"%s.*\"", item.star_table, item.star_table);
			}
			for (idx_t m = 0; m < model.members.size(); m++) {
				auto &member = model.members[m];
				if (member.table != table.GetIndex() || member.kind != item.kind || member.is_private) {
					continue;
				}
				SemanticQueryItem resolved;
				resolved.kind = item.kind;
				resolved.resolved = SemanticModel::MakeMemberMarker(m);
				resolved.output_name = member.name;
				spec.items.push_back(std::move(resolved));
			}
			continue;
		}
		SemanticQueryItem resolved;
		resolved.kind = item.kind;
		SemanticResolveContext resolve_context;
		resolve_context.allow_private = false;
		resolve_context.what = "Invalid " + StringUtil::Lower(SemanticMemberKindToString(item.kind)) + " \"" +
		                       item.expression->ToString() + "\"";
		resolved.resolved = model.Resolve(*item.expression, resolve_context);
		if (!item.alias.empty()) {
			resolved.output_name = item.alias;
		} else {
			idx_t member;
			idx_t table;
			string column;
			if (SemanticModel::IsMemberMarker(*resolved.resolved, member)) {
				resolved.output_name = model.members[member].name;
			} else if (SemanticModel::IsColumnMarker(*resolved.resolved, table, column)) {
				resolved.output_name = column;
			} else {
				resolved.output_name = item.expression->ToString();
			}
		}
		spec.items.push_back(std::move(resolved));
	}
	if (where) {
		SemanticResolveContext resolve_context;
		resolve_context.allow_private = false;
		resolve_context.what = "Invalid WHERE clause";
		spec.where = model.Resolve(*where, resolve_context);
	}
	return spec;
}

unique_ptr<SelectStatement> SemanticQueryCompiler::Compile(const SemanticQuerySpec &spec) {
	ExpressionCompiler compiler(model, analyzer, scope_counter);
	vector<const SemanticQueryItem *> dimensions;
	vector<const SemanticQueryItem *> facts;
	vector<const SemanticQueryItem *> metrics;
	case_insensitive_set_t output_names;
	for (auto &item : spec.items) {
		if (!output_names.insert(item.output_name).second) {
			throw BinderException("Duplicate output column name \"%s\" in SEMANTIC_VIEW; use an alias",
			                      item.output_name);
		}
		auto has_aggregate = analyzer.ContainsAggregateOrMetric(*item.resolved);
		switch (item.kind) {
		case SemanticMemberKind::DIMENSION:
		case SemanticMemberKind::FACT:
			if (has_aggregate) {
				throw BinderException("%s: metrics and aggregates are only allowed in METRICS", ItemWhat(item));
			}
			(item.kind == SemanticMemberKind::DIMENSION ? dimensions : facts).push_back(&item);
			break;
		case SemanticMemberKind::METRIC:
			if (!has_aggregate) {
				throw BinderException("%s: a METRICS expression must aggregate (use a metric or an aggregate "
				                      "function); use DIMENSIONS or FACTS for row-level values",
				                      ItemWhat(item));
			}
			metrics.push_back(&item);
			break;
		}
	}
	if (!facts.empty() && !metrics.empty()) {
		throw BinderException("Cannot specify FACTS and METRICS in the same SEMANTIC_VIEW clause");
	}
	if (spec.where && analyzer.ContainsAggregateOrMetric(*spec.where)) {
		throw BinderException("The WHERE clause of SEMANTIC_VIEW can only reference dimensions and facts");
	}

	// row-level tables referenced by dimensions / facts / WHERE
	set<idx_t> row_tables;
	for (auto *item : dimensions) {
		analyzer.RowTables(*item->resolved, DConstants::INVALID_INDEX, row_tables, ItemWhat(*item));
	}
	for (auto *item : facts) {
		analyzer.RowTables(*item->resolved, DConstants::INVALID_INDEX, row_tables, ItemWhat(*item));
	}
	if (spec.where) {
		analyzer.RowTables(*spec.where, DConstants::INVALID_INDEX, row_tables, "Invalid WHERE clause");
	}

	auto result = make_uniq<SelectStatement>();
	if (metrics.empty()) {
		// dimensions and/or facts: a row-level query over the finest referenced table
		auto base = analyzer.FinestTable(row_tables, "Invalid SEMANTIC_VIEW query");
		if (base == DConstants::INVALID_INDEX) {
			throw BinderException("SEMANTIC_VIEW must reference at least one dimension or fact");
		}
		if (!facts.empty() && !dimensions.empty()) {
			for (auto table : row_tables) {
				if (table != base) {
					throw BinderException("When FACTS and DIMENSIONS are both specified, all facts and dimensions "
					                      "(including those in the WHERE clause) must be defined in the same logical "
					                      "table");
				}
			}
		}
		SemanticJoinScope scope(model, base, "");
		auto node = make_uniq<SelectNode>();
		for (auto &item : spec.items) {
			auto expr = compiler.Row(*item.resolved, scope, DConstants::INVALID_INDEX, ItemWhat(item));
			if (item.kind == SemanticMemberKind::DIMENSION && !facts.empty()) {
				node->groups.group_expressions.push_back(expr->Copy());
			}
			if (item.kind == SemanticMemberKind::FACT && !dimensions.empty()) {
				vector<unique_ptr<ParsedExpression>> children;
				children.push_back(std::move(expr));
				expr = make_uniq<FunctionExpression>("any_value", std::move(children));
			}
			expr->alias = item.output_name;
			node->select_list.push_back(std::move(expr));
		}
		if (!node->groups.group_expressions.empty()) {
			GroupingSet set;
			for (idx_t i = 0; i < node->groups.group_expressions.size(); i++) {
				set.insert(i);
			}
			node->groups.grouping_sets.push_back(std::move(set));
		}
		if (facts.empty()) {
			node->modifiers.push_back(make_uniq<DistinctModifier>());
		}
		if (spec.where) {
			node->where_clause = compiler.Row(*spec.where, scope, DConstants::INVALID_INDEX, "Invalid WHERE clause");
		}
		node->from_table = scope.BuildFrom();
		result->node = std::move(node);
		return result;
	}

	// metrics: decompose into aggregate atoms grouped by home table
	vector<SemanticAtom> atoms;
	vector<unique_ptr<ParsedExpression>> posts;
	for (auto *item : metrics) {
		posts.push_back(analyzer.Decompose(*item->resolved, DConstants::INVALID_INDEX, atoms, ItemWhat(*item)));
	}
	// one block per (home table, USING relationships, NON ADDITIVE BY metric)
	vector<idx_t> block_homes;
	vector<vector<idx_t>> block_using;
	vector<idx_t> block_non_additive;
	vector<vector<idx_t>> block_atoms;
	for (idx_t a = 0; a < atoms.size(); a++) {
		auto using_key = atoms[a].using_relationships;
		std::sort(using_key.begin(), using_key.end());
		idx_t block = DConstants::INVALID_INDEX;
		for (idx_t b = 0; b < block_homes.size(); b++) {
			if (block_homes[b] == atoms[a].home && block_using[b] == using_key &&
			    block_non_additive[b] == atoms[a].non_additive_member) {
				block = b;
			}
		}
		if (block == DConstants::INVALID_INDEX) {
			block = block_homes.size();
			block_homes.push_back(atoms[a].home);
			block_using.push_back(using_key);
			block_non_additive.push_back(atoms[a].non_additive_member);
			block_atoms.emplace_back();
		}
		block_atoms[block].push_back(a);
	}

	auto build_block = [&](idx_t block, bool pad) {
		auto home = block_homes[block];
		for (auto table : row_tables) {
			if (!model.Reaches(home, table)) {
				throw BinderException("Invalid dimension specified: The dimension entity '%s' must be related to and "
				                      "have an equal or lower level of granularity compared to the base metric or "
				                      "dimension entity '%s'.",
				                      model.tables[table].name, model.tables[home].name);
			}
		}
		SemanticJoinScope scope(model, home, "", block_using[block]);
		if (block_non_additive[block] != DConstants::INVALID_INDEX) {
			return BuildNonAdditiveBlock(compiler, scope, model.members[block_non_additive[block]], dimensions, atoms,
			                             block_atoms[block], pad ? optional_idx(block) : optional_idx(),
			                             spec.where.get());
		}
		auto node = make_uniq<SelectNode>();
		for (idx_t d = 0; d < dimensions.size(); d++) {
			auto expr =
			    compiler.Row(*dimensions[d]->resolved, scope, DConstants::INVALID_INDEX, ItemWhat(*dimensions[d]));
			node->groups.group_expressions.push_back(expr->Copy());
			expr->alias = DimensionColumn(d);
			node->select_list.push_back(std::move(expr));
		}
		for (idx_t a = 0; a < atoms.size(); a++) {
			bool owned = std::find(block_atoms[block].begin(), block_atoms[block].end(), a) != block_atoms[block].end();
			if (!owned && !pad) {
				continue;
			}
			unique_ptr<ParsedExpression> expr;
			if (owned) {
				expr = compiler.Aggregate(atoms[a], scope, "Invalid metric");
			} else {
				expr = make_uniq<ConstantExpression>(Value());
			}
			expr->alias = AtomColumn(a);
			node->select_list.push_back(std::move(expr));
		}
		if (pad) {
			auto tag = make_uniq<ConstantExpression>(Value::BIGINT(NumericCast<int64_t>(block)));
			tag->alias = BLOCK_COLUMN;
			node->select_list.push_back(std::move(tag));
		}
		if (!node->groups.group_expressions.empty()) {
			GroupingSet set;
			for (idx_t i = 0; i < node->groups.group_expressions.size(); i++) {
				set.insert(i);
			}
			node->groups.grouping_sets.push_back(std::move(set));
		}
		if (spec.where) {
			node->where_clause = compiler.Row(*spec.where, scope, DConstants::INVALID_INDEX, "Invalid WHERE clause");
		}
		node->from_table = scope.BuildFrom();
		return node;
	};

	unique_ptr<QueryNode> aggregated;
	if (block_homes.size() == 1) {
		aggregated = build_block(0, false);
	} else {
		// UNION ALL of the blocks, merged on the dimension values (NULLs compare equal in GROUP BY)
		auto union_node = make_uniq<SetOperationNode>();
		union_node->setop_type = SetOperationType::UNION;
		union_node->setop_all = true;
		for (idx_t b = 0; b < block_homes.size(); b++) {
			union_node->children.push_back(build_block(b, true));
		}
		auto merged = make_uniq<SelectNode>();
		for (idx_t d = 0; d < dimensions.size(); d++) {
			merged->select_list.push_back(make_uniq<ColumnRefExpression>(DimensionColumn(d)));
			merged->groups.group_expressions.push_back(make_uniq<ColumnRefExpression>(DimensionColumn(d)));
		}
		for (idx_t b = 0; b < block_homes.size(); b++) {
			for (auto a : block_atoms[b]) {
				vector<unique_ptr<ParsedExpression>> children;
				children.push_back(make_uniq<ColumnRefExpression>(AtomColumn(a)));
				auto any_value = make_uniq<FunctionExpression>("any_value", std::move(children));
				any_value->filter = make_uniq<ComparisonExpression>(
				    ExpressionType::COMPARE_EQUAL, make_uniq<ColumnRefExpression>(BLOCK_COLUMN),
				    make_uniq<ConstantExpression>(Value::BIGINT(NumericCast<int64_t>(b))));
				any_value->alias = AtomColumn(a);
				merged->select_list.push_back(std::move(any_value));
			}
		}
		if (!merged->groups.group_expressions.empty()) {
			GroupingSet set;
			for (idx_t i = 0; i < merged->groups.group_expressions.size(); i++) {
				set.insert(i);
			}
			merged->groups.grouping_sets.push_back(std::move(set));
		}
		auto union_statement = make_uniq<SelectStatement>();
		union_statement->node = std::move(union_node);
		merged->from_table = make_uniq<SubqueryRef>(std::move(union_statement), "__sv_blocks");
		aggregated = std::move(merged);
	}

	// final projection: requested items in order
	auto final_node = make_uniq<SelectNode>();
	idx_t dimension_index = 0;
	idx_t metric_index = 0;
	for (auto &item : spec.items) {
		unique_ptr<ParsedExpression> expr;
		if (item.kind == SemanticMemberKind::METRIC) {
			expr = compiler.ReplaceAtoms(*posts[metric_index++],
			                             [](idx_t atom) { return make_uniq<ColumnRefExpression>(AtomColumn(atom)); });
			MapWindowDimensions(expr, dimensions);
		} else {
			expr = make_uniq<ColumnRefExpression>(DimensionColumn(dimension_index++));
		}
		expr->alias = item.output_name;
		final_node->select_list.push_back(std::move(expr));
	}
	auto aggregated_statement = make_uniq<SelectStatement>();
	aggregated_statement->node = std::move(aggregated);
	final_node->from_table = make_uniq<SubqueryRef>(std::move(aggregated_statement), AGGREGATED_ALIAS);
	result->node = std::move(final_node);
	return result;
}

unique_ptr<SelectStatement> SemanticQueryCompiler::CompileMemberProbe(idx_t member, idx_t &column) {
	auto &entry = model.members[member];
	SemanticQuerySpec spec;
	if (entry.kind == SemanticMemberKind::METRIC) {
		// window-function metrics can only be evaluated together with their window dimensions
		set<idx_t> window_dimensions;
		analyzer.WindowDimensions(*SemanticModel::MakeMemberMarker(member), window_dimensions);
		for (auto dimension : window_dimensions) {
			SemanticQueryItem item;
			item.kind = SemanticMemberKind::DIMENSION;
			item.resolved = SemanticModel::MakeMemberMarker(dimension);
			item.output_name = "__sv_probe_d" + to_string(dimension);
			spec.items.push_back(std::move(item));
		}
	}
	SemanticQueryItem item;
	item.kind = entry.kind == SemanticMemberKind::METRIC ? SemanticMemberKind::METRIC : SemanticMemberKind::FACT;
	item.resolved = SemanticModel::MakeMemberMarker(member);
	item.output_name = entry.name;
	spec.items.push_back(std::move(item));
	column = spec.items.size() - 1;
	return Compile(spec);
}

void ProbeSemanticModelTypes(ClientContext &context, SemanticModel &model) {
	SemanticQueryCompiler compiler(model, context);
	for (idx_t m = 0; m < model.members.size(); m++) {
		auto &member = model.members[m];
		try {
			idx_t column;
			auto statement = compiler.CompileMemberProbe(m, column);
			auto binder = Binder::CreateBinder(context);
			auto bound = binder->Bind(statement->Cast<SQLStatement>());
			member.type = bound.types[column];
		} catch (std::exception &ex) {
			ErrorData error(ex);
			throw BinderException("Invalid semantic view \"%s\": %s \"%s\": %s", model.name.name,
			                      SemanticModel::KindDisplayName(member.kind), model.MemberDisplayName(m),
			                      error.RawMessage());
		}
	}
}

} // namespace duckdb
