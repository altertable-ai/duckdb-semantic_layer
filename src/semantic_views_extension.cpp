#define DUCKDB_EXTENSION_MAIN

#include "semantic_views_extension.hpp"

#include "duckdb/main/extension/extension_loader.hpp"

namespace duckdb {

static void LoadInternal(ExtensionLoader &loader) {
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
