//===----------------------------------------------------------------------===//
//                         DuckDB - semantic_views
//
// semantic_change_tracking.hpp
//
// What changed in the sources of a materialization. Materializations only
// ask: which version is each source at, did a table change between two
// versions, and which rows changed. DuckLake tables answer through snapshots
// and ducklake_table_changes(); other tables are untracked (full refresh only,
// freshness unknown).
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/main/connection.hpp"
#include "duckdb/parser/tableref.hpp"
#include "semantic_model.hpp"

namespace duckdb {

//! A physical source table and how its changes can be tracked
struct SemanticSourceTable {
	//! DuckLake catalog (empty: untracked) and the table within it
	string catalog;
	string schema;
	string table;

	bool Tracked() const {
		return !catalog.empty();
	}
};

//! Which kinds of row changes a table had between two snapshots
struct SemanticTableChanges {
	bool inserted = false;
	bool deleted = false;

	bool Any() const {
		return inserted || deleted;
	}
};

//! What happened in a DuckLake catalog since a snapshot, read from its snapshot metadata (no data is read)
struct SemanticCatalogChanges {
	//! The latest snapshot
	idx_t current = 0;
	//! Whether the changes since the requested snapshot are still available (it was not expired)
	bool available = true;

	//! The kinds of row changes of `table` since the requested snapshot (an update deletes and inserts a row id)
	SemanticTableChanges RowChanges(const SemanticSourceTable &table) const;
	//! Whether the definition of `table` changed (altered, dropped or recreated, or its schema dropped)
	bool SchemaChanged(const SemanticSourceTable &table) const;

	//! The (kind, value) entries of the snapshots' change maps, and the current tables (name, id, schema id)
	vector<pair<string, string>> changes;
	struct Table {
		string name;
		string id;
		string schema_id;
	};
	vector<Table> tables;
};

//! How logical table `table` can be tracked; `reason` explains why it cannot
SemanticSourceTable ClassifySemanticSource(ClientContext &context, const SemanticModel &model, idx_t table,
                                           string &reason);

//! Queries change information through its own connection (callable while a query of the caller is being bound)
class SemanticChangeTracker {
public:
	explicit SemanticChangeTracker(ClientContext &context);

	Connection &GetConnection() {
		return connection;
	}

	//! The current snapshot of a DuckLake catalog and, when `since` is valid, what changed after it (one query)
	SemanticCatalogChanges CatalogChanges(const string &catalog, optional_idx since);

	//! The changed rows of `table` in (from, to] (rowid, snapshot_id, change_type 'insert' / 'delete', its columns),
	//! reading only the kinds of changes in `changes` (deleted rows are expensive to read: their values are fetched
	//! from the data files)
	static string ChangesSQL(const SemanticSourceTable &table, idx_t from, idx_t to,
	                         const SemanticTableChanges &changes);
	//! The table at a snapshot: `catalog.schema.table AT (VERSION => snapshot)`
	static unique_ptr<TableRef> AtSnapshot(const SemanticSourceTable &table, idx_t snapshot);

private:
	unique_ptr<MaterializedQueryResult> Run(const string &sql);

	Connection connection;
};

} // namespace duckdb
