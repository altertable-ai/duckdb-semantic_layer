#define DUCKDB_EXTENSION_MAIN

#include "semantic_views_extension.hpp"

#include "duckdb/main/config.hpp"
#include "duckdb/main/extension/extension_loader.hpp"
#include "semantic_views_functions.hpp"
#include "semantic_views_parser.hpp"

namespace duckdb {

static void LoadInternal(ExtensionLoader &loader) {
	RegisterSemanticViewsFunctions(loader);

	auto &db_config = DBConfig::GetConfig(loader.GetDatabaseInstance());
	ParserExtension::Register(db_config, SemanticViewsParserExtension());
	OperatorExtension::Register(db_config, make_shared_ptr<SemanticViewsOperatorExtension>());
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
