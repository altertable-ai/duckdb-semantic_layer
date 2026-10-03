//===----------------------------------------------------------------------===//
//                         DuckDB - semantic_views
//
// semantic_bridge.hpp
//
// Many-to-many paths: the bridge table through which a table reaches tables
// that are not at an equal or lower granularity (book -> book_authors ->
// author), and the anchor table both sides share.
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/set.hpp"
#include "semantic_model.hpp"

namespace duckdb {

//! How rows of a home table relate to rows of a bridge table: through the same row of the anchor table
struct SemanticBridge {
	//! The bridge table (many-to-one relationships to both sides)
	idx_t bridge = DConstants::INVALID_INDEX;
	//! The finest table reachable from both the home table and the bridge
	idx_t anchor = DConstants::INVALID_INDEX;
	//! Key columns of the anchor (a primary or unique key)
	vector<string> anchor_key;
	//! Table on the bridge's path to the anchor whose columns reference the anchor key, and those columns
	idx_t key_source = DConstants::INVALID_INDEX;
	vector<string> key_source_columns;

	bool SameAs(const SemanticBridge &other) const {
		return bridge == other.bridge && anchor == other.anchor;
	}
};

class SemanticBridgeResolver {
public:
	explicit SemanticBridgeResolver(const SemanticModel &model);

	//! The bridge through which rows of `home` relate to every table of `tables` (some of which `home` does not
	//! reach). Returns false when there is none; throws a BinderException when several bridges qualify.
	bool Resolve(idx_t home, const set<idx_t> &tables, const vector<idx_t> &using_relationships,
	             SemanticBridge &result) const;
	//! A table outside `tables` that reaches all of them through diverging paths (the base of a dimension-only
	//! query across a bridge); INVALID_INDEX when there is none, throws when several qualify
	idx_t CommonBase(const set<idx_t> &tables) const;

private:
	bool TryCandidate(idx_t home, idx_t bridge, const set<idx_t> &tables, const vector<idx_t> &using_relationships,
	                  SemanticBridge &result) const;
	bool PathFrom(idx_t from, idx_t to, const vector<idx_t> &using_relationships, vector<SemanticEdge> &path) const;
	bool IsSpecialJoin(const SemanticEdge &edge) const;
	string TableList(const vector<idx_t> &tables) const;

	const SemanticModel &model;
};

} // namespace duckdb
