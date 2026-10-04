#include "semantic_materialization_storage.hpp"

#include "duckdb/catalog/catalog.hpp"
#include "duckdb/common/exception/binder_exception.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/main/attached_database.hpp"

namespace duckdb {

namespace {

//! DuckDB permits writing one native database per transaction. Prepare every storage catalog before committing
//! any, and keep each catalog's tables and metadata in the same transaction.
class MaterializationStorageChange {
public:
	explicit MaterializationStorageChange(ClientContext &context) : context(context) {
	}

	Connection &Storage(const string &catalog) {
		auto entry = connections.find(catalog);
		if (entry != connections.end()) {
			return *entry->second;
		}
		if (Catalog::GetCatalog(context, catalog).GetAttached().IsReadOnly()) {
			throw BinderException("database \"%s\" is read-only", catalog);
		}
		auto connection = make_uniq<Connection>(*context.db);
		connection->BeginTransaction();
		auto &result = *connection;
		connections.emplace(catalog, std::move(connection));
		return result;
	}

	void Commit() {
		for (auto &entry : connections) {
			entry.second->Commit();
		}
	}

private:
	ClientContext &context;
	map<string, unique_ptr<Connection>> connections;
};

vector<shared_ptr<SemanticMaterialization>> Removed(const shared_ptr<SemanticModel> &before,
                                                    const shared_ptr<SemanticModel> &after) {
	vector<shared_ptr<SemanticMaterialization>> result;
	if (!before) {
		return result;
	}
	for (auto &old : before->materializations) {
		bool kept = false;
		if (after) {
			for (auto &materialization : after->materializations) {
				kept = kept || SameMaterializationStorage(*materialization, *old);
			}
		}
		if (!kept) {
			result.push_back(old);
		}
	}
	return result;
}

} // namespace

void ApplySemanticMaterializationChange(ClientContext &context, const shared_ptr<SemanticModel> &before,
                                        const shared_ptr<SemanticModel> &after,
                                        const vector<shared_ptr<SemanticMaterialization>> &added,
                                        const vector<SemanticStoredMaterialization> &obsolete,
                                        const std::function<void()> &placeholder) {
	auto removed = Removed(before, after);
	vector<shared_ptr<SemanticMaterialization>> retired;
	if (before) {
		for (auto &old : before->materializations) {
			bool shared = false;
			if (after) {
				for (auto &next : after->materializations) {
					shared = shared || old->state == next->state;
				}
			}
			if (!shared) {
				retired.push_back(old);
			}
		}
	}
	auto renamed = before && after && !StringUtil::CIEquals(before->Name().name, after->Name().name);
	// Keep in-flight refreshes out of the storage being removed or renamed. Only newly allocated states are changed
	// while staging; shared live states are invalidated after commit.
	set<SemanticMaterializationState *> protected_states;
	for (auto &materialization : retired) {
		protected_states.insert(materialization->state.get());
	}
	if (renamed) {
		for (auto &materialization : after->materializations) {
			protected_states.insert(materialization->state.get());
		}
	}
	vector<unique_lock<mutex>> locks;
	for (auto *state : protected_states) {
		locks.emplace_back(state->refresh_lock);
	}
	MaterializationStorageChange change(context);
	// Reserve all writable storage boundaries before performing any changes.
	for (auto &materialization : removed) {
		change.Storage(materialization->storage_catalog);
	}
	for (auto &entry : obsolete) {
		change.Storage(entry.catalog);
	}
	for (auto &materialization : added) {
		change.Storage(materialization->storage_catalog);
	}
	if (renamed) {
		for (auto &materialization : after->materializations) {
			change.Storage(materialization->storage_catalog);
		}
	}
	for (auto &materialization : removed) {
		DropSemanticMaterializationTables(change.Storage(materialization->storage_catalog), *materialization);
	}
	for (auto &entry : obsolete) {
		DropStoredMaterialization(change.Storage(entry.catalog), entry);
	}
	vector<pair<shared_ptr<SemanticMaterializationState>, SemanticMaterializationRefreshResult>> built;
	for (auto &materialization : added) {
		built.emplace_back(materialization->state,
		                   BuildSemanticMaterialization(context, change.Storage(materialization->storage_catalog),
		                                                *after, *materialization));
	}
	if (renamed) {
		set<string> catalogs;
		for (auto &materialization : after->materializations) {
			catalogs.insert(materialization->storage_catalog);
		}
		for (auto &catalog : catalogs) {
			RenameStoredMaterializations(change.Storage(catalog), *after, before->Name(), catalog);
		}
	}
	placeholder();
	change.Commit();
	for (auto &entry : built) {
		entry.second.Publish(*entry.first);
	}
	if (renamed) {
		for (auto &materialization : after->materializations) {
			lock_guard<mutex> guard(materialization->state->lock);
			materialization->state->owner = after->Name();
		}
	}
	for (auto &materialization : retired) {
		lock_guard<mutex> guard(materialization->state->lock);
		materialization->state->built = false;
		materialization->state->retired = true;
	}
}

} // namespace duckdb
