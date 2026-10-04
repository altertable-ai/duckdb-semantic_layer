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

	//! The latest snapshot of a DuckLake catalog
	idx_t CurrentSnapshot(const string &catalog);
	//! Whether the changes since `snapshot` are still available (the snapshot was not expired)
	bool SnapshotAvailable(const string &catalog, idx_t snapshot);
	//! Whether the definition of `table` changed (altered, dropped, recreated) in (from, to]
	bool SchemaChanged(const SemanticSourceTable &table, idx_t from, idx_t to);
	//! Whether rows of `table` were inserted / deleted in (from, to], from the snapshots' metadata (no data is read);
	//! an update is a deletion and an insertion of the same row id
	SemanticTableChanges RowChanges(const SemanticSourceTable &table, idx_t from, idx_t to);
	bool RowsChanged(const SemanticSourceTable &table, idx_t from, idx_t to) {
		return RowChanges(table, from, to).Any();
	}

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
