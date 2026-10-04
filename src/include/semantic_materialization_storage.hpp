#pragma once

#include "semantic_materialization_internal.hpp"

namespace duckdb {

//! The persisted row, decoded once at the storage boundary.
struct SemanticMaterializationRecord {
	SemanticStoredMaterialization location;
	ParsedSemanticMaterialization definition;
	string fingerprint;
	map<string, idx_t> watermark;
	timestamp_t verified_at;
	string last_refresh;
	string last_refresh_detail;
};

bool HasMetadata(ClientContext &context, const string &catalog);
vector<SemanticMaterializationRecord> ReadStoredMaterializations(Connection &connection, const string &catalog,
                                                                 const QualifiedName &view);
bool ReattachStoredMaterialization(SemanticMaterialization &materialization,
                                   const SemanticMaterializationRecord &record);
void WriteMetadata(Connection &connection, const SemanticModel &model, const SemanticMaterialization &materialization,
                   const SemanticMaterializationState &state, const map<string, idx_t> &watermark,
                   timestamp_t verified_at, const string &outcome, const string &detail);
void UpdateMetadata(Connection &connection, const SemanticMaterialization &materialization,
                    const SemanticMaterializationState &state, const map<string, idx_t> &watermark,
                    timestamp_t verified_at, const string &outcome, const string &detail);
//! The staged result of a refresh. Publish it only after its storage transaction commits.
struct SemanticMaterializationRefreshResult {
	map<string, idx_t> watermark;
	timestamp_t verified_at;
	string outcome;
	string detail;

	void Publish(SemanticMaterializationState &state) const;
	string Describe(const string &name) const;
};

SemanticMaterializationRefreshResult BuildSemanticMaterialization(ClientContext &context, Connection &connection,
                                                                  const SemanticModel &model,
                                                                  SemanticMaterialization &materialization);
void DropSemanticMaterializationTables(Connection &connection, const SemanticMaterialization &materialization);
void DropStoredMaterialization(Connection &connection, const SemanticStoredMaterialization &stored);
void RenameStoredMaterializations(Connection &connection, const SemanticModel &model,
                                  const QualifiedName &previous_name, const string &storage_catalog);

} // namespace duckdb
