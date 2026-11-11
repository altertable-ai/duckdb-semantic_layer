//===----------------------------------------------------------------------===//
//                         DuckDB - semantic_layer
//
// semantic_transforms.hpp
//
// The OFFSETs and CUMULATIVEs a query reads: those of the model (SemanticModel::offsets / cumulatives, shared and
// immutable) followed by the ones the query adds itself. An atom's metric context holds indices into this store.
//
//===----------------------------------------------------------------------===//

#pragma once

#include "semantic_model.hpp"

namespace duckdb {

class SemanticTransforms {
public:
	explicit SemanticTransforms(const SemanticModel &model) : model(model) {
	}

	//! Index < model.offsets.size(): the model's; otherwise query-local
	const SemanticOffset &Offset(idx_t index) const;
	const SemanticCumulative &Cumulative(idx_t index) const;
	//! Index of an Equals() entry (model's first, then local), or appends; returns its index
	idx_t AddOffset(SemanticOffset offset);
	idx_t AddCumulative(SemanticCumulative cumulative);

private:
	const SemanticModel &model;
	//! Boxed so that references handed out stay valid when entries are added
	vector<unique_ptr<SemanticOffset>> offsets;
	vector<unique_ptr<SemanticCumulative>> cumulatives;
};

} // namespace duckdb
