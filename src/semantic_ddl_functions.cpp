#include "duckdb/common/enum_util.hpp"
#include "duckdb/common/exception/catalog_exception.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/main/extension/extension_loader.hpp"
#include "semantic_lowering.hpp"
#include "semantic_view_registry.hpp"
#include "semantic_views_functions.hpp"

namespace duckdb {

//! A DDL statement: validated at bind time, applied to the registry when executed (not by EXPLAIN)
struct SemanticDDLData : public TableFunctionData {
	explicit SemanticDDLData(shared_ptr<SemanticViewsState> state_p, QualifiedName name_p)
	    : state(std::move(state_p)), name(std::move(name_p)) {
	}
	virtual string Execute() const = 0;

	string DisplayName() const {
		return StringUtil::Upper(name.name);
	}

	shared_ptr<SemanticViewsState> state;
	QualifiedName name;
};

struct SemanticDDLState : public GlobalTableFunctionState {
	bool done = false;
};

static unique_ptr<GlobalTableFunctionState> SemanticDDLInit(ClientContext &, TableFunctionInitInput &) {
	return make_uniq<SemanticDDLState>();
}

static void SemanticDDLScan(ClientContext &, TableFunctionInput &data_p, DataChunk &output) {
	auto &state = data_p.global_state->Cast<SemanticDDLState>();
	if (state.done) {
		output.SetCardinality(0);
		return;
	}
	state.done = true;
	output.SetValue(0, 0, Value(data_p.bind_data->Cast<SemanticDDLData>().Execute()));
	output.SetCardinality(1);
}

static void StatusColumn(vector<LogicalType> &return_types, vector<string> &names) {
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

	string Execute() const override {
		if (model && state->registry.Create(model, on_conflict == OnCreateConflict::REPLACE_ON_CONFLICT)) {
			return "Semantic view " + DisplayName() + " successfully created.";
		}
		if (on_conflict == OnCreateConflict::IGNORE_ON_CONFLICT) {
			return DisplayName() + " already exists, statement succeeded.";
		}
		throw CatalogException("Semantic view with name \"%s\" already exists", name.name);
	}
};

static unique_ptr<FunctionData> SemanticCreateBind(ClientContext &context, TableFunctionBindInput &input,
                                                   vector<LogicalType> &return_types, vector<string> &names) {
	StatusColumn(return_types, names);
	auto definition = SemanticLowering::Decode<ParsedSemanticView>(input.inputs[0]);
	auto &state = input.info->Cast<SemanticViewsFunctionInfo>().state;
	auto result = make_uniq<SemanticCreateData>(state, SemanticViewRegistry::ResolveDDLName(context, definition->name));
	result->on_conflict = EnumUtil::FromString<OnCreateConflict>(StringArgument(input, 1));
	bool skip = result->on_conflict == OnCreateConflict::IGNORE_ON_CONFLICT && state->registry.Get(result->name);
	if (!skip) {
		result->model = BuildSemanticModel(context, *definition, result->name);
	}
	return std::move(result);
}

//===--------------------------------------------------------------------===//
// DROP
//===--------------------------------------------------------------------===//
struct SemanticDropData : public SemanticDDLData {
	using SemanticDDLData::SemanticDDLData;
	bool if_exists;

	string Execute() const override {
		if (state->registry.Drop(name)) {
			return DisplayName() + " successfully dropped.";
		}
		if (if_exists) {
			return "Drop statement executed successfully (" + DisplayName() + " already dropped).";
		}
		throw CatalogException("Semantic view with name \"%s\" does not exist!", name.name);
	}
};

static unique_ptr<FunctionData> SemanticDropBind(ClientContext &context, TableFunctionBindInput &input,
                                                 vector<LogicalType> &return_types, vector<string> &names) {
	StatusColumn(return_types, names);
	auto result = make_uniq<SemanticDropData>(input.info->Cast<SemanticViewsFunctionInfo>().state,
	                                          SemanticViewRegistry::ResolveDDLName(context, NameArguments(input, 0)));
	result->if_exists = BooleanValue::Get(input.inputs[3]);
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

	string Execute() const override {
		if (!existing) {
			if (if_exists) {
				return "Statement executed successfully.";
			}
			throw CatalogException("Semantic view with name \"%s\" does not exist!", name.name);
		}
		if (!state->registry.Replace(*existing, model)) {
			throw CatalogException("Semantic view \"%s\" was dropped or altered by a concurrent statement", name.name);
		}
		return "Statement executed successfully.";
	}
};

template <class EDIT>
static unique_ptr<FunctionData> SemanticAlterBind(ClientContext &context, TableFunctionBindInput &input,
                                                  vector<LogicalType> &return_types, vector<string> &names, EDIT edit) {
	StatusColumn(return_types, names);
	auto &state = input.info->Cast<SemanticViewsFunctionInfo>().state;
	auto result =
	    make_uniq<SemanticAlterData>(state, SemanticViewRegistry::ResolveDDLName(context, NameArguments(input, 0)));
	result->if_exists = BooleanValue::Get(input.inputs[3]);
	result->existing = state->registry.Get(result->name);
	if (result->existing) {
		auto definition = result->existing->definition->Copy();
		edit(*definition);
		result->model = BuildSemanticModel(context, *definition, definition->name);
		result->model->created_on = result->existing->created_on;
	}
	return std::move(result);
}

static unique_ptr<FunctionData> SemanticRenameBind(ClientContext &context, TableFunctionBindInput &input,
                                                   vector<LogicalType> &return_types, vector<string> &names) {
	auto new_name = StringArgument(input, 4);
	return SemanticAlterBind(context, input, return_types, names,
	                         [&](ParsedSemanticView &definition) { definition.name.name = new_name; });
}

static unique_ptr<FunctionData> SemanticSetCommentBind(ClientContext &context, TableFunctionBindInput &input,
                                                       vector<LogicalType> &return_types, vector<string> &names) {
	auto comment = StringArgument(input, 4);
	return SemanticAlterBind(context, input, return_types, names,
	                         [&](ParsedSemanticView &definition) { definition.comment = comment; });
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
}

} // namespace duckdb
