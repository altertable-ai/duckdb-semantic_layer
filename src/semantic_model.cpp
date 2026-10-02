#include "semantic_model.hpp"

#include "duckdb/common/exception.hpp"
#include "duckdb/common/exception/binder_exception.hpp"
#include "duckdb/common/exception/catalog_exception.hpp"
#include "duckdb/common/to_string.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/parser/expression/subquery_expression.hpp"
#include "duckdb/parser/parsed_expression_iterator.hpp"

namespace duckdb {

bool SemanticTable::IsKey(const vector<string> &cols) const {
	auto matches = [&](const vector<string> &key) {
		if (key.size() != cols.size() || key.empty()) {
			return false;
		}
		for (auto &col : cols) {
			bool found = false;
			for (auto &k : key) {
				if (StringUtil::CIEquals(k, col)) {
					found = true;
					break;
				}
			}
			if (!found) {
				return false;
			}
		}
		return true;
	};
	if (matches(primary_key)) {
		return true;
	}
	for (auto &unique : unique_keys) {
		if (matches(unique)) {
			return true;
		}
	}
	return false;
}

//===--------------------------------------------------------------------===//
// Copy
//===--------------------------------------------------------------------===//
shared_ptr<SemanticModel> SemanticModel::Copy() const {
	auto result = make_shared_ptr<SemanticModel>();
	result->name = name;
	result->definition = definition ? definition->Copy() : nullptr;
	result->definition_sql = definition_sql;
	result->created_on = created_on;
	result->comment = comment;
	for (auto &table : tables) {
		SemanticTable copy;
		copy.name = table.name;
		copy.source = table.source->Copy();
		copy.catalog_name = table.catalog_name;
		copy.schema_name = table.schema_name;
		copy.table_name = table.table_name;
		copy.column_names = table.column_names;
		copy.column_types = table.column_types;
		copy.columns = table.columns;
		copy.primary_key = table.primary_key;
		copy.unique_keys = table.unique_keys;
		copy.range_constraints = table.range_constraints;
		copy.synonyms = table.synonyms;
		copy.comment = table.comment;
		result->tables.push_back(std::move(copy));
	}
	result->relationships = relationships;
	for (auto &member : members) {
		SemanticMember copy;
		copy.kind = member.kind;
		copy.table = member.table;
		copy.name = member.name;
		copy.is_private = member.is_private;
		copy.expression = member.expression->Copy();
		copy.resolved = member.resolved ? member.resolved->Copy() : nullptr;
		copy.type = member.type;
		copy.parsed = member.parsed.Copy();
		copy.using_relationships = member.using_relationships;
		result->members.push_back(std::move(copy));
	}
	result->edges = edges;
	result->path_count = path_count;
	return result;
}

//===--------------------------------------------------------------------===//
// Lookups
//===--------------------------------------------------------------------===//
optional_idx SemanticModel::FindTable(const string &table_name) const {
	for (idx_t i = 0; i < tables.size(); i++) {
		if (StringUtil::CIEquals(tables[i].name, table_name)) {
			return i;
		}
	}
	return optional_idx();
}

optional_idx SemanticModel::FindMember(idx_t table, const string &member_name) const {
	for (idx_t i = 0; i < members.size(); i++) {
		if (members[i].table == table && StringUtil::CIEquals(members[i].name, member_name)) {
			return i;
		}
	}
	return optional_idx();
}

vector<idx_t> SemanticModel::FindMembersByName(const string &member_name) const {
	vector<idx_t> result;
	for (idx_t i = 0; i < members.size(); i++) {
		if (StringUtil::CIEquals(members[i].name, member_name)) {
			result.push_back(i);
		}
	}
	return result;
}

string SemanticModel::QualifiedViewName() const {
	return name.name;
}

string SemanticModel::KindDisplayName(SemanticMemberKind kind) {
	switch (kind) {
	case SemanticMemberKind::FACT:
		return "Fact";
	case SemanticMemberKind::DIMENSION:
		return "Dimension";
	case SemanticMemberKind::METRIC:
		return "Metric";
	}
	return "Member";
}

string SemanticModel::MemberDisplayName(idx_t member_idx) const {
	auto &member = members[member_idx];
	if (member.IsDerived()) {
		return member.name;
	}
	return tables[member.table].name + "." + member.name;
}

//===--------------------------------------------------------------------===//
// Graph
//===--------------------------------------------------------------------===//
vector<SemanticEdge> SemanticModel::Path(idx_t from, idx_t to, const vector<idx_t> &using_relationships) const {
	if (from == to) {
		return {};
	}
	if (path_count[from][to] == 0) {
		throw BinderException("Semantic view \"%s\": logical table \"%s\" is not reachable from \"%s\" through "
		                      "many-to-one relationships",
		                      QualifiedViewName(), tables[to].name, tables[from].name);
	}
	// an edge is allowed unless its target is the target of a USING relationship and it is not one of them
	auto allowed = [&](const SemanticEdge &edge) {
		bool target_pinned = false;
		for (auto rel : using_relationships) {
			auto &pinned = relationships[rel];
			if (rel == edge.relationship) {
				return true;
			}
			target_pinned = target_pinned || pinned.to_table == edge.target;
		}
		return !target_pinned;
	};
	vector<vector<SemanticEdge>> found;
	vector<SemanticEdge> path;
	vector<bool> visited(tables.size(), false);
	std::function<void(idx_t)> dfs = [&](idx_t node) {
		if (found.size() > 1) {
			return;
		}
		if (node == to) {
			found.push_back(path);
			return;
		}
		visited[node] = true;
		for (auto &edge : edges[node]) {
			if (visited[edge.target] || !allowed(edge)) {
				continue;
			}
			path.push_back(edge);
			dfs(edge.target);
			path.pop_back();
		}
		visited[node] = false;
	};
	dfs(from);
	if (found.empty()) {
		throw BinderException("Semantic view \"%s\": the USING relationships of the metric do not connect \"%s\" to "
		                      "\"%s\"",
		                      QualifiedViewName(), tables[from].name, tables[to].name);
	}
	if (found.size() > 1) {
		throw BinderException("Semantic view \"%s\": multiple relationship paths from \"%s\" to \"%s\"; specify "
		                      "USING (<relationship>) on the metric to choose one",
		                      QualifiedViewName(), tables[from].name, tables[to].name);
	}
	return found[0];
}

//===--------------------------------------------------------------------===//
// Markers
//===--------------------------------------------------------------------===//
unique_ptr<ParsedExpression> SemanticModel::MakeColumnMarker(idx_t table, const string &column) {
	vector<string> names {SEMANTIC_COLUMN_MARKER, to_string(table), column};
	return make_uniq<ColumnRefExpression>(std::move(names));
}

unique_ptr<ParsedExpression> SemanticModel::MakeMemberMarker(idx_t member) {
	vector<string> names {SEMANTIC_MEMBER_MARKER, to_string(member)};
	return make_uniq<ColumnRefExpression>(std::move(names));
}

bool SemanticModel::IsColumnMarker(const ParsedExpression &expr, idx_t &table, string &column) {
	if (expr.GetExpressionClass() != ExpressionClass::COLUMN_REF) {
		return false;
	}
	auto &colref = expr.Cast<ColumnRefExpression>();
	if (colref.column_names.size() != 3 || colref.column_names[0] != SEMANTIC_COLUMN_MARKER) {
		return false;
	}
	table = std::stoull(colref.column_names[1]);
	column = colref.column_names[2];
	return true;
}

bool SemanticModel::IsMemberMarker(const ParsedExpression &expr, idx_t &member) {
	if (expr.GetExpressionClass() != ExpressionClass::COLUMN_REF) {
		return false;
	}
	auto &colref = expr.Cast<ColumnRefExpression>();
	if (colref.column_names.size() != 2 || colref.column_names[0] != SEMANTIC_MEMBER_MARKER) {
		return false;
	}
	member = std::stoull(colref.column_names[1]);
	return true;
}

//===--------------------------------------------------------------------===//
// Name resolution
//===--------------------------------------------------------------------===//
static string ErrorPrefix(const SemanticResolveContext &context) {
	return context.what.empty() ? string() : context.what + ": ";
}

static unique_ptr<ParsedExpression> ResolveColumnRef(const SemanticModel &model, const ColumnRefExpression &colref,
                                                     const SemanticResolveContext &context) {
	auto check_private = [&](idx_t member) {
		if (!context.allow_private && model.members[member].is_private) {
			throw BinderException("%sSemantic expression \"%s\" is PRIVATE and cannot be queried", ErrorPrefix(context),
			                      model.MemberDisplayName(member));
		}
		return member;
	};
	auto &names = colref.column_names;
	if (names.size() == 1) {
		auto &name = names[0];
		// 1. member of the owning table
		if (context.owner_table != DConstants::INVALID_INDEX) {
			auto member = model.FindMember(context.owner_table, name);
			if (member.IsValid() && member.GetIndex() != context.self_member) {
				return SemanticModel::MakeMemberMarker(check_private(member.GetIndex()));
			}
		}
		// 2. a unique member name in the view
		vector<idx_t> candidates;
		for (auto member : model.FindMembersByName(name)) {
			if (member != context.self_member) {
				candidates.push_back(member);
			}
		}
		if (candidates.size() > 1) {
			throw BinderException("%sAmbiguous name \"%s\"; qualify it with its logical table", ErrorPrefix(context),
			                      name);
		}
		if (candidates.size() == 1) {
			return SemanticModel::MakeMemberMarker(check_private(candidates[0]));
		}
		// 3. a column of the owning table
		if (context.owner_table != DConstants::INVALID_INDEX) {
			auto &table = model.tables[context.owner_table];
			auto entry = table.columns.find(name);
			if (entry != table.columns.end()) {
				return SemanticModel::MakeColumnMarker(context.owner_table, table.column_names[entry->second]);
			}
		} else {
			// 4. a column of exactly one logical table (view-level expressions)
			idx_t found_table = DConstants::INVALID_INDEX;
			string found_column;
			for (idx_t t = 0; t < model.tables.size(); t++) {
				auto entry = model.tables[t].columns.find(name);
				if (entry != model.tables[t].columns.end()) {
					if (found_table != DConstants::INVALID_INDEX) {
						throw BinderException("%sAmbiguous name \"%s\"; qualify it with its logical table",
						                      ErrorPrefix(context), name);
					}
					found_table = t;
					found_column = model.tables[t].column_names[entry->second];
				}
			}
			if (found_table != DConstants::INVALID_INDEX) {
				return SemanticModel::MakeColumnMarker(found_table, found_column);
			}
		}
		throw BinderException("%sUnknown column or semantic expression \"%s\"", ErrorPrefix(context), name);
	}
	if (names.size() == 2) {
		auto table = model.FindTable(names[0]);
		if (!table.IsValid()) {
			throw BinderException("%sUnknown logical table \"%s\" in \"%s.%s\"", ErrorPrefix(context), names[0],
			                      names[0], names[1]);
		}
		auto member = model.FindMember(table.GetIndex(), names[1]);
		if (member.IsValid() && member.GetIndex() != context.self_member) {
			return SemanticModel::MakeMemberMarker(check_private(member.GetIndex()));
		}
		auto &table_entry = model.tables[table.GetIndex()];
		auto entry = table_entry.columns.find(names[1]);
		if (entry != table_entry.columns.end()) {
			return SemanticModel::MakeColumnMarker(table.GetIndex(), table_entry.column_names[entry->second]);
		}
		throw BinderException("%sUnknown column or semantic expression \"%s.%s\"", ErrorPrefix(context), names[0],
		                      names[1]);
	}
	throw BinderException("%sUnsupported reference \"%s\": use <logical table>.<name>", ErrorPrefix(context),
	                      colref.ToString());
}

static void ResolveRecursive(const SemanticModel &model, unique_ptr<ParsedExpression> &expr,
                             const SemanticResolveContext &context) {
	if (expr->GetExpressionClass() == ExpressionClass::COLUMN_REF) {
		auto &colref = expr->Cast<ColumnRefExpression>();
		idx_t ignored_idx;
		string ignored_name;
		if (SemanticModel::IsColumnMarker(colref, ignored_idx, ignored_name) ||
		    SemanticModel::IsMemberMarker(colref, ignored_idx)) {
			return;
		}
		auto alias = expr->alias;
		expr = ResolveColumnRef(model, colref, context);
		expr->alias = alias;
		return;
	}
	if (expr->GetExpressionClass() == ExpressionClass::STAR) {
		throw BinderException("%s'*' is not supported here", ErrorPrefix(context));
	}
	// subqueries are left untouched (they must be uncorrelated); their IN-operand is resolved
	ParsedExpressionIterator::EnumerateChildren(
	    *expr, [&](unique_ptr<ParsedExpression> &child) { ResolveRecursive(model, child, context); });
}

unique_ptr<ParsedExpression> SemanticModel::Resolve(const ParsedExpression &expr,
                                                    const SemanticResolveContext &context) const {
	auto result = expr.Copy();
	ResolveRecursive(*this, result, context);
	return result;
}

} // namespace duckdb
