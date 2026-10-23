#include "duckdb/common/enum_util.hpp"
#include "duckdb/common/exception/catalog_exception.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/main/extension/extension_loader.hpp"
#include "semantic_catalog.hpp"
#include "semantic_lowering.hpp"
#include "semantic_materialization.hpp"
#include "semantic_view_registry.hpp"
#include "semantic_views_functions.hpp"

namespace duckdb {

//! A DDL statement: validated at bind time, applied to the registry when executed (not by EXPLAIN)
struct SemanticDDLData : public TableFunctionData {
	explicit SemanticDDLData(shared_ptr<SemanticViewsState> state_p, SemanticQualifiedName name_p)
	    : state(std::move(state_p)), name(std::move(name_p)) {
	}
	virtual string Execute(ClientContext &context) const = 0;

	string DisplayName() const {
		return StringUtil::Upper(name.name);
	}

	shared_ptr<SemanticViewsState> state;
	SemanticQualifiedName name;
};

struct SemanticDDLState : public GlobalTableFunctionState {
	bool done = false;
};

static unique_ptr<GlobalTableFunctionState> SemanticDDLInit(ClientContext &, TableFunctionInitInput &) {
	return make_uniq<SemanticDDLState>();
}

static void SemanticDDLScan(ClientContext &context, TableFunctionInput &data_p, DataChunk &output) {
	auto &state = data_p.global_state->Cast<SemanticDDLState>();
	if (state.done) {
		output.SetCardinality(0);
		return;
	}
	state.done = true;
	output.SetValue(0, 0, Value(data_p.bind_data->Cast<SemanticDDLData>().Execute(context)));
	output.SetCardinality(1);
}

static OnEntryNotFound IfNotFound(bool if_exists) {
	return if_exists ? OnEntryNotFound::RETURN_NULL : OnEntryNotFound::THROW_EXCEPTION;
}

static void StatusColumn(vector<LogicalType> &return_types, vector<Identifier> &names) {
	names.emplace_back("status");
	return_types.emplace_back(LogicalType::VARCHAR);
}

//===--------------------------------------------------------------------===//
// CREATE
//===--------------------------------------------------------------------===//
struct SemanticCreateData : public SemanticDDLData {
	using SemanticDDLData::SemanticDDLData;
	//! null when IF NOT EXISTS found an existing view
	shared_ptr<SemanticModel> model;
	OnCreateConflict on_conflict;
	//! Stored materializations of the view that no longer apply to its new definition
	vector<SemanticStoredMaterialization> obsolete;

	string Execute(ClientContext &context) const override {
		auto replace = on_conflict == OnCreateConflict::REPLACE_ON_CONFLICT;
		if (model && state->registry.Create(model, replace, [&](shared_ptr<SemanticModel> previous) {
			    if (previous) {
				    AdoptMaterializationStates(*model, *previous);
			    }
			    ApplySemanticMaterializationChange(context, previous, model, {}, obsolete, [&] {
				    ApplySemanticViewChange(context, SemanticQualifiedName(), model.get());
			    });
		    })) {
			return "Semantic view " + DisplayName() + " successfully created.";
		}
		if (on_conflict == OnCreateConflict::IGNORE_ON_CONFLICT) {
			return DisplayName() + " already exists, statement succeeded.";
		}
		throw CatalogException("Semantic view with name \"%s\" already exists", name.name);
	}
};

static unique_ptr<FunctionData> SemanticCreateBind(ClientContext &context, TableFunctionBindInput &input,
                                                   vector<LogicalType> &return_types, vector<Identifier> &names) {
	StatusColumn(return_types, names);
	auto definition = SemanticLowering::Decode<ParsedSemanticView>(input.inputs[0]);
	auto &state = input.info->Cast<SemanticViewsFunctionInfo>().state;
	auto result = make_uniq<SemanticCreateData>(state, SemanticViewRegistry::ResolveDDLName(context, definition->name));
	result->on_conflict = EnumUtil::FromString<OnCreateConflict>(StringArgument(input, 1));
	bool skip = result->on_conflict == OnCreateConflict::IGNORE_ON_CONFLICT && state->registry.Get(result->name);
	if (!skip) {
		BindSemanticViewChange(context, input.binder, SemanticQualifiedName(), result->name);
		result->model = BuildSemanticModel(context, *definition, result->name);
		// materializations stored for this view (e.g. before a restart) come back
		// with it
		result->obsolete = RestoreSemanticMaterializations(context, *result->model);
	}
	return std::move(result);
}

//===--------------------------------------------------------------------===//
// DROP
//===--------------------------------------------------------------------===//
struct SemanticDropData : public SemanticDDLData {
	using SemanticDDLData::SemanticDDLData;
	bool if_exists;

	string Execute(ClientContext &context) const override {
		bool dropped_placeholder = false;
		auto dropped = state->registry.Drop(name, [&](shared_ptr<SemanticModel> previous) {
			ApplySemanticMaterializationChange(context, previous, nullptr, {}, {}, [&] {
				dropped_placeholder = ApplySemanticViewChange(context, name, nullptr);
			});
		});
		if (dropped || dropped_placeholder) {
			return DisplayName() + " successfully dropped.";
		}
		if (if_exists) {
			return "Drop statement executed successfully (" + DisplayName() + " already dropped).";
		}
		throw CatalogException("Semantic view with name \"%s\" does not exist!", name.name);
	}
};

static unique_ptr<FunctionData> SemanticDropBind(ClientContext &context, TableFunctionBindInput &input,
                                                 vector<LogicalType> &return_types, vector<Identifier> &names) {
	StatusColumn(return_types, names);
	auto if_exists = BooleanValue::Get(input.inputs[3]);
	auto result = make_uniq<SemanticDropData>(
	    input.info->Cast<SemanticViewsFunctionInfo>().state,
	    SemanticViewRegistry::ResolveDDLName(context, NameArguments(input, 0), IfNotFound(if_exists)));
	result->if_exists = if_exists;
	BindSemanticViewChange(context, input.binder, result->name, SemanticQualifiedName());
	return std::move(result);
}

//===--------------------------------------------------------------------===//
// ALTER: the view is rebuilt from its edited definition and swapped in atomically
//===--------------------------------------------------------------------===//
struct SemanticAlterData : public SemanticDDLData {
	using SemanticDDLData::SemanticDDLData;
	bool if_exists;
	//! The view as found at bind time, and its altered version; null when the view does not exist
	shared_ptr<SemanticModel> existing;
	shared_ptr<SemanticModel> model;
	//! Materializations the altered view adds (built before it replaces the view)
	vector<shared_ptr<SemanticMaterialization>> added;
	string message = "Statement executed successfully.";

	string Execute(ClientContext &context) const override {
		if (!existing) {
			if (if_exists) {
				return "Statement executed successfully.";
			}
			throw CatalogException("Semantic view with name \"%s\" does not exist!", name.name);
		}
		if (!state->registry.Replace(*existing, model, [&] {
			    ApplySemanticMaterializationChange(context, existing, model, added, {}, [&] {
				    ApplySemanticViewChange(context, existing->Name(), model.get());
			    });
		    })) {
			throw CatalogException("Semantic view \"%s\" was dropped or altered by a concurrent statement", name.name);
		}
		return message;
	}
};

template <class EDIT>
static unique_ptr<FunctionData> SemanticAlterBind(ClientContext &context, TableFunctionBindInput &input,
                                                  vector<LogicalType> &return_types, vector<Identifier> &names,
                                                  EDIT edit) {
	StatusColumn(return_types, names);
	auto &state = input.info->Cast<SemanticViewsFunctionInfo>().state;
	auto if_exists = BooleanValue::Get(input.inputs[3]);
	auto result = make_uniq<SemanticAlterData>(
	    state, SemanticViewRegistry::ResolveDDLName(context, NameArguments(input, 0), IfNotFound(if_exists)));
	result->if_exists = if_exists;
	result->existing = state->registry.Get(result->name);
	if (result->existing) {
		auto definition = result->existing->definition->Copy();
		edit(*definition);
		result->model = BuildSemanticModel(context, *definition, definition->name);
		result->model->created_on = result->existing->created_on;
		AdoptMaterializationStates(*result->model, *result->existing);
		for (auto &materialization : result->model->materializations) {
			bool adopted = false;
			for (auto &old : result->existing->materializations) {
				adopted = adopted || old->state == materialization->state;
			}
			if (!adopted) {
				result->added.push_back(materialization);
			}
		}
		BindSemanticViewChange(context, input.binder, result->name, definition->name);
	}
	return std::move(result);
}

static unique_ptr<FunctionData> SemanticRenameBind(ClientContext &context, TableFunctionBindInput &input,
                                                   vector<LogicalType> &return_types, vector<Identifier> &names) {
	auto new_name = StringArgument(input, 4);
	return SemanticAlterBind(context, input, return_types, names,
	                         [&](ParsedSemanticView &definition) { definition.name.name = new_name; });
}

static unique_ptr<FunctionData> SemanticSetCommentBind(ClientContext &context, TableFunctionBindInput &input,
                                                       vector<LogicalType> &return_types, vector<Identifier> &names) {
	auto comment = StringArgument(input, 4);
	return SemanticAlterBind(context, input, return_types, names,
	                         [&](ParsedSemanticView &definition) { definition.comment = comment; });
}

static unique_ptr<FunctionData> SemanticSetMaxStalenessBind(ClientContext &context, TableFunctionBindInput &input,
                                                            vector<LogicalType> &return_types,
                                                            vector<Identifier> &names) {
	auto &seconds = input.inputs[4];
	if (!seconds.IsNull() && seconds.GetValue<int64_t>() < 0) {
		throw BinderException("MAX_STALENESS must not be negative");
	}
	return SemanticAlterBind(context, input, return_types, names, [&](ParsedSemanticView &definition) {
		definition.has_max_staleness = !seconds.IsNull();
		definition.max_staleness = seconds.IsNull() ? 0 : seconds.GetValue<int64_t>();
	});
}

//===--------------------------------------------------------------------===//
// Materializations
//===--------------------------------------------------------------------===//
static idx_t FindMaterialization(const ParsedSemanticView &definition, const string &name) {
	for (idx_t m = 0; m < definition.materializations.size(); m++) {
		if (StringUtil::CIEquals(definition.materializations[m].name, name)) {
			return m;
		}
	}
	return DConstants::INVALID_INDEX;
}

static unique_ptr<FunctionData> SemanticAddMaterializationBind(ClientContext &context, TableFunctionBindInput &input,
                                                               vector<LogicalType> &return_types,
                                                               vector<Identifier> &names) {
	auto payload = SemanticLowering::Decode<ParsedSemanticMaterializationPayload>(input.inputs[4]);
	auto materialization = &payload->materialization;
	auto if_not_exists = BooleanValue::Get(input.inputs[5]);
	bool skipped = false;
	auto result = SemanticAlterBind(context, input, return_types, names, [&](ParsedSemanticView &definition) {
		if (FindMaterialization(definition, materialization->name) != DConstants::INVALID_INDEX) {
			if (!if_not_exists) {
				throw CatalogException("Materialization \"%s\" already exists in semantic view \"%s\"",
				                       materialization->name, definition.name.name);
			}
			skipped = true;
			return;
		}
		definition.materializations.push_back(std::move(*materialization));
	});
	auto &data = result->Cast<SemanticAlterData>();
	if (data.existing) {
		data.message =
		    skipped ? "Materialization already exists, statement succeeded." : "Materialization successfully added.";
	}
	return result;
}

static unique_ptr<FunctionData> SemanticDropMaterializationBind(ClientContext &context, TableFunctionBindInput &input,
                                                                vector<LogicalType> &return_types,
                                                                vector<Identifier> &names) {
	auto name = StringArgument(input, 4);
	auto if_exists = BooleanValue::Get(input.inputs[5]);
	return SemanticAlterBind(context, input, return_types, names, [&](ParsedSemanticView &definition) {
		auto index = FindMaterialization(definition, name);
		if (index == DConstants::INVALID_INDEX) {
			if (!if_exists) {
				throw CatalogException("Materialization \"%s\" does not exist in semantic view \"%s\"", name,
				                       definition.name.name);
			}
			return;
		}
		definition.materializations.erase(definition.materializations.begin() + NumericCast<int64_t>(index));
	});
}

//! REFRESH MATERIALIZATION: brings the materialization up to date (in its own transaction)
struct SemanticRefreshData : public SemanticDDLData {
	using SemanticDDLData::SemanticDDLData;
	bool if_exists;
	shared_ptr<SemanticModel> model;
	shared_ptr<SemanticMaterialization> materialization;
	SemanticRefreshMode mode;

	string Execute(ClientContext &context) const override {
		if (!model) {
			if (if_exists) {
				return "Statement executed successfully.";
			}
			throw CatalogException("Semantic view with name \"%s\" does not exist!", name.name);
		}
		return RefreshSemanticMaterialization(context, *model, *materialization, mode);
	}
};

static unique_ptr<FunctionData> SemanticRefreshMaterializationBind(ClientContext &context,
                                                                   TableFunctionBindInput &input,
                                                                   vector<LogicalType> &return_types,
                                                                   vector<Identifier> &names) {
	StatusColumn(return_types, names);
	auto &state = input.info->Cast<SemanticViewsFunctionInfo>().state;
	auto if_exists = BooleanValue::Get(input.inputs[3]);
	auto result = make_uniq<SemanticRefreshData>(
	    state, SemanticViewRegistry::ResolveDDLName(context, NameArguments(input, 0), IfNotFound(if_exists)));
	result->if_exists = if_exists;
	result->model = state->registry.Get(result->name);
	if (result->model) {
		auto materialization_name = StringArgument(input, 4);
		for (auto &materialization : result->model->materializations) {
			if (StringUtil::CIEquals(materialization->name, materialization_name)) {
				result->materialization = materialization;
			}
		}
		if (!result->materialization) {
			throw CatalogException("Materialization \"%s\" does not exist in semantic view \"%s\"",
			                       materialization_name, result->name.name);
		}
		auto mode = StringArgument(input, 5);
		result->mode = mode == "FULL"          ? SemanticRefreshMode::FULL
		               : mode == "INCREMENTAL" ? SemanticRefreshMode::INCREMENTAL
		               : mode == "AUTO"        ? SemanticRefreshMode::AUTO
		                                       : result->materialization->refresh_mode;
	}
	return std::move(result);
}

void RegisterSemanticDDLFunctions(ExtensionLoader &loader, shared_ptr<SemanticViewsState> state) {
	auto name_and = [](vector<LogicalType> extra) {
		vector<LogicalType> result(3, LogicalType::VARCHAR);
		result.insert(result.end(), extra.begin(), extra.end());
		return result;
	};
	loader.RegisterFunction(SemanticViewsTableFunction(SEMANTIC_VIEW_CREATE_FUNCTION,
	                                                   {LogicalType::BLOB, LogicalType::VARCHAR}, SemanticDDLScan,
	                                                   SemanticCreateBind, SemanticDDLInit, state));
	loader.RegisterFunction(SemanticViewsTableFunction(SEMANTIC_VIEW_DROP_FUNCTION, name_and({LogicalType::BOOLEAN}),
	                                                   SemanticDDLScan, SemanticDropBind, SemanticDDLInit, state));
	loader.RegisterFunction(SemanticViewsTableFunction(SEMANTIC_VIEW_RENAME_FUNCTION,
	                                                   name_and({LogicalType::BOOLEAN, LogicalType::VARCHAR}),
	                                                   SemanticDDLScan, SemanticRenameBind, SemanticDDLInit, state));
	loader.RegisterFunction(SemanticViewsTableFunction(
	    SEMANTIC_VIEW_SET_COMMENT_FUNCTION, name_and({LogicalType::BOOLEAN, LogicalType::VARCHAR}), SemanticDDLScan,
	    SemanticSetCommentBind, SemanticDDLInit, state));
	loader.RegisterFunction(SemanticViewsTableFunction(
	    SEMANTIC_VIEW_SET_MAX_STALENESS_FUNCTION, name_and({LogicalType::BOOLEAN, LogicalType::BIGINT}),
	    SemanticDDLScan, SemanticSetMaxStalenessBind, SemanticDDLInit, state));
	loader.RegisterFunction(
	    SemanticViewsTableFunction(SEMANTIC_VIEW_ADD_MATERIALIZATION_FUNCTION,
	                               name_and({LogicalType::BOOLEAN, LogicalType::BLOB, LogicalType::BOOLEAN}),
	                               SemanticDDLScan, SemanticAddMaterializationBind, SemanticDDLInit, state));
	loader.RegisterFunction(
	    SemanticViewsTableFunction(SEMANTIC_VIEW_DROP_MATERIALIZATION_FUNCTION,
	                               name_and({LogicalType::BOOLEAN, LogicalType::VARCHAR, LogicalType::BOOLEAN}),
	                               SemanticDDLScan, SemanticDropMaterializationBind, SemanticDDLInit, state));
	loader.RegisterFunction(
	    SemanticViewsTableFunction(SEMANTIC_VIEW_REFRESH_MATERIALIZATION_FUNCTION,
	                               name_and({LogicalType::BOOLEAN, LogicalType::VARCHAR, LogicalType::VARCHAR}),
	                               SemanticDDLScan, SemanticRefreshMaterializationBind, SemanticDDLInit, state));
}

} // namespace duckdb
