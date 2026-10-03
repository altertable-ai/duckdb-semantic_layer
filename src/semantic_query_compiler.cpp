#include "semantic_query_compiler.hpp"

#include "duckdb/common/error_data.hpp"
#include "duckdb/common/exception.hpp"
#include "duckdb/common/exception/binder_exception.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/common/to_string.hpp"
#include "duckdb/parser/expression/case_expression.hpp"
#include "duckdb/parser/expression/constant_expression.hpp"
#include "duckdb/parser/expression/function_expression.hpp"
#include "duckdb/parser/expression/window_expression.hpp"
#include "duckdb/parser/parsed_expression_iterator.hpp"
#include "duckdb/parser/query_node/set_operation_node.hpp"
#include "duckdb/parser/result_modifier.hpp"
#include "duckdb/parser/tableref/joinref.hpp"
#include "duckdb/planner/binder.hpp"
#include "semantic_ast_helpers.hpp"
#include "semantic_bridge.hpp"
#include "semantic_join_scope.hpp"

namespace duckdb {

namespace {

string DimensionColumn(idx_t index) {
	return "__sv_d" + to_string(index);
}

string AtomColumn(idx_t index) {
	return "__sv_a" + to_string(index);
}

string BridgeKeyColumn(idx_t index) {
	return "__sv_k" + to_string(index);
}

constexpr const char *BLOCK_COLUMN = "__sv_block";

string ItemWhat(const SemanticQueryItem &item) {
	switch (item.kind) {
	case SemanticMemberKind::DIMENSION:
		return "Invalid dimension \"" + item.output_name + "\"";
	case SemanticMemberKind::FACT:
		return "Invalid fact \"" + item.output_name + "\"";
	default:
		return "Invalid metric \"" + item.output_name + "\"";
	}
}

} // namespace

//===--------------------------------------------------------------------===//
// Expressions
//===--------------------------------------------------------------------===//
class SemanticExpressionCompiler {
public:
	SemanticExpressionCompiler(const SemanticModel &model, SemanticAnalyzer &analyzer)
	    : model(model), analyzer(analyzer) {
	}

	//! A row-level expression, evaluated per row of `scope`'s base in the row context of `context_table`
	unique_ptr<ParsedExpression> Row(const ParsedExpression &expr, SemanticJoinScope &scope, idx_t context_table,
	                                 const string &what) {
		auto ref = SemanticMarkers::Classify(expr);
		unique_ptr<ParsedExpression> result;
		if (ref.Is(SemanticRefKind::COLUMN)) {
			result = scope.ColumnReference(ref.index, ref.column);
		} else if (ref.Is(SemanticRefKind::MEMBER) && model.members[ref.index].IsRowLevel()) {
			auto &member = model.members[ref.index];
			result = Row(*member.resolved, scope, member.table, what);
		} else if (ref.Is(SemanticRefKind::MEMBER)) {
			// a metric in a row-level expression: aggregate the finer rows belonging to the current row
			vector<SemanticAtom> atoms;
			auto post = analyzer.DecomposeMetric(ref.index, atoms, what);
			result = ReplaceAtoms(*post,
			                      [&](idx_t atom) { return ChildAggregate(atoms[atom], context_table, scope, what); });
		} else if (analyzer.IsAggregate(expr)) {
			SemanticAtom atom;
			atom.home = analyzer.AggregateHome(expr.Cast<FunctionExpression>(), context_table, what);
			atom.aggregate = expr.Copy();
			atom.context.table = context_table;
			atom.context.using_relationships = scope.UsingRelationships();
			result = ChildAggregate(atom, context_table, scope, what);
		} else {
			result = expr.Copy();
			ParsedExpressionIterator::EnumerateChildren(
			    *result, [&](unique_ptr<ParsedExpression> &child) { child = Row(*child, scope, context_table, what); });
			return result;
		}
		result->alias = expr.alias;
		return result;
	}

	//! An aggregate atom over the rows of `scope`'s base
	unique_ptr<ParsedExpression> Aggregate(const SemanticAtom &atom, SemanticJoinScope &scope, const string &what) {
		auto result = atom.aggregate->Copy();
		ParsedExpressionIterator::EnumerateChildren(
		    *result, [&](unique_ptr<ParsedExpression> &child) { child = Row(*child, scope, atom.home, what); });
		return result;
	}

	template <class FUNC>
	static unique_ptr<ParsedExpression> ReplaceAtoms(const ParsedExpression &expr, FUNC replacement) {
		auto ref = SemanticMarkers::Classify(expr);
		if (ref.Is(SemanticRefKind::ATOM)) {
			auto result = replacement(ref.index);
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
		auto path = model.Path(atom.home, context, atom.context.using_relationships);
		auto &last = path.back();
		auto &rel = model.relationships[last.relationship];
		if (rel.asof_index != DConstants::INVALID_INDEX || rel.is_range) {
			throw BinderException("%s: aggregating the rows of \"%s\" through the ASOF / range relationship \"%s\" "
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
		for (idx_t c = 0; c < inner_columns.size(); c++) {
			conditions.push_back(Compare(ExpressionType::COMPARE_EQUAL, ColumnRef(inner_columns[c], inner_alias),
			                             ColumnRef(outer_columns[c], outer_alias)));
		}
		node->where_clause = AndAll(std::move(conditions));
		node->from_table = inner.BuildFrom();
		return MakeScalarSubquery(std::move(node));
	}

	const SemanticModel &model;
	SemanticAnalyzer &analyzer;
};

namespace {

//! Window-function metrics run over the merged rows: their dimension references become the query's dimension
//! columns (which must be requested), and PARTITION BY EXCLUDING expands to every other query dimension.
void MapWindowDimensions(const SemanticModel &model, unique_ptr<ParsedExpression> &expr,
                         const vector<const SemanticQueryItem *> &dimensions) {
	auto dimension_column = [&](const ParsedExpression &reference) {
		for (idx_t d = 0; d < dimensions.size(); d++) {
			if (dimensions[d]->resolved->Equals(reference)) {
				return ColumnRef(DimensionColumn(d));
			}
		}
		auto ref = SemanticMarkers::Classify(reference);
		auto name = ref.Is(SemanticRefKind::MEMBER) ? model.MemberDisplayName(ref.index) : reference.ToString();
		throw BinderException("Invalid semantic view query: Dimension '%s' used in a window function metric must be "
		                      "requested in the query.",
		                      name);
	};
	std::function<void(unique_ptr<ParsedExpression> &)> map_key = [&](unique_ptr<ParsedExpression> &key) {
		if (SemanticMarkers::Classify(*key).Is(SemanticRefKind::MEMBER)) {
			key = dimension_column(*key);
			return;
		}
		ParsedExpressionIterator::EnumerateChildren(*key, map_key);
	};
	if (expr->GetExpressionClass() != ExpressionClass::WINDOW) {
		ParsedExpressionIterator::EnumerateChildren(
		    *expr, [&](unique_ptr<ParsedExpression> &child) { MapWindowDimensions(model, child, dimensions); });
		return;
	}
	auto &window = expr->Cast<WindowExpression>();
	vector<unique_ptr<ParsedExpression>> partitions;
	for (auto &partition : window.partitions) {
		if (!SemanticMarkers::IsExcluding(*partition)) {
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
		MapWindowDimensions(model, child, dimensions);
	}
}

//===--------------------------------------------------------------------===//
// Metric queries
//===--------------------------------------------------------------------===//
//! Atoms aggregated together: same home table, USING relationships and NON ADDITIVE BY metric
struct Block {
	idx_t home;
	vector<idx_t> using_relationships;
	idx_t non_additive_member;
	vector<idx_t> atoms;

	bool Accepts(const SemanticAtom &atom) const {
		return home == atom.home && using_relationships == atom.context.using_relationships &&
		       non_additive_member == atom.context.non_additive_member;
	}
	bool Owns(idx_t atom) const {
		return std::find(atoms.begin(), atoms.end(), atom) != atoms.end();
	}
};

//! Dimensions of a block reached through the same many-to-many bridge: they come from one de-duplicated
//! (anchor key, dimensions) mapping subquery joined to the block's rows
struct BridgeGroup {
	SemanticBridge bridge;
	vector<idx_t> dimensions;
	//! WHERE conjuncts on the bridged side: the group becomes a semi-join
	vector<const ParsedExpression *> conjuncts;
};

//! Top-level AND conjuncts of `expr`
void SplitConjuncts(const ParsedExpression &expr, vector<const ParsedExpression *> &conjuncts) {
	if (expr.GetExpressionType() == ExpressionType::CONJUNCTION_AND) {
		for (auto &child : expr.Cast<ConjunctionExpression>().children) {
			SplitConjuncts(*child, conjuncts);
		}
		return;
	}
	conjuncts.push_back(&expr);
}

class MetricQueryCompiler {
public:
	MetricQueryCompiler(const SemanticModel &model, SemanticAnalyzer &analyzer, SemanticExpressionCompiler &compiler,
	                    SemanticAliasGenerator &aliases, const SemanticQuerySpec &spec,
	                    const vector<const SemanticQueryItem *> &dimensions,
	                    const vector<const SemanticQueryItem *> &metrics)
	    : model(model), analyzer(analyzer), compiler(compiler), aliases(aliases), spec(spec), dimensions(dimensions),
	      resolver(model) {
		for (auto *item : metrics) {
			posts.push_back(analyzer.Decompose(*item->resolved, SemanticMetricContext(), atoms, ItemWhat(*item)));
		}
		for (idx_t a = 0; a < atoms.size(); a++) {
			auto block =
			    std::find_if(blocks.begin(), blocks.end(), [&](const Block &b) { return b.Accepts(atoms[a]); });
			if (block == blocks.end()) {
				blocks.push_back(Block {
				    atoms[a].home, atoms[a].context.using_relationships, atoms[a].context.non_additive_member, {}});
				block = blocks.end() - 1;
			}
			block->atoms.push_back(a);
		}
	}

	unique_ptr<SelectStatement> Compile() {
		auto aggregated =
		    blocks.size() == 1 ? unique_ptr<QueryNode>(BuildBlock(blocks[0], optional_idx())) : MergeBlocks();
		// final projection: requested items in order, metrics computed from their atoms
		auto node = make_uniq<SelectNode>();
		idx_t dimension = 0;
		idx_t metric = 0;
		for (auto &item : spec.items) {
			unique_ptr<ParsedExpression> expr;
			if (item.kind == SemanticMemberKind::METRIC) {
				expr = SemanticExpressionCompiler::ReplaceAtoms(*posts[metric++],
				                                                [](idx_t atom) { return ColumnRef(AtomColumn(atom)); });
				MapWindowDimensions(model, expr, dimensions);
			} else {
				expr = ColumnRef(DimensionColumn(dimension++));
			}
			expr->alias = item.output_name;
			node->select_list.push_back(std::move(expr));
		}
		node->from_table = MakeSubqueryRef(std::move(aggregated), "__sv_agg");
		return MakeSelectStatement(std::move(node));
	}

private:
	//! A block: an inner select produces the dimensions and the aggregate arguments for the rows of the home table
	//! (optionally restricted to the last snapshot); the outer select aggregates them by dimension. With `tag`,
	//! every atom of the query gets a column (NULL when not owned) plus the block tag, for the UNION ALL merge.
	unique_ptr<SelectNode> BuildBlock(const Block &block, optional_idx tag) {
		vector<BridgeGroup> bridges;
		vector<idx_t> dimension_bridge;
		for (auto *dimension : dimensions) {
			dimension_bridge.push_back(PlaceRowItem(block, *dimension->resolved, ItemWhat(*dimension), false, bridges));
		}
		vector<unique_ptr<ParsedExpression>> local_conjuncts;
		if (spec.where) {
			vector<const ParsedExpression *> conjuncts;
			SplitConjuncts(*spec.where, conjuncts);
			for (auto *conjunct : conjuncts) {
				auto bridge = PlaceRowItem(block, *conjunct, "Invalid WHERE clause", true, bridges);
				if (bridge == DConstants::INVALID_INDEX) {
					local_conjuncts.push_back(conjunct->Copy());
				} else {
					bridges[bridge].conjuncts.push_back(conjunct);
				}
			}
		}
		vector<string> bridge_aliases;
		for (idx_t b = 0; b < bridges.size(); b++) {
			bridge_aliases.push_back(aliases.NestedPrefix() + "bridge");
		}
		SemanticJoinScope scope(model, aliases, block.home, block.using_relationships);
		auto rows = make_uniq<SelectNode>();
		auto aggregate = make_uniq<SelectNode>();
		vector<unique_ptr<ParsedExpression>> groups;
		for (idx_t d = 0; d < dimensions.size(); d++) {
			unique_ptr<ParsedExpression> expr;
			if (dimension_bridge[d] == DConstants::INVALID_INDEX) {
				expr =
				    compiler.Row(*dimensions[d]->resolved, scope, DConstants::INVALID_INDEX, ItemWhat(*dimensions[d]));
			} else {
				bridges[dimension_bridge[d]].dimensions.push_back(d);
				expr = ColumnRef(DimensionColumn(d), bridge_aliases[dimension_bridge[d]]);
			}
			groups.push_back(expr->Copy());
			expr->alias = DimensionColumn(d);
			rows->select_list.push_back(std::move(expr));
			aggregate->select_list.push_back(ColumnRef(DimensionColumn(d)));
			aggregate->groups.group_expressions.push_back(ColumnRef(DimensionColumn(d)));
		}
		idx_t argument_count = 0;
		auto extract = [&](unique_ptr<ParsedExpression> &argument) {
			auto name = "__sv_x" + to_string(argument_count++);
			argument->alias = name;
			rows->select_list.push_back(std::move(argument));
			argument = ColumnRef(name);
		};
		for (idx_t a = 0; a < atoms.size(); a++) {
			unique_ptr<ParsedExpression> expr;
			if (block.Owns(a)) {
				expr = compiler.Aggregate(atoms[a], scope, "Invalid metric");
				auto &function = expr->Cast<FunctionExpression>();
				for (auto &child : function.children) {
					extract(child);
				}
				if (function.filter) {
					extract(function.filter);
				}
				if (function.order_bys) {
					for (auto &order : function.order_bys->orders) {
						extract(order.expression);
					}
				}
			} else if (tag.IsValid()) {
				expr = make_uniq<ConstantExpression>(Value());
			} else {
				continue;
			}
			expr->alias = AtomColumn(a);
			aggregate->select_list.push_back(std::move(expr));
		}
		if (block.non_additive_member != DConstants::INVALID_INDEX) {
			aggregate->where_clause =
			    LastSnapshotFilter(model.members[block.non_additive_member], scope, std::move(groups), *rows);
		}
		if (!local_conjuncts.empty()) {
			rows->where_clause = compiler.Row(*AndAll(std::move(local_conjuncts)), scope, DConstants::INVALID_INDEX,
			                                  "Invalid WHERE clause");
		}
		if (rows->select_list.empty()) {
			// e.g. only COUNT(*): the rows still have to be produced
			rows->select_list.push_back(make_uniq<ConstantExpression>(Value::BOOLEAN(true)));
		}
		// the anchor key of every bridge on the home side, before the joins of the scope are built
		vector<vector<unique_ptr<ParsedExpression>>> bridge_conditions;
		for (idx_t b = 0; b < bridges.size(); b++) {
			auto &bridge = bridges[b].bridge;
			vector<unique_ptr<ParsedExpression>> conditions;
			for (idx_t c = 0; c < bridge.anchor_key.size(); c++) {
				conditions.push_back(Compare(ExpressionType::COMPARE_EQUAL,
				                             scope.ColumnReference(bridge.anchor, bridge.anchor_key[c]),
				                             ColumnRef(BridgeKeyColumn(c), bridge_aliases[b])));
			}
			bridge_conditions.push_back(std::move(conditions));
		}
		rows->from_table = scope.BuildFrom();
		for (idx_t b = 0; b < bridges.size(); b++) {
			auto join = make_uniq<JoinRef>(JoinRefType::REGULAR);
			join->type = bridges[b].conjuncts.empty() ? JoinType::LEFT : JoinType::INNER;
			join->left = std::move(rows->from_table);
			join->right = MakeSubqueryRef(BridgeMap(block, bridges[b]), bridge_aliases[b]);
			join->condition = AndAll(std::move(bridge_conditions[b]));
			rows->from_table = std::move(join);
		}
		if (tag.IsValid()) {
			auto tag_value = make_uniq<ConstantExpression>(Value::BIGINT(NumericCast<int64_t>(tag.GetIndex())));
			tag_value->alias = BLOCK_COLUMN;
			aggregate->select_list.push_back(std::move(tag_value));
		}
		GroupByAll(*aggregate);
		aggregate->from_table = MakeSubqueryRef(std::move(rows), "__sv_rows");
		return aggregate;
	}

	BinderException GranularityError(idx_t table, idx_t home) const {
		return BinderException("Invalid dimension specified: The dimension entity '%s' must be related to and have an "
		                       "equal or lower level of granularity compared to the base metric or dimension entity "
		                       "'%s'.",
		                       model.tables[table].name, model.tables[home].name);
	}

	//! Where a row-level item of the query is evaluated for `block`: INVALID_INDEX for the block's own rows (every
	//! table it reads is reachable from the home table), else the index of its bridge group in `bridges`
	idx_t PlaceRowItem(const Block &block, const ParsedExpression &expr, const string &what, bool is_condition,
	                   vector<BridgeGroup> &bridges) {
		set<idx_t> tables;
		analyzer.RowTables(expr, DConstants::INVALID_INDEX, tables, what);
		vector<idx_t> far_tables;
		for (auto table : tables) {
			if (!model.Reaches(block.home, table)) {
				far_tables.push_back(table);
			}
		}
		if (far_tables.empty()) {
			return DConstants::INVALID_INDEX;
		}
		SemanticBridge bridge;
		if (!resolver.Resolve(block.home, tables, block.using_relationships, bridge)) {
			// every far table may still have its own bridge: the item mixes several of them
			set<idx_t> bridge_tables;
			for (auto table : far_tables) {
				SemanticBridge single;
				if (!resolver.Resolve(block.home, {table}, block.using_relationships, single)) {
					throw GranularityError(table, block.home);
				}
				bridge_tables.insert(single.bridge);
			}
			vector<string> names;
			for (auto table : bridge_tables) {
				names.push_back("\"" + model.tables[table].name + "\"");
			}
			throw BinderException("%s: combines tables reached through different many-to-many bridges (%s)%s", what,
			                      StringUtil::Join(names, ", "),
			                      is_condition ? "; split it into separate conditions combined with AND" : "");
		}
		for (idx_t b = 0; b < bridges.size(); b++) {
			if (bridges[b].bridge.SameAs(bridge)) {
				return b;
			}
		}
		bridges.push_back(BridgeGroup {std::move(bridge), {}});
		return bridges.size() - 1;
	}

	//! SELECT DISTINCT <anchor key>, <bridged dimensions> FROM <bridge> [joins]: each row of the home table joins
	//! at most one row per combination of bridged dimension values, so nothing is counted twice within a group
	unique_ptr<QueryNode> BridgeMap(const Block &block, const BridgeGroup &group) {
		auto &bridge = group.bridge;
		SemanticJoinScope scope(model, aliases, bridge.bridge, block.using_relationships, aliases.NestedPrefix());
		auto node = make_uniq<SelectNode>();
		for (idx_t c = 0; c < bridge.key_source_columns.size(); c++) {
			auto key = scope.ColumnReference(bridge.key_source, bridge.key_source_columns[c]);
			key->alias = BridgeKeyColumn(c);
			node->select_list.push_back(std::move(key));
		}
		for (auto d : group.dimensions) {
			auto expr =
			    compiler.Row(*dimensions[d]->resolved, scope, DConstants::INVALID_INDEX, ItemWhat(*dimensions[d]));
			expr->alias = DimensionColumn(d);
			node->select_list.push_back(std::move(expr));
		}
		vector<unique_ptr<ParsedExpression>> conditions;
		for (auto *conjunct : group.conjuncts) {
			conditions.push_back(compiler.Row(*conjunct, scope, DConstants::INVALID_INDEX, "Invalid WHERE clause"));
		}
		node->where_clause = AndAll(std::move(conditions));
		node->modifiers.push_back(make_uniq<DistinctModifier>());
		node->from_table = scope.BuildFrom();
		return std::move(node);
	}

	//! Semi-additive metrics keep, within every group of dimension values (`partitions`), the rows of the last
	//! snapshot in NON ADDITIVE BY order. Adds the needed columns to `rows` and returns the filter on them.
	unique_ptr<ParsedExpression> LastSnapshotFilter(const SemanticMember &metric, SemanticJoinScope &scope,
	                                                vector<unique_ptr<ParsedExpression>> partitions, SelectNode &rows) {
		auto window = [&](ExpressionType type, const string &function) {
			auto result = make_uniq<WindowExpression>(type, INVALID_CATALOG, INVALID_SCHEMA, function);
			for (auto &partition : partitions) {
				result->partitions.push_back(partition->Copy());
			}
			result->start = WindowBoundary::UNBOUNDED_PRECEDING;
			result->end = WindowBoundary::UNBOUNDED_FOLLOWING;
			return result;
		};
		if (metric.non_additive.size() > 1) {
			// several dimensions: rank by the reversed order, the last snapshot ranks first
			auto rank = window(ExpressionType::WINDOW_RANK, "rank");
			rank->end = WindowBoundary::CURRENT_ROW_RANGE;
			for (auto &entry : metric.non_additive) {
				auto type = entry.type == OrderType::ASCENDING ? OrderType::DESCENDING : OrderType::ASCENDING;
				auto nulls = entry.null_order == OrderByNullType::NULLS_LAST ? OrderByNullType::NULLS_FIRST
				                                                             : OrderByNullType::NULLS_LAST;
				rank->orders.emplace_back(
				    type, nulls, compiler.Row(*entry.resolved, scope, DConstants::INVALID_INDEX, "NON ADDITIVE BY"));
			}
			rank->alias = "__sv_rank";
			rows.select_list.push_back(std::move(rank));
			return Compare(ExpressionType::COMPARE_EQUAL, ColumnRef("__sv_rank"),
			               make_uniq<ConstantExpression>(Value::BIGINT(1)));
		}
		// one dimension: compare with the group's last value from unordered window aggregates (no sort):
		// max (ASC) / min (DESC), or NULL when NULLs sort last and the group has some
		auto &entry = metric.non_additive[0];
		auto value = compiler.Row(*entry.resolved, scope, DConstants::INVALID_INDEX, "NON ADDITIVE BY");
		auto last = window(ExpressionType::WINDOW_AGGREGATE, entry.type == OrderType::ASCENDING ? "max" : "min");
		last->children.push_back(value->Copy());
		unique_ptr<ParsedExpression> last_value = std::move(last);
		if (entry.null_order == OrderByNullType::NULLS_LAST) {
			auto counted = window(ExpressionType::WINDOW_AGGREGATE, "count");
			counted->children.push_back(value->Copy());
			CaseCheck has_nulls;
			has_nulls.when_expr = Compare(ExpressionType::COMPARE_GREATERTHAN,
			                              window(ExpressionType::WINDOW_AGGREGATE, "count_star"), std::move(counted));
			has_nulls.then_expr = make_uniq<ConstantExpression>(Value());
			auto case_expr = make_uniq<CaseExpression>();
			case_expr->case_checks.push_back(std::move(has_nulls));
			case_expr->else_expr = std::move(last_value);
			last_value = std::move(case_expr);
		}
		value->alias = "__sv_na";
		last_value->alias = "__sv_last";
		rows.select_list.push_back(std::move(value));
		rows.select_list.push_back(std::move(last_value));
		return Compare(ExpressionType::COMPARE_NOT_DISTINCT_FROM, ColumnRef("__sv_na"), ColumnRef("__sv_last"));
	}

	//! UNION ALL of the blocks merged on the dimension values: each block contributes at most one row per
	//! dimension tuple, so any_value(atom) FILTER (block = b) is that block's value (NULLs group together)
	unique_ptr<QueryNode> MergeBlocks() {
		auto blocks_union = make_uniq<SetOperationNode>();
		blocks_union->setop_type = SetOperationType::UNION;
		blocks_union->setop_all = true;
		for (idx_t b = 0; b < blocks.size(); b++) {
			blocks_union->children.push_back(BuildBlock(blocks[b], b));
		}
		auto merged = make_uniq<SelectNode>();
		for (idx_t d = 0; d < dimensions.size(); d++) {
			merged->select_list.push_back(ColumnRef(DimensionColumn(d)));
			merged->groups.group_expressions.push_back(ColumnRef(DimensionColumn(d)));
		}
		for (idx_t b = 0; b < blocks.size(); b++) {
			for (auto a : blocks[b].atoms) {
				vector<unique_ptr<ParsedExpression>> children;
				children.push_back(ColumnRef(AtomColumn(a)));
				auto any_value = make_uniq<FunctionExpression>("any_value", std::move(children));
				any_value->filter = Compare(ExpressionType::COMPARE_EQUAL, ColumnRef(BLOCK_COLUMN),
				                            make_uniq<ConstantExpression>(Value::BIGINT(NumericCast<int64_t>(b))));
				any_value->alias = AtomColumn(a);
				merged->select_list.push_back(std::move(any_value));
			}
		}
		GroupByAll(*merged);
		merged->from_table = MakeSubqueryRef(std::move(blocks_union), "__sv_blocks");
		return std::move(merged);
	}

	const SemanticModel &model;
	SemanticAnalyzer &analyzer;
	SemanticExpressionCompiler &compiler;
	SemanticAliasGenerator &aliases;
	const SemanticQuerySpec &spec;
	const vector<const SemanticQueryItem *> &dimensions;
	SemanticBridgeResolver resolver;
	vector<SemanticAtom> atoms;
	vector<unique_ptr<ParsedExpression>> posts;
	vector<Block> blocks;
};

} // namespace

//===--------------------------------------------------------------------===//
// Compiler
//===--------------------------------------------------------------------===//
SemanticQueryCompiler::SemanticQueryCompiler(const SemanticModel &model, ClientContext &context)
    : model(model), analyzer(model, context) {
}

SemanticQuerySpec SemanticQueryCompiler::Resolve(const vector<ParsedSemanticQueryItem> &items,
                                                 optional_ptr<ParsedExpression> where) {
	SemanticQuerySpec spec;
	SemanticResolveContext resolve_context;
	resolve_context.allow_private = false;
	for (auto &item : items) {
		if (!item.expression) {
			auto table = model.FindTable(item.star_table);
			if (!table.IsValid()) {
				throw BinderException("Unknown logical table \"%s\" in \"%s.*\"", item.star_table, item.star_table);
			}
			for (idx_t m = 0; m < model.members.size(); m++) {
				auto &member = model.members[m];
				if (member.table == table.GetIndex() && member.Kind() == item.kind && !member.IsPrivate()) {
					spec.items.push_back(SemanticQueryItem {item.kind, SemanticMarkers::Member(m), member.Name()});
				}
			}
			continue;
		}
		resolve_context.what = "Invalid " + StringUtil::Lower(SemanticMemberKindToString(item.kind)) + " \"" +
		                       item.expression->ToString() + "\"";
		auto resolved = model.Resolve(*item.expression, resolve_context);
		auto output_name = item.alias;
		if (output_name.empty()) {
			auto ref = SemanticMarkers::Classify(*resolved);
			output_name = ref.Is(SemanticRefKind::MEMBER)   ? model.members[ref.index].Name()
			              : ref.Is(SemanticRefKind::COLUMN) ? ref.column
			                                                : item.expression->ToString();
		}
		spec.items.push_back(SemanticQueryItem {item.kind, std::move(resolved), output_name});
	}
	if (where) {
		resolve_context.what = "Invalid WHERE clause";
		spec.where = model.Resolve(*where, resolve_context);
	}
	return spec;
}

unique_ptr<SelectStatement> SemanticQueryCompiler::Compile(const SemanticQuerySpec &spec) {
	vector<const SemanticQueryItem *> dimensions;
	vector<const SemanticQueryItem *> facts;
	vector<const SemanticQueryItem *> metrics;
	case_insensitive_set_t output_names;
	for (auto &item : spec.items) {
		if (!output_names.insert(item.output_name).second) {
			throw BinderException("Duplicate output column name \"%s\" in SEMANTIC_VIEW; use an alias",
			                      item.output_name);
		}
		auto aggregates = analyzer.ContainsAggregateOrMetric(*item.resolved);
		if (item.kind == SemanticMemberKind::METRIC && !aggregates) {
			throw BinderException("%s: a METRICS expression must aggregate (use a metric or an aggregate function); "
			                      "use DIMENSIONS or FACTS for row-level values",
			                      ItemWhat(item));
		}
		if (item.kind != SemanticMemberKind::METRIC && aggregates) {
			throw BinderException("%s: metrics and aggregates are only allowed in METRICS", ItemWhat(item));
		}
		auto &list = item.kind == SemanticMemberKind::METRIC      ? metrics
		             : item.kind == SemanticMemberKind::DIMENSION ? dimensions
		                                                          : facts;
		list.push_back(&item);
	}
	if (!facts.empty() && !metrics.empty()) {
		throw BinderException("Cannot specify FACTS and METRICS in the same SEMANTIC_VIEW clause");
	}
	if (spec.where && analyzer.ContainsAggregateOrMetric(*spec.where)) {
		throw BinderException("The WHERE clause of SEMANTIC_VIEW can only reference dimensions and facts");
	}
	// row-level tables referenced by dimensions / facts / WHERE
	set<idx_t> row_tables;
	for (auto &item : spec.items) {
		if (item.kind != SemanticMemberKind::METRIC) {
			analyzer.RowTables(*item.resolved, DConstants::INVALID_INDEX, row_tables, ItemWhat(item));
		}
	}
	if (spec.where) {
		analyzer.RowTables(*spec.where, DConstants::INVALID_INDEX, row_tables, "Invalid WHERE clause");
	}

	SemanticAliasGenerator aliases;
	SemanticExpressionCompiler compiler(model, analyzer);
	if (!metrics.empty()) {
		return MetricQueryCompiler(model, analyzer, compiler, aliases, spec, dimensions, metrics).Compile();
	}
	return CompileRowQuery(compiler, aliases, spec, !dimensions.empty(), !facts.empty(), row_tables);
}

//! Dimensions and/or facts: rows of the finest referenced table (distinct dimension values, or facts per row /
//! per dimension group)
unique_ptr<SelectStatement> SemanticQueryCompiler::CompileRowQuery(SemanticExpressionCompiler &compiler,
                                                                   SemanticAliasGenerator &aliases,
                                                                   const SemanticQuerySpec &spec, bool has_dimensions,
                                                                   bool has_facts, const set<idx_t> &row_tables) {
	auto base = DConstants::INVALID_INDEX;
	if (!has_facts && !row_tables.empty()) {
		// no referenced table reaches the others: dimensions related through a many-to-many bridge
		bool related = false;
		for (auto candidate : row_tables) {
			bool reaches_all = true;
			for (auto other : row_tables) {
				reaches_all = reaches_all && model.Reaches(candidate, other);
			}
			related = related || reaches_all;
		}
		if (!related) {
			base = SemanticBridgeResolver(model).CommonBase(row_tables);
		}
	}
	if (base == DConstants::INVALID_INDEX) {
		base = analyzer.FinestTable(row_tables, "Invalid SEMANTIC_VIEW query");
	}
	if (base == DConstants::INVALID_INDEX) {
		throw BinderException("SEMANTIC_VIEW must reference at least one dimension or fact");
	}
	if (has_facts && has_dimensions && row_tables.size() > 1) {
		throw BinderException("When FACTS and DIMENSIONS are both specified, all facts and dimensions (including "
		                      "those in the WHERE clause) must be defined in the same logical table");
	}
	SemanticJoinScope scope(model, aliases, base, {});
	auto node = make_uniq<SelectNode>();
	for (auto &item : spec.items) {
		auto expr = compiler.Row(*item.resolved, scope, DConstants::INVALID_INDEX, ItemWhat(item));
		if (has_facts && item.kind == SemanticMemberKind::DIMENSION) {
			node->groups.group_expressions.push_back(expr->Copy());
		}
		if (has_dimensions && item.kind == SemanticMemberKind::FACT) {
			vector<unique_ptr<ParsedExpression>> children;
			children.push_back(std::move(expr));
			expr = make_uniq<FunctionExpression>("any_value", std::move(children));
		}
		expr->alias = item.output_name;
		node->select_list.push_back(std::move(expr));
	}
	GroupByAll(*node);
	if (!has_facts) {
		node->modifiers.push_back(make_uniq<DistinctModifier>());
	}
	if (spec.where) {
		node->where_clause = compiler.Row(*spec.where, scope, DConstants::INVALID_INDEX, "Invalid WHERE clause");
	}
	node->from_table = scope.BuildFrom();
	return MakeSelectStatement(std::move(node));
}

unique_ptr<SelectStatement> SemanticQueryCompiler::CompileMemberProbe(idx_t member, idx_t &column) {
	auto &entry = model.members[member];
	SemanticQuerySpec spec;
	if (entry.Kind() == SemanticMemberKind::METRIC) {
		// window-function metrics can only be evaluated together with their window dimensions
		set<idx_t> window_dimensions;
		analyzer.WindowDimensions(*SemanticMarkers::Member(member), window_dimensions);
		for (auto dimension : window_dimensions) {
			spec.items.push_back(SemanticQueryItem {SemanticMemberKind::DIMENSION, SemanticMarkers::Member(dimension),
			                                        "__sv_probe_d" + to_string(dimension)});
		}
	}
	auto kind = entry.Kind() == SemanticMemberKind::METRIC ? SemanticMemberKind::METRIC : SemanticMemberKind::FACT;
	spec.items.push_back(SemanticQueryItem {kind, SemanticMarkers::Member(member), entry.Name()});
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
			member.type = binder->Bind(statement->Cast<SQLStatement>()).types[column];
		} catch (std::exception &ex) {
			ErrorData error(ex);
			throw BinderException("Invalid semantic view \"%s\": %s \"%s\": %s", model.Name().name,
			                      SemanticModel::KindDisplayName(member.Kind()), model.MemberDisplayName(m),
			                      error.RawMessage());
		}
	}
}

} // namespace duckdb
