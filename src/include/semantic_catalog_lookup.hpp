//===----------------------------------------------------------------------===//
//                         DuckDB - semantic_views
//
// semantic_catalog_lookup.hpp
//
// Catalog lookups by (catalog, schema, name) strings, where an empty part
// resolves through the client's search path.
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/catalog/catalog.hpp"
#include "duckdb/catalog/entry_lookup_info.hpp"
#include "duckdb/common/enums/on_entry_not_found.hpp"

namespace duckdb {

inline QualifiedName MakeQualifiedName(const string &catalog, const string &schema, const string &name) {
	return QualifiedName(Identifier(catalog), Identifier(schema), Identifier(name));
}

inline optional_ptr<CatalogEntry> LookupCatalogEntry(ClientContext &context, CatalogType type, const string &catalog,
                                                     const string &schema, const string &name,
                                                     OnEntryNotFound if_not_found = OnEntryNotFound::RETURN_NULL) {
	return Catalog::GetEntry(context, EntryLookupInfo(type, MakeQualifiedName(catalog, schema, name)), if_not_found);
}

} // namespace duckdb
