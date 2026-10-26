#define DUCKDB_EXTENSION_MAIN

#include "semantic_layer_extension.hpp"

#include "duckdb/main/config.hpp"
#include "duckdb/main/extension/extension_loader.hpp"
#include "semantic_materialization.hpp"
#include "semantic_layer_registry.hpp"
#include "semantic_layer_binding.hpp"
#include "semantic_layer_functions.hpp"

namespace duckdb {

static void LoadInternal(ExtensionLoader &loader) {
	auto &config = DBConfig::GetConfig(loader.GetDatabaseInstance());
	config.AddExtensionOption(SEMANTIC_VIEW_MATERIALIZATIONS_SETTING,
	                          "Answer semantic view queries from fresh materializations when they can",
	                          LogicalType::BOOLEAN, Value::BOOLEAN(true));
	auto state = make_shared_ptr<SemanticViewsState>();
	RegisterSemanticViewsFunctions(loader, state);
	RegisterSemanticViewsBinding(loader.GetDatabaseInstance(), std::move(state));
}

void SemanticLayerExtension::Load(ExtensionLoader &loader) {
	LoadInternal(loader);
}

std::string SemanticLayerExtension::Name() {
	return "semantic_layer";
}

std::string SemanticLayerExtension::Version() const {
#ifdef EXT_VERSION_SEMANTIC_LAYER
	return EXT_VERSION_SEMANTIC_LAYER;
#else
	return "";
#endif
}

} // namespace duckdb

extern "C" {

DUCKDB_CPP_EXTENSION_ENTRY(semantic_layer, loader) {
	duckdb::LoadInternal(loader);
}
}
