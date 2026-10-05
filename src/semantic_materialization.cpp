#include "semantic_definition_exception.hpp"
#include "semantic_materialization.hpp"

#include "duckdb/catalog/catalog.hpp"
#include "duckdb/catalog/entry_lookup_info.hpp"
#include "duckdb/common/error_data.hpp"
#include "duckdb/common/exception/binder_exception.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/common/to_string.hpp"
#include "duckdb/common/types/hash.hpp"
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
// Planning
//===--------------------------------------------------------------------===//
string SemanticMaterialization::TableName(idx_t block) const {
	return QuoteIdentifier(state->catalog) + "." + QuoteIdentifier(state->schema) + "." +
	       QuoteIdentifier(state->tables[block]);
}

shared_ptr<SemanticMaterialization> PlanSemanticMaterialization(ClientContext &context, SemanticModel &model,
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
		throw SemanticDefinitionException("%s: a materialization needs at least one metric", what);
	}
	vector<SemanticBlockPlan> plans;
	try {
		plans = compiler.PlanBlocks(result->spec);
	} catch (SemanticDefinitionException &ex) {
		ErrorData error(ex);
		throw SemanticDefinitionException("%s: %s", what, error.RawMessage());
	} catch (std::exception &ex) {
		ErrorData error(ex);
		throw BinderException("%s: %s", what, error.RawMessage());
	}
	idx_t atom_count = 0;

	for (auto &plan : plans) {
		if (plan.non_additive_member != DConstants::INVALID_INDEX) {
			throw SemanticDefinitionException(
			    "%s: semi-additive metric \"%s\" (NON ADDITIVE BY) cannot be materialized", what,
			    model.MemberDisplayName(plan.non_additive_member));
		}
		SemanticMaterializedBlock block;
		block.home = plan.home;
		block.using_relationships = plan.using_relationships;
		block.tables = std::move(plan.dependencies.tables);
		block.nested_tables = std::move(plan.dependencies.nested_tables);
		block.children = std::move(plan.dependencies.children);
		block.dependencies_complete = plan.dependencies.complete;
		for (idx_t k = 0; k < plan.atoms.size(); k++) {
			SemanticMaterializedAtom atom;
			atom.kind = ClassifySemanticAggregate(*plan.atoms[k].aggregate, plan.atom_types[k]);
			atom.type = plan.atom_types[k];
			atom.prefix = "s" + to_string(atom_count++);
			atom.atom = std::move(plan.atoms[k]);
			block.atoms.push_back(std::move(atom));
		}
		for (auto &child : block.children) {
			if (block.tables.count(child.table) || !block.tables.count(child.parent)) {
				plan.dependencies.Unsupported("a child aggregation overlaps the block's row joins");
			}
		}
		if (!plan.dependencies.incremental_reason.empty() && result->incremental_reason.empty()) {
			result->incremental_reason = plan.dependencies.incremental_reason;
		}
		result->blocks.push_back(std::move(block));
	}
	// incremental maintenance: every block must read plain many-to-one joins of DuckLake tables, row by row
	result->incremental = result->incremental_reason.empty();
	auto unsupported = [&](const string &reason) {
		if (result->incremental) {
			result->incremental = false;
			result->incremental_reason = reason;
		}
	};
	for (idx_t b = 0; b < result->blocks.size(); b++) {
		auto &block = result->blocks[b];

		for (auto table : block.tables) {
			for (auto &edge : model.Path(block.home, table, block.using_relationships)) {
				auto &rel = model.relationships[edge.relationship];
				if (rel.asof_index != DConstants::INVALID_INDEX || rel.is_range) {
					unsupported("relationship \"" + rel.name + "\" is an ASOF or range relationship");
				}
			}
		}
	}
	SemanticMaterializationSources sources(context, model, *result);
	if (!sources.untracked_reason.empty()) {
		unsupported(sources.untracked_reason);
	}
	// storage: a writable DuckDB database or DuckLake catalog (default: the semantic view's database)
	auto storage = parsed.storage_catalog.empty() ? model.Name().catalog : parsed.storage_catalog;
	auto catalog = Catalog::GetCatalogEntry(context, storage);
	if (!catalog) {
		throw BinderException("%s: database \"%s\" does not exist", what, storage);
	}
	if (catalog->IsSystemCatalog() || catalog->IsTemporaryCatalog()) {
		throw BinderException("%s: materializations cannot be stored in the %s database", what, catalog->GetName());
	}
	auto type = catalog->GetCatalogType();
	if (type != "duckdb" && type != "ducklake") {
		throw BinderException("%s: materializations are stored in DuckDB databases or DuckLake catalogs, not in "
		                      "\"%s\" (%s)",
		                      what, catalog->GetName(), type);
	}
	result->storage_catalog = catalog->GetName();
	result->storage_schema = parsed.storage_schema.empty() ? SEMANTIC_MATERIALIZATION_SCHEMA : parsed.storage_schema;
	// the fingerprint: the queries that build the tables (same queries, same content at the same snapshots)
	string queries;
	for (idx_t b = 0; b < result->blocks.size(); b++) {
		auto &block = result->blocks[b];
		queries += compiler
		               .CompileStates(result->spec, b,
		                              [&](idx_t atom, const FunctionExpression &compiled) {
			                              return SemanticStateExpressions(block.atoms[atom], compiled);
		                              })
		               ->ToString();
		queries += ";";
	}
	result->fingerprint = StringUtil::Format("%016llx", static_cast<uint64_t>(Hash(queries.c_str(), queries.size())));
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
		model.materializations.push_back(PlanSemanticMaterialization(context, model, parsed));
	}
}

void AdoptMaterializationStates(SemanticModel &model, const SemanticModel &previous) {
	for (auto &materialization : model.materializations) {
		for (auto &old : previous.materializations) {
			if (StringUtil::CIEquals(materialization->name, old->name) &&
			    materialization->fingerprint == old->fingerprint &&
			    StringUtil::CIEquals(materialization->storage_catalog, old->storage_catalog) &&
			    StringUtil::CIEquals(materialization->storage_schema, old->storage_schema)) {
				materialization->state = old->state;
			}
		}
	}
}

bool SameMaterializationStorage(const SemanticMaterialization &a, const SemanticMaterialization &b) {
	if (a.state == b.state) {
		return true;
	}
	lock_guard<mutex> guard_a(a.state->lock);
	lock_guard<mutex> guard_b(b.state->lock);
	return !a.state->tables.empty() && StringUtil::CIEquals(a.state->catalog, b.state->catalog) &&
	       StringUtil::CIEquals(a.state->schema, b.state->schema) && a.state->tables == b.state->tables;
}

//===--------------------------------------------------------------------===//
// Freshness
//===--------------------------------------------------------------------===//
SemanticMaterializationSources::SemanticMaterializationSources(ClientContext &context, const SemanticModel &model,
                                                               const SemanticMaterialization &materialization) {
	set<idx_t> tables;
	for (auto &block : materialization.blocks) {
		complete = complete && block.dependencies_complete;
		auto sources = block.SourceTables();
		tables.insert(sources.begin(), sources.end());
	}
	for (auto table : tables) {
		string reason;
		auto source = ClassifySemanticSource(context, model, table, reason);
		if (!source.Tracked()) {
			complete = false;
			if (untracked_reason.empty()) {
				untracked_reason = std::move(reason);
			}
			continue;
		}
		by_table.emplace(table, source);
		by_catalog[source.catalog].push_back(std::move(source));
	}
}

bool SemanticMaterializationSources::Covers(const map<string, idx_t> &watermark) const {
	if (!complete || watermark.empty() || by_catalog.size() != watermark.size()) {
		return false;
	}
	for (auto &entry : by_catalog) {
		if (!watermark.count(entry.first)) {
			return false;
		}
	}
	return true;
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
			status.reason = "it has not been built yet (refresh it)";
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
	SemanticMaterializationSources sources(context, model, materialization);
	if (!sources.Covers(watermark)) {
		stale_reason = "its sources are not change-tracked (only DuckLake tables are), so its freshness is unknown";
	} else {
		SemanticChangeTracker tracker(context);
		map<string, idx_t> current;
		for (auto &entry : watermark) {
			auto changes = tracker.CatalogChanges(entry.first, entry.second);
			current[entry.first] = changes.current;
			if (changes.current == entry.second) {
				continue;
			}
			if (!changes.available) {
				stale_reason = "the changes since its snapshot are no longer available";
				break;
			}
			for (auto &source : sources.by_catalog.at(entry.first)) {
				if (changes.SchemaChanged(source) || changes.RowChanges(source).Any()) {
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
