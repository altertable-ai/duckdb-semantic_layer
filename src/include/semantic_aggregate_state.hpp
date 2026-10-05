#pragma once

#include "semantic_materialization.hpp"

namespace duckdb {

enum class SemanticRecompute : uint8_t { NEVER, DELETIONS, ALWAYS };

//! Every SQL operation needed to maintain one atom, in the order of its state columns.
struct SemanticAggregateMaintenance {
	vector<string> columns;
	vector<string> delta;
	vector<string> updates;
	vector<string> inserts;
	SemanticRecompute recompute = SemanticRecompute::NEVER;
};

SemanticAggregateMaintenance PlanSemanticAggregateMaintenance(const SemanticMaterializedAtom &atom);

} // namespace duckdb
