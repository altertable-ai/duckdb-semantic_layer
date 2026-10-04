#define DUCKDB_EXTENSION_MAIN

#include "semantic_views_extension.hpp"

#include "duckdb/main/config.hpp"
#include "duckdb/main/extension/extension_loader.hpp"
#include "semantic_materialization.hpp"
#include "semantic_view_registry.hpp"
#include "semantic_views_binding.hpp"
#include "semantic_views_functions.hpp"

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

void SemanticViewsExtension::Load(ExtensionLoader &loader) {
	LoadInternal(loader);
}

std::string SemanticViewsExtension::Name() {
	return "semantic_views";
}

std::string SemanticViewsExtension::Version() const {
#ifdef EXT_VERSION_SEMANTIC_VIEWS
	return EXT_VERSION_SEMANTIC_VIEWS;
#else
	return "";
#endif
}

} // namespace duckdb

extern "C" {

DUCKDB_CPP_EXTENSION_ENTRY(semantic_views, loader) {
	duckdb::LoadInternal(loader);
}
}
