#define DUCKDB_EXTENSION_MAIN

#include "semantic_views_extension.hpp"

#include "duckdb/main/config.hpp"
#include "duckdb/main/connection_manager.hpp"
#include "duckdb/main/extension/extension_loader.hpp"
#include "semantic_views_functions.hpp"
#include "semantic_views_parser.hpp"
#include "semantic_view_registry.hpp"

namespace duckdb {

static void LoadInternal(ExtensionLoader &loader) {
	auto state = make_shared_ptr<SemanticViewsState>();
	RegisterSemanticViewsFunctions(loader, state);

	auto &db_config = DBConfig::GetConfig(loader.GetDatabaseInstance());
	ParserExtension::Register(db_config, SemanticViewsParserExtension());
	OperatorExtension::Register(db_config, make_shared_ptr<SemanticViewsOperatorExtension>(state));
	// queries over semantic views by name are rewritten on planning errors: on every new connection, and on the
	// connections that already exist when the extension is loaded
	ExtensionCallback::Register(db_config, make_shared_ptr<SemanticViewsConnectionCallback>(state));
	for (auto &connection : ConnectionManager::Get(loader.GetDatabaseInstance()).GetConnectionList()) {
		SemanticViewsRebindState::Register(*connection, state);
	}
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
