#include "semantic_transforms.hpp"

namespace duckdb {

const SemanticOffset &SemanticTransforms::Offset(idx_t index) const {
	if (index < model.offsets.size()) {
		return model.offsets[index];
	}
	return *offsets[index - model.offsets.size()];
}

const SemanticCumulative &SemanticTransforms::Cumulative(idx_t index) const {
	if (index < model.cumulatives.size()) {
		return model.cumulatives[index];
	}
	return *cumulatives[index - model.cumulatives.size()];
}

idx_t SemanticTransforms::AddOffset(SemanticOffset offset) {
	for (idx_t i = 0; i < model.offsets.size(); i++) {
		if (model.offsets[i].Equals(offset)) {
			return i;
		}
	}
	for (idx_t i = 0; i < offsets.size(); i++) {
		if (offsets[i]->Equals(offset)) {
			return model.offsets.size() + i;
		}
	}
	offsets.push_back(make_uniq<SemanticOffset>(std::move(offset)));
	return model.offsets.size() + offsets.size() - 1;
}

idx_t SemanticTransforms::AddCumulative(SemanticCumulative cumulative) {
	for (idx_t i = 0; i < model.cumulatives.size(); i++) {
		if (model.cumulatives[i].Equals(cumulative)) {
			return i;
		}
	}
	for (idx_t i = 0; i < cumulatives.size(); i++) {
		if (cumulatives[i]->Equals(cumulative)) {
			return model.cumulatives.size() + i;
		}
	}
	cumulatives.push_back(make_uniq<SemanticCumulative>(std::move(cumulative)));
	return model.cumulatives.size() + cumulatives.size() - 1;
}

} // namespace duckdb
