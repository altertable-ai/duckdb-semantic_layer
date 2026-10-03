#include "semantic_bridge.hpp"

#include "duckdb/common/exception/binder_exception.hpp"
#include "duckdb/common/string_util.hpp"

namespace duckdb {

SemanticBridgeResolver::SemanticBridgeResolver(const SemanticModel &model) : model(model) {
}

bool SemanticBridgeResolver::PathFrom(idx_t from, idx_t to, const vector<idx_t> &using_relationships,
                                      vector<SemanticEdge> &path) const {
	// several paths, or none allowed by USING: not a usable bridge
	return model.TryPath(from, to, using_relationships, path) == SemanticPathResult::FOUND;
}

bool SemanticBridgeResolver::IsSpecialJoin(const SemanticEdge &edge) const {
	auto &rel = model.relationships[edge.relationship];
	return rel.asof_index != DConstants::INVALID_INDEX || rel.is_range;
}

static bool SameEdge(const SemanticEdge &a, const SemanticEdge &b) {
	return a.relationship == b.relationship && a.reversed == b.reversed;
}

template <class TABLES>
string SemanticBridgeResolver::TableList(const TABLES &tables) const {
	vector<string> names;
	for (auto table : tables) {
		names.push_back("\"" + model.tables[table].name + "\"");
	}
	return StringUtil::Join(names, ", ");
}

bool SemanticBridgeResolver::TryCandidate(idx_t home, idx_t bridge, const set<idx_t> &tables,
                                          const vector<idx_t> &using_relationships, SemanticBridge &result) const {
	if (model.Reaches(home, bridge)) {
		return false;
	}
	for (auto table : tables) {
		if (!model.Reaches(bridge, table)) {
			return false;
		}
	}
	// the anchor: the finest table both sides reach
	set<idx_t> common;
	for (idx_t t = 0; t < model.tables.size(); t++) {
		if (model.Reaches(bridge, t) && model.Reaches(home, t)) {
			common.insert(t);
		}
	}
	auto finest = model.FinestTable(common);
	if (!finest.IsValid()) {
		return false;
	}
	auto anchor = finest.GetIndex();
	vector<SemanticEdge> anchor_path;
	if (!PathFrom(bridge, anchor, using_relationships, anchor_path)) {
		return false;
	}
	for (auto &edge : anchor_path) {
		if (IsSpecialJoin(edge)) {
			return false;
		}
	}
	// the paths to the far side must leave the bridge through another relationship than the path to the anchor
	for (auto table : tables) {
		if (model.Reaches(home, table)) {
			continue;
		}
		vector<SemanticEdge> path;
		if (!PathFrom(bridge, table, using_relationships, path) || path.empty() || SameEdge(path[0], anchor_path[0]) ||
		    IsSpecialJoin(path[0])) {
			return false;
		}
	}
	auto &last = anchor_path.back();
	auto &rel = model.relationships[last.relationship];
	result.bridge = bridge;
	result.anchor = anchor;
	result.anchor_key = last.reversed ? rel.from_columns : rel.to_columns;
	result.key_source = anchor_path.size() >= 2 ? anchor_path[anchor_path.size() - 2].target : bridge;
	result.key_source_columns = last.reversed ? rel.to_columns : rel.from_columns;
	return true;
}

vector<SemanticBridge> SemanticBridgeResolver::Candidates(idx_t home, const set<idx_t> &tables,
                                                          const vector<idx_t> &using_relationships) const {
	vector<SemanticBridge> candidates;
	for (idx_t bridge = 0; bridge < model.tables.size(); bridge++) {
		SemanticBridge candidate;
		if (TryCandidate(home, bridge, tables, using_relationships, candidate)) {
			candidates.push_back(std::move(candidate));
		}
	}
	if (candidates.size() > 1 && !using_relationships.empty()) {
		// USING (relationship) on the metric selects the bridge the relationship starts from
		vector<SemanticBridge> pinned;
		for (auto &candidate : candidates) {
			for (auto rel : using_relationships) {
				if (model.relationships[rel].from_table == candidate.bridge) {
					pinned.push_back(candidate);
					break;
				}
			}
		}
		if (!pinned.empty()) {
			return pinned;
		}
	}
	return candidates;
}

BinderException SemanticBridgeResolver::AmbiguityError(idx_t home, const vector<idx_t> &far_tables,
                                                       const vector<SemanticBridge> &candidates) const {
	vector<idx_t> bridges;
	for (auto &candidate : candidates) {
		bridges.push_back(candidate.bridge);
	}
	return BinderException("Ambiguous many-to-many path from \"%s\" to %s: bridge tables %s both qualify; specify "
	                       "USING (...) on the metric",
	                       model.tables[home].name, TableList(far_tables), TableList(bridges));
}

SemanticBridge SemanticBridgeResolver::Resolve(idx_t home, const set<idx_t> &tables,
                                               const vector<idx_t> &using_relationships, const string &what,
                                               const string &hint) const {
	auto candidates = Candidates(home, tables, using_relationships);
	if (candidates.size() == 1) {
		return std::move(candidates[0]);
	}
	vector<idx_t> far_tables;
	for (auto table : tables) {
		if (!model.Reaches(home, table)) {
			far_tables.push_back(table);
		}
	}
	D_ASSERT(!far_tables.empty());
	if (candidates.size() > 1) {
		throw AmbiguityError(home, far_tables, candidates);
	}
	// no single bridge relates all the tables: find the table without any, else the bridges the tables need
	set<idx_t> bridges;
	for (auto table : far_tables) {
		auto single = Candidates(home, {table}, using_relationships);
		if (single.empty()) {
			throw BinderException("Invalid dimension specified: The dimension entity '%s' must be related to and have "
			                      "an equal or lower level of granularity compared to the base metric or dimension "
			                      "entity '%s'.",
			                      model.tables[table].name, model.tables[home].name);
		}
		if (single.size() > 1) {
			throw AmbiguityError(home, {table}, single);
		}
		bridges.insert(single[0].bridge);
	}
	if (bridges.size() == 1) {
		throw BinderException("%s: combines tables that the many-to-many bridge %s does not relate%s", what,
		                      TableList(bridges), hint);
	}
	throw BinderException("%s: combines tables reached through different many-to-many bridges (%s)%s", what,
	                      TableList(bridges), hint);
}

idx_t SemanticBridgeResolver::CommonBase(const set<idx_t> &tables) const {
	vector<idx_t> candidates;
	for (idx_t base = 0; base < model.tables.size(); base++) {
		if (tables.count(base)) {
			continue;
		}
		bool diverges = false;
		bool valid = true;
		optional_ptr<const SemanticEdge> first_edge;
		vector<vector<SemanticEdge>> paths(tables.size());
		idx_t i = 0;
		for (auto table : tables) {
			auto &path = paths[i++];
			if (!PathFrom(base, table, {}, path)) {
				valid = false;
				break;
			}
			if (!first_edge) {
				first_edge = &path[0];
			} else if (!SameEdge(*first_edge, path[0])) {
				diverges = true;
			}
		}
		if (valid && diverges) {
			candidates.push_back(base);
		}
	}
	if (candidates.empty()) {
		return DConstants::INVALID_INDEX;
	}
	if (candidates.size() > 1) {
		vector<idx_t> table_list(tables.begin(), tables.end());
		throw BinderException("Ambiguous many-to-many path between %s: bridge tables %s both qualify",
		                      TableList(table_list), TableList(candidates));
	}
	return candidates[0];
}

} // namespace duckdb
