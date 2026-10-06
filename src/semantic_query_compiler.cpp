#include "semantic_definition_exception.hpp"
#include "semantic_query_compiler.hpp"

#include "duckdb/common/error_data.hpp"
#include "duckdb/common/exception.hpp"
#include "duckdb/common/exception/binder_exception.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/common/to_string.hpp"
#include "duckdb/parser/expression/case_expression.hpp"
#include "duckdb/parser/expression/constant_expression.hpp"
#include "duckdb/parser/expression/star_expression.hpp"
#include "duckdb/parser/expression/function_expression.hpp"
#include "duckdb/parser/expression/window_expression.hpp"
#include "duckdb/parser/parsed_expression_iterator.hpp"
#include "duckdb/parser/query_node/set_operation_node.hpp"
#include "duckdb/parser/result_modifier.hpp"
#include "duckdb/parser/tableref/joinref.hpp"
#include "duckdb/planner/binder.hpp"
#include "duckdb/planner/operator/logical_get.hpp"
#include "duckdb/catalog/catalog.hpp"
#include "duckdb/catalog/catalog_entry/table_catalog_entry.hpp"
#include "duckdb/catalog/catalog_entry/schema_catalog_entry.hpp"
#include "semantic_ast_helpers.hpp"
#include "semantic_expression_compiler.hpp"
#include "semantic_bridge.hpp"
#include "semantic_cumulative.hpp"
#include "semantic_join_scope.hpp"
#include "semantic_time_reads.hpp"

namespace duckdb {

namespace {

string DimensionColumn(idx_t index) {
	return SemanticDimensionColumn(index);
}

string AtomColumn(idx_t index) {
	return "__sv_a" + to_string(index);
}

constexpr const char *BLOCK_COLUMN = "__sv_block";

} // namespace

string SemanticItemWhat(const SemanticQueryItem &item) {
	switch (item.kind) {
	case SemanticMemberKind::DIMENSION:
		return "Invalid dimension \"" + item.output_name + "\"";
	case SemanticMemberKind::FACT:
		return "Invalid fact \"" + item.output_name + "\"";
	default:
		return "Invalid metric \"" + item.output_name + "\"";
	}
}

string SemanticDimensionColumn(idx_t dimension) {
	return "__sv_d" + to_string(dimension);
}

//===--------------------------------------------------------------------===//
// Expressions
//===--------------------------------------------------------------------===//

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
		throw SemanticDefinitionException(
		    "Invalid semantic view query: Dimension '%s' used in a window function metric must be "
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
	for (auto &order : window.arg_orders) {
		map_key(order.expression);
	}
	ParsedExpressionIterator::EnumerateChildren(
	    *expr, [&](unique_ptr<ParsedExpression> &child) { MapWindowDimensions(model, child, dimensions); });
}

//===--------------------------------------------------------------------===//
// Metric queries
//===--------------------------------------------------------------------===//
//! Atoms aggregated together: equal block keys
struct Block {
	SemanticBlockKey key;
	vector<idx_t> atoms;

	bool Owns(idx_t atom) const {
		return std::find(atoms.begin(), atoms.end(), atom) != atoms.end();
	}
};

//! The many-to-many bridges of one aggregation block. Row-level items (dimensions, WHERE conjuncts) that the home table
//! does not reach come from one de-duplicated mapping subquery per bridge,
//!   SELECT DISTINCT <anchor key>, <bridged dimensions> FROM <bridge> [joins] WHERE <bridged conjuncts>
//! joined to the block's rows on the anchor key: each home row joins at most once per combination of bridged dimension
//! values, so nothing is counted twice within a group.
class BlockBridges {
public:
	BlockBridges(const SemanticModel &model, SemanticAnalyzer &analyzer, SemanticExpressionCompiler &compiler,
	             SemanticAliasGenerator &aliases, const SemanticBlockKey &block,
	             optional_ptr<const SemanticTableSources> sources)
	    : model(model), analyzer(analyzer), compiler(compiler), aliases(aliases), resolver(model), block(block),
	      sources(sources) {
	}

	//! The reference, in the block's rows, to dimension `expr` (output column `column`) when it is reached through a
	//! bridge; nullptr when the home table reaches it
	unique_ptr<ParsedExpression> Dimension(const ParsedExpression &expr, const string &column, const string &what) {
		auto group = Place(expr, what, string());
		if (!group) {
			return nullptr;
		}
		group->dimensions.push_back(BridgedDimension {&expr, column, what});
		return ColumnRef(column, group->alias);
	}

	//! Whether WHERE conjunct `expr` is reached through a bridge: it then filters the bridge's mapping subquery,
	//! which becomes a semi-join
	bool Condition(const ParsedExpression &expr) {
		auto group = Place(expr, WHERE_WHAT, "; split it into separate conditions combined with AND");
		if (!group) {
			return false;
		}
		group->conditions.push_back(&expr);
		return true;
	}

	//! The block's FROM clause: `scope` joined with the mapping subquery of every bridge
	unique_ptr<TableRef> Join(SemanticJoinScope &scope) {
		// the anchor keys join their tables into `scope`, so they come before its FROM clause is built
		vector<unique_ptr<ParsedExpression>> conditions;
		for (auto &group : groups) {
			vector<unique_ptr<ParsedExpression>> keys;
			for (idx_t c = 0; c < group.bridge.anchor_key.size(); c++) {
				keys.push_back(Compare(ExpressionType::COMPARE_EQUAL,
				                       scope.ColumnReference(group.bridge.anchor, group.bridge.anchor_key[c]),
				                       ColumnRef(KeyColumn(c), group.alias)));
			}
			conditions.push_back(AndAll(std::move(keys)));
		}
		auto result = scope.BuildFrom();
		for (idx_t g = 0; g < groups.size(); g++) {
			auto join = make_uniq<JoinRef>(JoinRefType::REGULAR);
			// home rows without a bridge row keep NULL bridged dimensions, unless bridged conditions filter them out
			join->type = groups[g].conditions.empty() ? JoinType::LEFT : JoinType::INNER;
			join->left = std::move(result);
			join->right = MakeSubqueryRef(MappingQuery(groups[g]), groups[g].alias);
			join->condition = std::move(conditions[g]);
			result = std::move(join);
		}
		return result;
	}

private:
	static constexpr const char *WHERE_WHAT = "Invalid WHERE clause";

	struct BridgedDimension {
		const ParsedExpression *expr;
		string column;
		string what;
	};
	//! Items reached through the same bridge, read from one mapping subquery
	struct Group {
		SemanticBridge bridge;
		string alias;
		vector<BridgedDimension> dimensions;
		vector<const ParsedExpression *> conditions;
	};

	static string KeyColumn(idx_t index) {
		return "__sv_k" + to_string(index);
	}

	//! The group of the bridge `expr` is reached through; nullptr when the home table reaches it
	optional_ptr<Group> Place(const ParsedExpression &expr, const string &what, const string &hint) {
		set<idx_t> tables;
		analyzer.RowTables(expr, DConstants::INVALID_INDEX, tables, what);
		bool local = true;
		for (auto table : tables) {
			local = local && model.Reaches(block.home, table);
		}
		if (local) {
			return nullptr;
		}
		auto bridge = resolver.Resolve(block.home, tables, block.using_relationships, what, hint);
		for (auto &group : groups) {
			if (group.bridge.SameAs(bridge)) {
				return &group;
			}
		}
		groups.push_back(Group {std::move(bridge), aliases.NestedPrefix() + "bridge", {}, {}});
		return &groups.back();
	}

	unique_ptr<QueryNode> MappingQuery(const Group &group) {
		auto &bridge = group.bridge;
		SemanticJoinScope scope(model, aliases, bridge.bridge, block.using_relationships, aliases.NestedPrefix(),
		                        sources);
		auto node = make_uniq<SelectNode>();
		for (idx_t c = 0; c < bridge.key_source_columns.size(); c++) {
			auto key = scope.ColumnReference(bridge.key_source, bridge.key_source_columns[c]);
			key->alias = KeyColumn(c);
			node->select_list.push_back(std::move(key));
		}
		for (auto &dimension : group.dimensions) {
			auto expr = compiler.ShiftedRow(*dimension.expr, scope, dimension.what, block.offsets);
			expr->alias = dimension.column;
			node->select_list.push_back(std::move(expr));
		}
		vector<unique_ptr<ParsedExpression>> conditions;
		for (auto *condition : group.conditions) {
			conditions.push_back(compiler.ShiftedRow(*condition, scope, WHERE_WHAT, block.offsets));
		}
		node->where_clause = AndAll(std::move(conditions));
		node->modifiers.push_back(make_uniq<DistinctModifier>());
		node->from_table = scope.BuildFrom();
		return std::move(node);
	}

	const SemanticModel &model;
	SemanticAnalyzer &analyzer;
	SemanticExpressionCompiler &compiler;
	SemanticAliasGenerator &aliases;
	SemanticBridgeResolver resolver;
	//! Bridged dimensions and conditions are shifted (OFFSET) like the others
	const SemanticBlockKey &block;
	optional_ptr<const SemanticTableSources> sources;
	vector<Group> groups;
};

class MetricQueryCompiler {
public:
	MetricQueryCompiler(const SemanticModel &model, SemanticAnalyzer &analyzer, SemanticExpressionCompiler &compiler,
	                    SemanticAliasGenerator &aliases, const SemanticQuerySpec &spec,
	                    const vector<const SemanticQueryItem *> &dimensions,
	                    const vector<const SemanticQueryItem *> &metrics, const SemanticCompileOptions &options)
	    : model(model), analyzer(analyzer), compiler(compiler), aliases(aliases), spec(spec), dimensions(dimensions),
	      options(options) {
		for (auto *item : metrics) {
			posts.push_back(
			    analyzer.Decompose(*item->resolved, SemanticMetricContext(), atoms, SemanticItemWhat(*item)));
		}
		for (idx_t a = 0; a < atoms.size(); a++) {
			auto key = atoms[a].BlockKey();
			auto block = std::find_if(blocks.begin(), blocks.end(), [&](const Block &b) { return b.key == key; });
			if (block == blocks.end()) {
				blocks.push_back(Block {std::move(key), {}});
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

	const vector<SemanticAtom> &Atoms() const {
		return atoms;
	}
	idx_t BlockCount() const {
		return blocks.size();
	}
	SemanticBlockPlan Plan(idx_t b) const {
		auto &block = blocks[b];
		SemanticBlockPlan result;
		result.key = block.key;
		for (auto a : block.atoms) {
			result.atoms.push_back(atoms[a].Copy());
		}
		return result;
	}
	//! The block as aggregated from its sources (dimensions, then its atoms)
	unique_ptr<SelectNode> SourceBlock(idx_t b, SemanticBlockDependencies &dependencies) {
		return BuildSourceBlock(blocks[b], optional_idx(), &dependencies);
	}

	//! SELECT <dimensions>, count(*) AS __sv_rows, <states of the block's atoms> ... GROUP BY <dimensions>
	unique_ptr<SelectNode> BuildStateBlock(idx_t b, const SemanticStateBuilder &states) {
		auto &block = blocks[b];
		SemanticJoinScope scope(model, aliases, block.key.home, block.key.using_relationships, string(),
		                        options.sources);
		BlockBridges bridges(model, analyzer, compiler, aliases, block.key, options.sources);
		auto parts = BuildDimensionRows(block, scope, bridges);
		auto aggregate = make_uniq<SelectNode>();
		for (idx_t d = 0; d < dimensions.size(); d++) {
			aggregate->select_list.push_back(ColumnRef(DimensionColumn(d)));
			aggregate->groups.group_expressions.push_back(ColumnRef(DimensionColumn(d)));
		}
		auto rows_count = make_uniq<FunctionExpression>("count_star", vector<unique_ptr<ParsedExpression>>());
		rows_count->alias = MATERIALIZED_ROWS_COLUMN;
		aggregate->select_list.push_back(std::move(rows_count));
		for (idx_t k = 0; k < block.atoms.size(); k++) {
			for (auto &state : states(k, parts.aggregates[k]->Cast<FunctionExpression>())) {
				aggregate->select_list.push_back(std::move(state));
			}
		}
		FinishRows(*parts.rows, scope, bridges);
		GroupByAll(*aggregate);
		aggregate->from_table = MakeSubqueryRef(std::move(parts.rows), "__sv_rows");
		return aggregate;
	}

private:
	static constexpr const char *WHERE_WHAT = "Invalid WHERE clause";

	//! The rows of a block: the arguments of its aggregates, filtered by conjuncts of the query's WHERE (shifted by the
	//! block's OFFSETs), the block's row filters and the state query's row filter. `dimensions` holds the query's
	//! dimensions, compiled over the rows' scope through a column map; `aggregates` the compiled aggregate of each atom
	//! of the block, reading the rows.
	struct BlockRows {
		unique_ptr<SelectNode> rows;
		vector<unique_ptr<ParsedExpression>> dimensions;
		vector<unique_ptr<ParsedExpression>> aggregates;
	};

	//! The conjuncts of the query's WHERE
	vector<const ParsedExpression *> WhereConjuncts() const {
		vector<const ParsedExpression *> result;
		if (spec.where) {
			SplitConjuncts(*spec.where, result);
		}
		return result;
	}

	BlockRows BuildRows(const Block &block, SemanticJoinScope &scope, BlockBridges &bridges, SemanticColumnMap &map,
	                    const vector<const ParsedExpression *> &where) {
		BlockRows result;
		result.rows = make_uniq<SelectNode>();
		auto &rows = *result.rows;
		auto dependencies = scope.Dependencies();
		if (dependencies) {
			dependencies->in_dimension = true;
		}
		for (idx_t d = 0; d < dimensions.size(); d++) {
			auto &resolved = *dimensions[d]->resolved;
			auto what = SemanticItemWhat(*dimensions[d]);
			auto bridged = bridges.Dimension(resolved, DimensionColumn(d), what);
			if (bridged && dependencies) {
				throw SemanticDefinitionException("%s: many-to-many bridges cannot be materialized", what);
			}
			result.dimensions.push_back(bridged ? map.Opaque(std::move(bridged))
			                                    : compiler.MappedRow(resolved, scope, what, map));
		}
		if (dependencies) {
			dependencies->in_dimension = false;
		}
		// the aggregates read their arguments, FILTERs and ORDER BYs from the rows, as __sv_x<n>
		idx_t argument_count = 0;
		auto extract = [&](unique_ptr<ParsedExpression> &argument) {
			auto name = "__sv_x" + to_string(argument_count++);
			argument->alias = name;
			rows.select_list.push_back(std::move(argument));
			argument = ColumnRef(name);
		};
		for (auto a : block.atoms) {
			auto expr = compiler.Aggregate(atoms[a], scope, "Invalid metric");
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
			result.aggregates.push_back(std::move(expr));
		}
		vector<unique_ptr<ParsedExpression>> conditions;
		vector<unique_ptr<ParsedExpression>> local_conjuncts;
		for (auto *conjunct : where) {
			if (!bridges.Condition(*conjunct)) {
				local_conjuncts.push_back(conjunct->Copy());
			}
		}
		if (!local_conjuncts.empty()) {
			conditions.push_back(
			    compiler.ShiftedRow(*AndAll(std::move(local_conjuncts)), scope, WHERE_WHAT, block.key.offsets));
		}
		for (auto metric : block.key.row_filters) {
			conditions.push_back(
			    compiler.Row(*model.members[metric].filter, scope, DConstants::INVALID_INDEX, "Invalid metric"));
		}
		if (options.row_filter) {
			auto filter = options.row_filter(scope, result.dimensions);
			if (filter) {
				conditions.push_back(std::move(filter));
			}
		}
		rows.where_clause = AndAll(std::move(conditions));
		return result;
	}

	//! The rows of a block that groups them by the query's dimensions: those shifted by the block's OFFSETs, and
	//! read by the rows as __sv_d<i>
	BlockRows BuildDimensionRows(const Block &block, SemanticJoinScope &scope, BlockBridges &bridges) {
		SemanticOffsetMap map(model, block.key.offsets);
		auto result = BuildRows(block, scope, bridges, map, WhereConjuncts());
		vector<unique_ptr<ParsedExpression>> select_list;
		for (idx_t d = 0; d < result.dimensions.size(); d++) {
			select_list.push_back(result.dimensions[d]->Copy());
			select_list.back()->alias = DimensionColumn(d);
		}
		for (auto &expr : result.rows->select_list) {
			select_list.push_back(std::move(expr));
		}
		result.rows->select_list = std::move(select_list);
		return result;
	}

	//! Completes the rows once every expression is compiled (the scope's joins are then known)
	static void FinishRows(SelectNode &rows, SemanticJoinScope &scope, BlockBridges &bridges) {
		if (rows.select_list.empty()) {
			// e.g. only COUNT(*): the rows still have to be produced
			rows.select_list.push_back(make_uniq<ConstantExpression>(Value::BOOLEAN(true)));
		}
		rows.from_table = bridges.Join(scope);
	}

	//! A block: an inner select produces the dimensions and the aggregate arguments for the rows of the home table
	//! (optionally restricted to the last snapshot); the outer select aggregates them by dimension. With `tag`,
	//! every atom of the query gets a column (NULL when not owned) plus the block tag, for the UNION ALL merge.
	unique_ptr<SelectNode> BuildBlock(const Block &block, optional_idx tag) {
		if (block.key.cumulative != DConstants::INVALID_INDEX) {
			return BuildCumulativeBlock(block, tag);
		}
		if (options.materializations) {
			vector<const ParsedExpression *> dimension_exprs;
			for (auto *dimension : dimensions) {
				dimension_exprs.push_back(dimension->resolved.get());
			}
			auto read =
			    options.materializations->Find(block.key, block.atoms, atoms, dimension_exprs, spec.where.get());
			if (read) {
				return BuildMaterializedBlock(block, tag, *read);
			}
		}
		return BuildSourceBlock(block, tag);
	}

	unique_ptr<SelectNode> BuildSourceBlock(const Block &block, optional_idx tag,
	                                        optional_ptr<SemanticBlockDependencies> dependencies = nullptr) {
		SemanticJoinScope scope(model, aliases, block.key.home, block.key.using_relationships, string(),
		                        options.sources, dependencies);
		BlockBridges bridges(model, analyzer, compiler, aliases, block.key, options.sources);
		auto parts = BuildDimensionRows(block, scope, bridges);
		auto aggregate = make_uniq<SelectNode>();
		for (idx_t d = 0; d < dimensions.size(); d++) {
			aggregate->select_list.push_back(ColumnRef(DimensionColumn(d)));
			aggregate->groups.group_expressions.push_back(ColumnRef(DimensionColumn(d)));
		}
		AddAtomColumns(*aggregate, block, std::move(parts.aggregates), tag);
		if (block.key.non_additive_member != DConstants::INVALID_INDEX) {
			aggregate->where_clause = LastSnapshotFilter(model.members[block.key.non_additive_member], scope,
			                                             std::move(parts.dimensions), *parts.rows);
		}
		FinishRows(*parts.rows, scope, bridges);
		GroupByAll(*aggregate);
		aggregate->from_table = MakeSubqueryRef(std::move(parts.rows), "__sv_rows");
		return aggregate;
	}

	//! The block computed from a materialization's stored state
	unique_ptr<SelectNode> BuildMaterializedBlock(const Block &block, optional_idx tag,
	                                              SemanticMaterializedRead &read) {
		auto node = make_uniq<SelectNode>();
		for (idx_t d = 0; d < dimensions.size(); d++) {
			node->groups.group_expressions.push_back(read.dimensions[d]->Copy());
			read.dimensions[d]->alias = DimensionColumn(d);
			node->select_list.push_back(std::move(read.dimensions[d]));
		}
		AddAtomColumns(*node, block, std::move(read.aggregates), tag);
		node->where_clause = std::move(read.where);
		node->from_table = std::move(read.table);
		GroupByAll(*node);
		return node;
	}

	//! A block of CUMULATIVE aggregates (semantic_cumulative.hpp): its rows read the stored date, the query's
	//! dimensions and WHERE conditions on the date read the anchor
	unique_ptr<SelectNode> BuildCumulativeBlock(const Block &block, optional_idx tag) {
		auto &cumulative = model.cumulatives[block.key.cumulative];
		SemanticJoinScope scope(model, aliases, block.key.home, block.key.using_relationships, string(),
		                        options.sources);
		BlockBridges bridges(model, analyzer, compiler, aliases, block.key, options.sources);
		SemanticAnchorMap anchor_map(model, cumulative, block.key.offsets);
		// the WHERE conjuncts reading the date filter the anchors, the others the rows
		SemanticCumulativeParts parts;
		vector<const ParsedExpression *> row_where;
		vector<unique_ptr<ParsedExpression>> anchor_where;
		for (auto *conjunct : WhereConjuncts()) {
			if (SemanticReadsColumn(model, analyzer, *conjunct, cumulative.table, cumulative.column)) {
				anchor_where.push_back(compiler.MappedRow(*conjunct, scope, WHERE_WHAT, anchor_map));
			} else {
				row_where.push_back(conjunct);
			}
		}
		auto rows = BuildRows(block, scope, bridges, anchor_map, row_where);
		parts.date = scope.ColumnReference(cumulative.table, cumulative.column);
		FinishRows(*rows.rows, scope, bridges);
		parts.rows = std::move(rows.rows);
		parts.leaves = anchor_map.TakeLeaves();
		parts.dimensions = std::move(rows.dimensions);
		parts.anchor_where = AndAll(std::move(anchor_where));
		parts.aggregates = std::move(rows.aggregates);
		auto result = BuildSemanticCumulativeBlock(analyzer.Context(), model, aliases, options.sources, cumulative,
		                                           std::move(parts));
		AddAtomColumns(*result.node, block, std::move(result.values), tag);
		return std::move(result.node);
	}

	//! Selects every atom of `block` (`owned`, in block order) as its atom column; merged with other blocks (`tag`),
	//! every other atom of the query as NULL, and the block tag
	void AddAtomColumns(SelectNode &node, const Block &block, vector<unique_ptr<ParsedExpression>> owned,
	                    optional_idx tag) {
		idx_t next = 0;
		for (idx_t a = 0; a < atoms.size(); a++) {
			unique_ptr<ParsedExpression> expr;
			if (block.Owns(a)) {
				expr = std::move(owned[next++]);
			} else if (tag.IsValid()) {
				expr = make_uniq<ConstantExpression>(Value());
			} else {
				continue;
			}
			expr->alias = AtomColumn(a);
			node.select_list.push_back(std::move(expr));
		}
		if (tag.IsValid()) {
			auto tag_value = make_uniq<ConstantExpression>(Value::BIGINT(NumericCast<int64_t>(tag.GetIndex())));
			tag_value->alias = BLOCK_COLUMN;
			node.select_list.push_back(std::move(tag_value));
		}
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
	const SemanticCompileOptions &options;
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
				throw SemanticDefinitionException("Unknown logical table \"%s\" in \"%s.*\"", item.star_table,
				                                  item.star_table);
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

void SemanticQueryCompiler::SplitItems(const SemanticQuerySpec &spec, vector<const SemanticQueryItem *> &dimensions,
                                       vector<const SemanticQueryItem *> &facts,
                                       vector<const SemanticQueryItem *> &metrics, set<idx_t> &row_tables) {
	case_insensitive_set_t output_names;
	for (auto &item : spec.items) {
		if (!output_names.insert(item.output_name).second) {
			throw SemanticDefinitionException("Duplicate output column name \"%s\" in SEMANTIC_VIEW; use an alias",
			                                  item.output_name);
		}
		auto aggregates = analyzer.ContainsAggregateOrMetric(*item.resolved);
		if (item.kind == SemanticMemberKind::METRIC && !aggregates) {
			throw SemanticDefinitionException(
			    "%s: a METRICS expression must aggregate (use a metric or an aggregate function); "
			    "use DIMENSIONS or FACTS for row-level values",
			    SemanticItemWhat(item));
		}
		if (item.kind != SemanticMemberKind::METRIC && aggregates) {
			throw SemanticDefinitionException("%s: metrics and aggregates are only allowed in METRICS",
			                                  SemanticItemWhat(item));
		}
		auto &list = item.kind == SemanticMemberKind::METRIC      ? metrics
		             : item.kind == SemanticMemberKind::DIMENSION ? dimensions
		                                                          : facts;
		list.push_back(&item);
	}
	if (!facts.empty() && !metrics.empty()) {
		throw SemanticDefinitionException("Cannot specify FACTS and METRICS in the same SEMANTIC_VIEW clause");
	}
	if (spec.where && analyzer.ContainsAggregateOrMetric(*spec.where)) {
		throw SemanticDefinitionException("The WHERE clause of SEMANTIC_VIEW can only reference dimensions and facts");
	}
	// row-level tables referenced by dimensions / facts / WHERE
	for (auto &item : spec.items) {
		if (item.kind != SemanticMemberKind::METRIC) {
			analyzer.RowTables(*item.resolved, DConstants::INVALID_INDEX, row_tables, SemanticItemWhat(item));
		}
	}
	if (spec.where) {
		analyzer.RowTables(*spec.where, DConstants::INVALID_INDEX, row_tables, "Invalid WHERE clause");
	}
}

unique_ptr<SelectStatement> SemanticQueryCompiler::Compile(const SemanticQuerySpec &spec,
                                                           const SemanticCompileOptions &options) {
	vector<const SemanticQueryItem *> dimensions;
	vector<const SemanticQueryItem *> facts;
	vector<const SemanticQueryItem *> metrics;
	set<idx_t> row_tables;
	SplitItems(spec, dimensions, facts, metrics, row_tables);
	SemanticAliasGenerator aliases;
	SemanticExpressionCompiler compiler(model, analyzer);
	if (!metrics.empty()) {
		return MetricQueryCompiler(model, analyzer, compiler, aliases, spec, dimensions, metrics, options).Compile();
	}
	return CompileRowQuery(compiler, aliases, spec, !dimensions.empty(), !facts.empty(), row_tables, options);
}

//! Binding can introduce reads through macros and views that were absent from the compiled AST.
//! Match every physical scan occurrence against the sources emitted by the semantic compiler.
static void VerifyBoundSources(const SemanticModel &model, LogicalOperator &op, map<idx_t, idx_t> &remaining,
                               SemanticBlockDependencies &dependencies) {
	if (op.type == LogicalOperatorType::LOGICAL_GET) {
		auto table = op.Cast<LogicalGet>().GetTable();
		bool accounted = false;
		if (table) {
			for (auto &entry : remaining) {
				auto &source = model.tables[entry.first];
				if (entry.second && source.catalog_name == table->ParentCatalog().GetName() &&
				    source.schema_name == table->ParentSchema().name && source.table_name == table->name) {
					entry.second--;
					accounted = true;
					break;
				}
			}
		}
		if (!accounted) {
			dependencies.complete = false;
			dependencies.Unsupported("binding introduces source reads outside the compiled semantic dependencies");
		}
	}
	for (auto &child : op.children) {
		VerifyBoundSources(model, *child, remaining, dependencies);
	}
}

vector<SemanticBlockPlan> SemanticQueryCompiler::PlanBlocks(const SemanticQuerySpec &spec) {
	vector<const SemanticQueryItem *> dimensions;
	vector<const SemanticQueryItem *> facts;
	vector<const SemanticQueryItem *> metrics;
	set<idx_t> row_tables;
	SplitItems(spec, dimensions, facts, metrics, row_tables);
	SemanticAliasGenerator aliases;
	SemanticExpressionCompiler compiler(model, analyzer);
	SemanticCompileOptions options;
	MetricQueryCompiler metric_compiler(model, analyzer, compiler, aliases, spec, dimensions, metrics, options);
	vector<SemanticBlockPlan> result;
	for (idx_t b = 0; b < metric_compiler.BlockCount(); b++) {
		auto plan = metric_compiler.Plan(b);
		// the atom types: bind the block as aggregated from its sources (dimensions come first)
		auto node = make_uniq<SelectNode>();
		node->select_list.push_back(make_uniq<StarExpression>());
		node->from_table = MakeSubqueryRef(metric_compiler.SourceBlock(b, plan.dependencies), "__sv_probe");
		auto limit = make_uniq<LimitModifier>();
		limit->limit = make_uniq<ConstantExpression>(Value::BIGINT(0));
		node->modifiers.push_back(std::move(limit));
		auto statement = MakeSelectStatement(std::move(node));
		auto binder = Binder::CreateBinder(analyzer.Context());
		auto bound = binder->Bind(statement->Cast<SQLStatement>());
		auto remaining = plan.dependencies.scans;
		VerifyBoundSources(model, *bound.plan, remaining, plan.dependencies);
		auto &types = bound.types;
		for (idx_t k = 0; k < plan.atoms.size(); k++) {
			plan.atom_types.push_back(types[dimensions.size() + k]);
		}
		result.push_back(std::move(plan));
	}
	return result;
}

unique_ptr<SelectStatement> SemanticQueryCompiler::CompileStates(const SemanticQuerySpec &spec, idx_t block,
                                                                 const SemanticStateBuilder &states,
                                                                 const SemanticCompileOptions &options) {
	vector<const SemanticQueryItem *> dimensions;
	vector<const SemanticQueryItem *> facts;
	vector<const SemanticQueryItem *> metrics;
	set<idx_t> row_tables;
	SplitItems(spec, dimensions, facts, metrics, row_tables);
	SemanticAliasGenerator aliases;
	SemanticExpressionCompiler compiler(model, analyzer);
	MetricQueryCompiler metric_compiler(model, analyzer, compiler, aliases, spec, dimensions, metrics, options);
	return MakeSelectStatement(metric_compiler.BuildStateBlock(block, states));
}

//! Dimensions and/or facts: rows of the finest referenced table (distinct dimension values, or facts per row /
//! per dimension group)
unique_ptr<SelectStatement> SemanticQueryCompiler::CompileRowQuery(SemanticExpressionCompiler &compiler,
                                                                   SemanticAliasGenerator &aliases,
                                                                   const SemanticQuerySpec &spec, bool has_dimensions,
                                                                   bool has_facts, const set<idx_t> &row_tables,
                                                                   const SemanticCompileOptions &options) {
	// the finest referenced table, else (dimensions only) a bridge relating them, else the "not related" error
	auto finest = model.FinestTable(row_tables);
	auto base = finest.IsValid() ? finest.GetIndex() : DConstants::INVALID_INDEX;
	if (base == DConstants::INVALID_INDEX && !has_facts && !row_tables.empty()) {
		base = SemanticBridgeResolver(model).CommonBase(row_tables);
	}
	if (base == DConstants::INVALID_INDEX) {
		base = analyzer.FinestTable(row_tables, "Invalid SEMANTIC_VIEW query");
	}
	if (base == DConstants::INVALID_INDEX) {
		throw SemanticDefinitionException("SEMANTIC_VIEW must reference at least one dimension or fact");
	}
	if (has_facts && has_dimensions && row_tables.size() > 1) {
		throw SemanticDefinitionException(
		    "When FACTS and DIMENSIONS are both specified, all facts and dimensions (including "
		    "those in the WHERE clause) must be defined in the same logical table");
	}
	SemanticJoinScope scope(model, aliases, base, {}, string(), options.sources);
	auto node = make_uniq<SelectNode>();
	for (auto &item : spec.items) {
		auto expr = compiler.Row(*item.resolved, scope, DConstants::INVALID_INDEX, SemanticItemWhat(item));
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
			throw SemanticDefinitionException("Invalid semantic view \"%s\": %s \"%s\": %s", model.Name().name,
			                                  SemanticModel::KindDisplayName(member.Kind()), model.MemberDisplayName(m),
			                                  error.RawMessage());
		}
	}
}

} // namespace duckdb
