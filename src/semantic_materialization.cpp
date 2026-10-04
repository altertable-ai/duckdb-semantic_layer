#include "semantic_materialization.hpp"

#include "duckdb/catalog/catalog.hpp"
#include "duckdb/catalog/entry_lookup_info.hpp"
#include "duckdb/common/error_data.hpp"
#include "duckdb/common/exception/binder_exception.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/common/to_string.hpp"
#include "duckdb/common/types/timestamp.hpp"
#include "duckdb/main/client_context.hpp"
#include "duckdb/parser/expression/cast_expression.hpp"
#include "duckdb/parser/expression/constant_expression.hpp"
#include "duckdb/parser/expression/function_expression.hpp"
#include "duckdb/parser/expression/operator_expression.hpp"
#include "duckdb/parser/parsed_expression_iterator.hpp"
#include "duckdb/parser/tableref/basetableref.hpp"
#include "semantic_ast_helpers.hpp"
#include "semantic_change_tracking.hpp"
#include "semantic_markers.hpp"
#include "semantic_materialization_internal.hpp"

namespace duckdb {

//===--------------------------------------------------------------------===//
// Aggregate state
//===--------------------------------------------------------------------===//
string SemanticAggregateKindToString(SemanticAggregateKind kind) {
	switch (kind) {
	case SemanticAggregateKind::SUM:
		return "SUM";
	case SemanticAggregateKind::COUNT:
		return "COUNT";
	case SemanticAggregateKind::AVG:
		return "AVG";
	case SemanticAggregateKind::MIN:
		return "MIN";
	case SemanticAggregateKind::MAX:
		return "MAX";
	default:
		return "OPAQUE";
	}
}

SemanticAggregateKind ClassifySemanticAggregate(const ParsedExpression &expr, const LogicalType &type) {
	auto &function = expr.Cast<FunctionExpression>();
	if (function.distinct || function.export_state || (function.order_bys && !function.order_bys->orders.empty())) {
		return SemanticAggregateKind::OPAQUE;
	}
	auto name = StringUtil::Lower(function.function_name);
	if (name == "count_star" && function.children.empty()) {
		return SemanticAggregateKind::COUNT;
	}
	if (function.children.size() != 1) {
		return SemanticAggregateKind::OPAQUE;
	}
	if (name == "sum") {
		return SemanticAggregateKind::SUM;
	}
	if (name == "count") {
		return SemanticAggregateKind::COUNT;
	}
	if ((name == "avg" || name == "mean") && type.id() == LogicalTypeId::DOUBLE) {
		// sum / count reproduces the average of numbers (DOUBLE); averages of other types are stored as is
		return SemanticAggregateKind::AVG;
	}
	if (name == "min") {
		return SemanticAggregateKind::MIN;
	}
	if (name == "max") {
		return SemanticAggregateKind::MAX;
	}
	return SemanticAggregateKind::OPAQUE;
}

vector<string> SemanticMaterializedAtom::StateColumns() const {
	switch (kind) {
	case SemanticAggregateKind::SUM:
	case SemanticAggregateKind::AVG:
		return {prefix + "_sum", prefix + "_cnt"};
	case SemanticAggregateKind::COUNT:
		return {prefix + "_cnt"};
	case SemanticAggregateKind::MIN:
		return {prefix + "_min"};
	case SemanticAggregateKind::MAX:
		return {prefix + "_max"};
	default:
		return {prefix + "_val"};
	}
}

static unique_ptr<ParsedExpression> Call(const string &name, vector<unique_ptr<ParsedExpression>> children) {
	return make_uniq<FunctionExpression>(name, std::move(children));
}

static unique_ptr<ParsedExpression> Call(const string &name, unique_ptr<ParsedExpression> child) {
	vector<unique_ptr<ParsedExpression>> children;
	children.push_back(std::move(child));
	return Call(name, std::move(children));
}

vector<unique_ptr<ParsedExpression>> SemanticStateExpressions(const SemanticMaterializedAtom &atom,
                                                              const FunctionExpression &compiled) {
	// the same aggregate (and FILTER) over the same argument, decomposed into its state
	auto over_argument = [&](const string &name) {
		auto result = Call(name, compiled.children[0]->Copy());
		result->Cast<FunctionExpression>().filter = compiled.filter ? compiled.filter->Copy() : nullptr;
		return result;
	};
	vector<unique_ptr<ParsedExpression>> result;
	switch (atom.kind) {
	case SemanticAggregateKind::SUM:
	case SemanticAggregateKind::AVG:
		result.push_back(over_argument("sum"));
		result.push_back(over_argument("count"));
		break;
	default:
		result.push_back(compiled.Copy());
		break;
	}
	auto columns = atom.StateColumns();
	for (idx_t c = 0; c < result.size(); c++) {
		result[c]->alias = columns[c];
	}
	return result;
}

unique_ptr<ParsedExpression> SemanticRollupExpression(const SemanticMaterializedAtom &atom, const string &alias) {
	auto column = [&](const string &suffix) {
		return ColumnRef(atom.prefix + suffix, alias);
	};
	unique_ptr<ParsedExpression> result;
	switch (atom.kind) {
	case SemanticAggregateKind::SUM:
		result = Call("sum", column("_sum"));
		break;
	case SemanticAggregateKind::COUNT: {
		// COUNT over no rows is 0, SUM over no rows is NULL
		auto coalesce = make_uniq<OperatorExpression>(ExpressionType::OPERATOR_COALESCE);
		coalesce->children.push_back(Call("sum", column("_cnt")));
		coalesce->children.push_back(make_uniq<ConstantExpression>(Value::BIGINT(0)));
		result = std::move(coalesce);
		break;
	}
	case SemanticAggregateKind::AVG: {
		vector<unique_ptr<ParsedExpression>> count;
		count.push_back(Call("sum", column("_cnt")));
		count.push_back(make_uniq<ConstantExpression>(Value::BIGINT(0)));
		vector<unique_ptr<ParsedExpression>> division;
		division.push_back(make_uniq<CastExpression>(LogicalType::DOUBLE, Call("sum", column("_sum"))));
		division.push_back(Call("nullif", std::move(count)));
		result = make_uniq<FunctionExpression>("/", std::move(division), nullptr, nullptr, false, true);
		break;
	}
	case SemanticAggregateKind::MIN:
		result = Call("min", column("_min"));
		break;
	case SemanticAggregateKind::MAX:
		result = Call("max", column("_max"));
		break;
	default:
		result = Call("any_value", column("_val"));
		break;
	}
	return make_uniq<CastExpression>(atom.type, std::move(result));
}

//===--------------------------------------------------------------------===//
// Planning
//===--------------------------------------------------------------------===//
string SemanticMaterialization::TableName(idx_t block) const {
	return QuoteIdentifier(state->catalog) + "." + QuoteIdentifier(state->schema) + "." +
	       QuoteIdentifier(state->tables[block]);
}

//! Whether evaluating `expr` per row reads rows we cannot attribute to a single row of its table (child aggregations,
//! subqueries), so that changes of other tables could change it unnoticed
static bool ReadsOtherRows(const SemanticModel &model, SemanticAnalyzer &analyzer, const ParsedExpression &expr) {
	if (expr.GetExpressionClass() == ExpressionClass::SUBQUERY || analyzer.IsAggregate(expr)) {
		return true;
	}
	auto ref = SemanticMarkers::Classify(expr);
	if (ref.Is(SemanticRefKind::MEMBER)) {
		return ReadsOtherRows(model, analyzer, *model.members[ref.index].resolved);
	}
	bool found = false;
	ParsedExpressionIterator::EnumerateChildren(
	    expr, [&](const ParsedExpression &child) { found = found || ReadsOtherRows(model, analyzer, child); });
	return found;
}

//! Adds the tables on the join path from `home` to every table `expr` reads
static void CollectPathTables(const SemanticModel &model, SemanticAnalyzer &analyzer, const ParsedExpression &expr,
                              idx_t context, const SemanticMaterializedBlock &block, set<idx_t> &tables,
                              const string &what) {
	set<idx_t> read;
	analyzer.RowTables(expr, context, read, what);
	for (auto table : read) {
		for (auto &edge : model.Path(block.home, table, block.using_relationships)) {
			tables.insert(edge.target);
		}
	}
}

static shared_ptr<SemanticMaterialization> PlanMaterialization(ClientContext &context, SemanticModel &model,
                                                               const ParsedSemanticMaterialization &parsed) {
	auto what = "Invalid materialization \"" + parsed.name + "\"";
	SemanticQueryCompiler compiler(model, context);
	SemanticAnalyzer analyzer(model, context);
	auto resolved = compiler.Resolve(parsed.items, nullptr);
	auto result = make_shared_ptr<SemanticMaterialization>();
	result->name = parsed.name;
	result->refresh_mode = parsed.refresh_mode;
	// dimensions first: they are the first columns of every block table
	for (auto kind : {SemanticMemberKind::DIMENSION, SemanticMemberKind::METRIC}) {
		for (idx_t i = 0; i < resolved.items.size(); i++) {
			auto &item = resolved.items[i];
			if (item.kind != kind) {
				continue;
			}
			auto name = item.output_name;
			auto ref = SemanticMarkers::Classify(*item.resolved);
			if (ref.Is(SemanticRefKind::MEMBER)) {
				name = model.MemberDisplayName(ref.index);
			}
			(kind == SemanticMemberKind::DIMENSION ? result->dimension_names : result->metric_names).push_back(name);
			result->spec.items.push_back(SemanticQueryItem {item.kind, std::move(item.resolved),
			                                                "__sv_m" + to_string(result->spec.items.size())});
		}
	}
	result->dimension_count = result->dimension_names.size();
	if (result->metric_names.empty()) {
		throw BinderException("%s: a materialization needs at least one metric", what);
	}
	vector<SemanticBlockPlan> plans;
	try {
		plans = compiler.PlanBlocks(result->spec);
	} catch (std::exception &ex) {
		ErrorData error(ex);
		throw BinderException("%s: %s", what, error.RawMessage());
	}
	idx_t atom_count = 0;
	for (auto &plan : plans) {
		if (plan.non_additive_member != DConstants::INVALID_INDEX) {
			throw BinderException("%s: semi-additive metric \"%s\" (NON ADDITIVE BY) cannot be materialized", what,
			                      model.MemberDisplayName(plan.non_additive_member));
		}
		SemanticMaterializedBlock block;
		block.home = plan.home;
		block.using_relationships = plan.using_relationships;
		block.tables.insert(plan.home);
		for (idx_t d = 0; d < result->dimension_count; d++) {
			set<idx_t> read;
			analyzer.RowTables(*result->spec.items[d].resolved, DConstants::INVALID_INDEX, read, what);
			for (auto table : read) {
				if (!model.Reaches(plan.home, table)) {
					throw BinderException("%s: dimension \"%s\" is not reachable from logical table \"%s\" through "
					                      "many-to-one relationships (many-to-many bridges cannot be materialized)",
					                      what, result->dimension_names[d], model.tables[plan.home].name);
				}
			}
			CollectPathTables(model, analyzer, *result->spec.items[d].resolved, DConstants::INVALID_INDEX, block,
			                  block.tables, what);
		}
		for (idx_t k = 0; k < plan.atoms.size(); k++) {
			SemanticMaterializedAtom atom;
			atom.kind = ClassifySemanticAggregate(*plan.atoms[k].aggregate, plan.atom_types[k]);
			atom.type = plan.atom_types[k];
			atom.prefix = "s" + to_string(atom_count++);
			ParsedExpressionIterator::EnumerateChildren(*plan.atoms[k].aggregate, [&](const ParsedExpression &child) {
				CollectPathTables(model, analyzer, child, plan.atoms[k].home, block, block.tables, what);
			});
			atom.atom = std::move(plan.atoms[k]);
			block.atoms.push_back(std::move(atom));
		}
		result->blocks.push_back(std::move(block));
	}
	// incremental maintenance: every block must read plain many-to-one joins of DuckLake tables, row by row
	result->incremental = true;
	auto unsupported = [&](const string &reason) {
		if (result->incremental) {
			result->incremental = false;
			result->incremental_reason = reason;
		}
	};
	for (idx_t d = 0; d < result->dimension_count; d++) {
		if (ReadsOtherRows(model, analyzer, *result->spec.items[d].resolved)) {
			unsupported("dimension \"" + result->dimension_names[d] + "\" aggregates rows or uses a subquery");
		}
	}
	for (auto &block : result->blocks) {
		for (auto &atom : block.atoms) {
			ParsedExpressionIterator::EnumerateChildren(*atom.atom.aggregate, [&](const ParsedExpression &child) {
				if (ReadsOtherRows(model, analyzer, child)) {
					unsupported("a metric aggregates values that themselves aggregate other rows or use a subquery");
				}
			});
		}
		for (auto table : block.tables) {
			for (auto &edge : model.Path(block.home, table, block.using_relationships)) {
				auto &rel = model.relationships[edge.relationship];
				if (rel.asof_index != DConstants::INVALID_INDEX || rel.is_range) {
					unsupported("relationship \"" + rel.name + "\" is an ASOF or range relationship");
				}
			}
			string reason;
			if (!ClassifySemanticSource(context, model, table, reason).Tracked()) {
				unsupported(reason);
			}
		}
	}
	result->state = make_shared_ptr<SemanticMaterializationState>();
	return result;
}

void PlanSemanticMaterializations(ClientContext &context, SemanticModel &model) {
	case_insensitive_set_t names;
	for (auto &parsed : model.definition->materializations) {
		if (!names.insert(parsed.name).second) {
			throw BinderException("Invalid semantic view \"%s\": duplicate materialization \"%s\"", model.Name().name,
			                      parsed.name);
		}
		model.materializations.push_back(PlanMaterialization(context, model, parsed));
	}
}

void AdoptMaterializationStates(SemanticModel &model, const SemanticModel &previous) {
	for (auto &materialization : model.materializations) {
		for (auto &old : previous.materializations) {
			if (StringUtil::CIEquals(materialization->name, old->name) &&
			    materialization->dimension_names == old->dimension_names &&
			    materialization->metric_names == old->metric_names &&
			    materialization->blocks.size() == old->blocks.size()) {
				materialization->state = old->state;
			}
		}
	}
}

//===--------------------------------------------------------------------===//
// Freshness
//===--------------------------------------------------------------------===//
//! The tracked source tables of a materialization, per DuckLake catalog
map<string, vector<SemanticSourceTable>> SemanticMaterializationSources(ClientContext &context,
                                                                        const SemanticModel &model,
                                                                        const SemanticMaterialization &materialization,
                                                                        bool &all_tracked) {
	map<string, vector<SemanticSourceTable>> result;
	set<idx_t> tables;
	for (auto &block : materialization.blocks) {
		tables.insert(block.tables.begin(), block.tables.end());
	}
	all_tracked = true;
	for (auto table : tables) {
		string reason;
		auto source = ClassifySemanticSource(context, model, table, reason);
		if (!source.Tracked()) {
			all_tracked = false;
			continue;
		}
		result[source.catalog].push_back(source);
	}
	return result;
}

SemanticMaterializationStatus GetMaterializationStatus(ClientContext &context, const SemanticModel &model,
                                                       const SemanticMaterialization &materialization) {
	SemanticMaterializationStatus status;
	auto &state = *materialization.state;
	map<string, idx_t> watermark;
	timestamp_t verified_at;
	{
		lock_guard<mutex> guard(state.lock);
		if (!state.built) {
			status.reason = "not built";
			return status;
		}
		watermark = state.watermark;
		verified_at = state.verified_at;
		// a table created by a refresh committed after this transaction started is not visible to it
		for (auto &table : state.tables) {
			EntryLookupInfo lookup(CatalogType::TABLE_ENTRY, table);
			if (!Catalog::GetEntry(context, state.catalog, state.schema, lookup, OnEntryNotFound::RETURN_NULL)) {
				status.reason = "its table " + table + " is not visible in this transaction";
				return status;
			}
		}
	}
	string stale_reason;
	if (watermark.empty()) {
		stale_reason = "its sources are not change-tracked (only DuckLake tables are), so its freshness is unknown";
	} else {
		bool all_tracked;
		auto sources = SemanticMaterializationSources(context, model, materialization, all_tracked);
		SemanticChangeTracker tracker(context);
		map<string, idx_t> current;
		for (auto &entry : watermark) {
			auto snapshot = tracker.CurrentSnapshot(entry.first);
			current[entry.first] = snapshot;
			if (snapshot == entry.second) {
				continue;
			}
			if (!tracker.SnapshotAvailable(entry.first, entry.second)) {
				stale_reason = "the changes since its snapshot are no longer available";
				break;
			}
			for (auto &source : sources[entry.first]) {
				if (tracker.SchemaChanged(source, entry.second, snapshot) ||
				    tracker.RowsChanged(source, entry.second, snapshot)) {
					stale_reason = "source " + source.catalog + "." + source.schema + "." + source.table +
					               " changed since its last refresh";
					break;
				}
			}
			if (!stale_reason.empty()) {
				break;
			}
		}
		if (stale_reason.empty()) {
			// unchanged sources: the materialization is correct at the current snapshots too
			lock_guard<mutex> guard(state.lock);
			if (state.watermark == watermark) {
				state.watermark = current;
				state.verified_at = Timestamp::GetCurrentTimestamp();
			}
			status.usable = true;
			return status;
		}
	}
	if (model.definition->has_max_staleness) {
		auto age =
		    Timestamp::GetEpochSeconds(Timestamp::GetCurrentTimestamp()) - Timestamp::GetEpochSeconds(verified_at);
		if (age <= model.definition->max_staleness) {
			status.usable = true;
			status.reason = "within MAX_STALENESS (" + stale_reason + ")";
			return status;
		}
	}
	status.reason = stale_reason;
	return status;
}

//===--------------------------------------------------------------------===//
// Query substitution
//===--------------------------------------------------------------------===//
static constexpr const char *MATERIALIZATION_ALIAS = "__sv_mat";

//! `expr` computed from the materialization's dimension columns; nullptr when it reads anything else
static unique_ptr<ParsedExpression> RewriteOverDimensions(const ParsedExpression &expr,
                                                          const SemanticMaterialization &materialization) {
	for (idx_t d = 0; d < materialization.dimension_count; d++) {
		if (expr.Equals(*materialization.spec.items[d].resolved)) {
			return ColumnRef(SemanticDimensionColumn(d), MATERIALIZATION_ALIAS);
		}
	}
	if (!SemanticMarkers::Classify(expr).Is(SemanticRefKind::NONE) ||
	    expr.GetExpressionClass() == ExpressionClass::SUBQUERY) {
		return nullptr;
	}
	auto result = expr.Copy();
	bool failed = false;
	ParsedExpressionIterator::EnumerateChildren(*result, [&](unique_ptr<ParsedExpression> &child) {
		if (failed) {
			return;
		}
		auto rewritten = RewriteOverDimensions(*child, materialization);
		if (!rewritten) {
			failed = true;
			return;
		}
		rewritten->alias = child->alias;
		child = std::move(rewritten);
	});
	return failed ? nullptr : std::move(result);
}

static bool SameContext(const SemanticMetricContext &a, const SemanticMetricContext &b) {
	return a.table == b.table && a.using_relationships == b.using_relationships &&
	       a.non_additive_member == b.non_additive_member;
}

SemanticMaterializationMatcher::SemanticMaterializationMatcher(ClientContext &context, const SemanticModel &model)
    : context(context), model(model), enabled(true) {
	Value setting;
	if (context.TryGetCurrentSetting(SEMANTIC_VIEW_MATERIALIZATIONS_SETTING, setting) && !setting.IsNull()) {
		enabled = BooleanValue::Get(setting);
	}
}

bool SemanticMaterializationMatcher::IsUsable(const SemanticMaterialization &materialization) {
	auto entry = usable_cache.find(&materialization);
	if (entry != usable_cache.end()) {
		return entry->second;
	}
	auto usable = GetMaterializationStatus(context, model, materialization).usable;
	usable_cache[&materialization] = usable;
	return usable;
}

unique_ptr<SemanticMaterializedRead> SemanticMaterializationMatcher::Find(
    const SemanticBlockPlan &block, const vector<idx_t> &atoms, const vector<SemanticAtom> &all_atoms,
    const vector<const ParsedExpression *> &dimensions, optional_ptr<const ParsedExpression> where) {
	if (!enabled || block.non_additive_member != DConstants::INVALID_INDEX) {
		return nullptr;
	}
	// the coarsest usable materialization first: it has the fewest groups to aggregate
	vector<shared_ptr<SemanticMaterialization>> candidates(model.materializations.begin(),
	                                                       model.materializations.end());
	std::stable_sort(candidates.begin(), candidates.end(),
	                 [](const shared_ptr<SemanticMaterialization> &a, const shared_ptr<SemanticMaterialization> &b) {
		                 return a->dimension_count < b->dimension_count;
	                 });
	for (auto &materialization : candidates) {
		for (idx_t b = 0; b < materialization->blocks.size(); b++) {
			auto &stored = materialization->blocks[b];
			if (stored.home != block.home || stored.using_relationships != block.using_relationships) {
				continue;
			}
			// every aggregate of the query block must be stored
			vector<const SemanticMaterializedAtom *> mapped;
			for (auto a : atoms) {
				auto &atom = all_atoms[a];
				optional_ptr<const SemanticMaterializedAtom> found;
				for (auto &candidate : stored.atoms) {
					if (candidate.atom.aggregate->Equals(*atom.aggregate) &&
					    SameContext(candidate.atom.context, atom.context)) {
						found = &candidate;
						break;
					}
				}
				if (!found) {
					break;
				}
				mapped.push_back(found.get());
			}
			if (mapped.size() != atoms.size()) {
				continue;
			}
			// the query's dimensions and filter must be computable from the stored dimensions
			auto read = make_uniq<SemanticMaterializedRead>();
			set<idx_t> grouped;
			bool computable = true;
			for (auto *dimension : dimensions) {
				auto rewritten = RewriteOverDimensions(*dimension, *materialization);
				if (!rewritten) {
					computable = false;
					break;
				}
				for (idx_t d = 0; d < materialization->dimension_count; d++) {
					if (rewritten->Equals(*ColumnRef(SemanticDimensionColumn(d), MATERIALIZATION_ALIAS))) {
						grouped.insert(d);
					}
				}
				read->dimensions.push_back(std::move(rewritten));
			}
			if (computable && where) {
				read->where = RewriteOverDimensions(*where, *materialization);
				computable = read->where != nullptr;
			}
			if (!computable) {
				continue;
			}
			// grouping by fewer dimensions than stored rolls the stored groups up
			bool rollup = grouped.size() != materialization->dimension_count;
			bool rollup_possible = true;
			for (auto *atom : mapped) {
				rollup_possible = rollup_possible && atom->kind != SemanticAggregateKind::OPAQUE;
			}
			if ((rollup && !rollup_possible) || !IsUsable(*materialization)) {
				continue;
			}
			auto table = make_uniq<BaseTableRef>();
			{
				lock_guard<mutex> guard(materialization->state->lock);
				table->catalog_name = materialization->state->catalog;
				table->schema_name = materialization->state->schema;
				table->table_name = materialization->state->tables[b];
			}
			table->alias = MATERIALIZATION_ALIAS;
			read->table = std::move(table);
			for (auto *atom : mapped) {
				read->aggregates.push_back(SemanticRollupExpression(*atom, MATERIALIZATION_ALIAS));
			}
			used = true;
			return read;
		}
	}
	return nullptr;
}

} // namespace duckdb
