#include "semantic_definition_exception.hpp"
#include "semantic_model.hpp"
#include "semantic_ast_helpers.hpp"

#include "duckdb/common/exception.hpp"
#include "duckdb/common/exception/binder_exception.hpp"
#include "duckdb/common/exception/catalog_exception.hpp"
#include "duckdb/common/to_string.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/parser/expression/function_expression.hpp"
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
		if (members[i].table == table && StringUtil::CIEquals(members[i].Name(), member_name)) {
			return i;
		}
	}
	return optional_idx();
}

vector<idx_t> SemanticModel::FindMembersByName(const string &member_name) const {
	vector<idx_t> result;
	for (idx_t i = 0; i < members.size(); i++) {
		if (StringUtil::CIEquals(members[i].Name(), member_name)) {
			result.push_back(i);
		}
	}
	return result;
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
		return member.Name();
	}
	return tables[member.table].name + "." + member.Name();
}

//===--------------------------------------------------------------------===//
// Graph
//===--------------------------------------------------------------------===//
optional_idx SemanticModel::FinestTable(const set<idx_t> &tables) const {
	for (auto candidate : tables) {
		bool reaches_all = true;
		for (auto other : tables) {
			reaches_all = reaches_all && Reaches(candidate, other);
		}
		if (reaches_all) {
			return candidate;
		}
	}
	return optional_idx();
}

vector<vector<SemanticEdge>>
SemanticModel::FindPaths(idx_t from, idx_t to, const std::function<bool(const SemanticEdge &)> &allowed,
                         const std::function<bool(const vector<SemanticEdge> &)> &accept, bool skip_detours) const {
	vector<vector<SemanticEdge>> found;
	vector<SemanticEdge> path;
	vector<bool> visited(tables.size(), false);
	std::function<void(idx_t)> dfs = [&](idx_t node) {
		if (found.size() > 1) {
			return;
		}
		if (node == to) {
			if (!accept || accept(path)) {
				found.push_back(path);
			}
			return;
		}
		visited[node] = true;
		// a detour reaches a table that an earlier table of the path (the visited ones) references directly
		auto is_detour = [&](idx_t target) {
			for (idx_t earlier = 0; earlier < tables.size(); earlier++) {
				if (!visited[earlier] || earlier == node) {
					continue;
				}
				for (auto &direct : edges[earlier]) {
					if (direct.target == target && allowed(direct)) {
						return true;
					}
				}
			}
			return false;
		};
		for (auto &edge : edges[node]) {
			if (visited[edge.target] || !allowed(edge) || (skip_detours && is_detour(edge.target))) {
				continue;
			}
			path.push_back(edge);
			dfs(edge.target);
			path.pop_back();
		}
		visited[node] = false;
	};
	dfs(from);
	return found;
}

SemanticPathResult SemanticModel::TryPath(idx_t from, idx_t to, const vector<idx_t> &using_relationships,
                                          vector<SemanticEdge> &result) const {
	result.clear();
	if (from == to) {
		return SemanticPathResult::FOUND;
	}
	if (path_count[from][to] == 0) {
		return SemanticPathResult::UNREACHABLE;
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
	auto found = FindPaths(from, to, allowed);
	if (found.empty()) {
		return SemanticPathResult::EXCLUDED_BY_USING;
	}
	if (found.size() > 1 && !using_relationships.empty()) {
		// the paths through a USING relationship: USING (fa) picks the TIME reached through fa
		auto through_using = FindPaths(from, to, allowed, [&](const vector<SemanticEdge> &path) {
			for (auto &edge : path) {
				for (auto rel : using_relationships) {
					if (edge.relationship == rel) {
						return true;
					}
				}
			}
			return false;
		});
		if (through_using.size() == 1) {
			found = std::move(through_using);
		}
	}
	if (found.size() > 1) {
		// Default relationships (TIME) settle it: the table's own wins, then the only path entering `to` through one
		auto is_default = [&](const SemanticEdge &edge) {
			return !edge.reversed && relationships[edge.relationship].is_default;
		};
		for (auto &edge : edges[from]) {
			if (edge.target == to && is_default(edge) && allowed(edge)) {
				result.push_back(edge);
				return SemanticPathResult::FOUND;
			}
		}
		found = FindPaths(from, to, [&](const SemanticEdge &edge) {
			return allowed(edge) && (edge.target != to || is_default(edge));
		});
		if (found.size() != 1) {
			// still ambiguous: a relationship that references the target directly wins over a detour
			found = FindPaths(from, to, allowed, nullptr, true);
			if (found.size() != 1) {
				return SemanticPathResult::AMBIGUOUS;
			}
		}
	}
	result = std::move(found[0]);
	return SemanticPathResult::FOUND;
}

SemanticEdge SemanticModel::TimeEdge(idx_t from, const vector<idx_t> &using_relationships) const {
	D_ASSERT(time_spine.IsValid() && from != time_spine.GetIndex());
	return Path(from, time_spine.GetIndex(), using_relationships).back();
}

vector<SemanticEdge> SemanticModel::Path(idx_t from, idx_t to, const vector<idx_t> &using_relationships) const {
	vector<SemanticEdge> path;
	switch (TryPath(from, to, using_relationships, path)) {
	case SemanticPathResult::FOUND:
		return path;
	case SemanticPathResult::UNREACHABLE:
		throw SemanticDefinitionException(
		    "Semantic view \"%s\": logical table \"%s\" is not reachable from \"%s\" through "
		    "many-to-one relationships",
		    Name().name, tables[to].name, tables[from].name);
	case SemanticPathResult::EXCLUDED_BY_USING:
		throw SemanticDefinitionException(
		    "Semantic view \"%s\": the USING relationships of the metric do not connect \"%s\" to "
		    "\"%s\"",
		    Name().name, tables[from].name, tables[to].name);
	case SemanticPathResult::AMBIGUOUS:
		throw SemanticDefinitionException(
		    "Semantic view \"%s\": multiple relationship paths from \"%s\" to \"%s\"; specify "
		    "USING (<relationship>) on the metric to choose one",
		    Name().name, tables[from].name, tables[to].name);
	}
	throw InternalException("unknown SemanticPathResult");
}

//===--------------------------------------------------------------------===//
// Name resolution
//===--------------------------------------------------------------------===//
static string ErrorPrefix(const SemanticResolveContext &context) {
	return context.what.empty() ? string() : context.what + ": ";
}

SemanticRef SemanticModel::Lookup(const vector<string> &names, const SemanticResolveContext &context,
                                  string &error) const {
	SemanticRef result;
	auto member_ref = [&](idx_t member) {
		result.kind = SemanticRefKind::MEMBER;
		result.index = member;
		return result;
	};
	auto column_ref = [&](idx_t table, idx_t column) {
		result.kind = SemanticRefKind::COLUMN;
		result.index = table;
		result.column = tables[table].column_names[column];
		return result;
	};
	auto fail = [&](const string &message) {
		error = message;
		return result;
	};
	if (names.size() == 1) {
		auto &name = names[0];
		// 1. member of the owning table
		if (context.owner_table != DConstants::INVALID_INDEX) {
			auto member = FindMember(context.owner_table, name);
			if (member.IsValid() && member.GetIndex() != context.self_member) {
				return member_ref(member.GetIndex());
			}
			// 2. a physical column of the owning table, even when another table defines a member of the same name
			// (a self-named definition such as a.value AS value, or o.total AS SUM(amount) next to c.amount)
			auto entry = tables[context.owner_table].columns.find(name);
			if (entry != tables[context.owner_table].columns.end()) {
				return column_ref(context.owner_table, entry->second);
			}
		}
		// 3. a unique member name in the view
		vector<idx_t> candidates;
		for (auto member : FindMembersByName(name)) {
			if (member != context.self_member) {
				candidates.push_back(member);
			}
		}
		if (candidates.size() > 1) {
			return fail(StringUtil::Format("Ambiguous name \"%s\"; qualify it with its logical table", name));
		}
		if (candidates.size() == 1) {
			return member_ref(candidates[0]);
		}
		// 4. (view-level expressions) a column of exactly one logical table
		for (idx_t t = 0; t < tables.size(); t++) {
			if (context.owner_table != DConstants::INVALID_INDEX && t != context.owner_table) {
				continue;
			}
			auto entry = tables[t].columns.find(name);
			if (entry == tables[t].columns.end()) {
				continue;
			}
			if (result.Is(SemanticRefKind::COLUMN)) {
				result = SemanticRef();
				return fail(StringUtil::Format("Ambiguous name \"%s\"; qualify it with its logical table", name));
			}
			column_ref(t, entry->second);
		}
		if (result.Is(SemanticRefKind::COLUMN)) {
			return result;
		}
		return fail(StringUtil::Format("Unknown column or semantic expression \"%s\"", name));
	}
	if (names.size() == 2) {
		auto table = FindTable(names[0]);
		if (!table.IsValid()) {
			auto message =
			    StringUtil::Format("Unknown logical table \"%s\" in \"%s.%s\"", names[0], names[0], names[1]);
			if (context.in_query) {
				message += " (a semantic query cannot reference the outer query: correlated subqueries over a "
				           "semantic view are not supported)";
			}
			return fail(message);
		}
		auto member = FindMember(table.GetIndex(), names[1]);
		if (member.IsValid() && member.GetIndex() != context.self_member) {
			return member_ref(member.GetIndex());
		}
		auto entry = tables[table.GetIndex()].columns.find(names[1]);
		if (entry != tables[table.GetIndex()].columns.end()) {
			return column_ref(table.GetIndex(), entry->second);
		}
		return fail(StringUtil::Format("Unknown column or semantic expression \"%s.%s\"", names[0], names[1]));
	}
	return fail(
	    StringUtil::Format("Unsupported reference \"%s\": use <logical table>.<name>", StringUtil::Join(names, ".")));
}

//! DuckDB's keyword functions written without parentheses: a copy of the table of Binder::GetSQLValueFunction
//! (planner/column_qualifier.cpp), which needs a binder that model resolution does not have
static string SQLValueFunctionName(const string &name) {
	static const case_insensitive_map_t<string> FUNCTIONS = {{"current_catalog", "current_catalog"},
	                                                         {"current_date", "current_date"},
	                                                         {"current_schema", "current_schema"},
	                                                         {"current_role", "current_role"},
	                                                         {"current_time", "get_current_time"},
	                                                         {"current_timestamp", "get_current_timestamp"},
	                                                         {"current_user", "current_user"},
	                                                         {"localtime", "current_localtime"},
	                                                         {"localtimestamp", "current_localtimestamp"},
	                                                         {"session_user", "session_user"},
	                                                         {"user", "user"}};
	auto entry = FUNCTIONS.find(name);
	return entry == FUNCTIONS.end() ? string() : entry->second;
}

static unique_ptr<ParsedExpression> ResolveColumnRef(const SemanticModel &model, const ColumnRefExpression &colref,
                                                     const SemanticResolveContext &context) {
	string error;
	auto names = IdentifiersToStrings(colref.ColumnNames());
	auto ref = model.Lookup(names, context, error);
	if (ref.Is(SemanticRefKind::NONE) && names.size() == 1) {
		auto function = SQLValueFunctionName(names[0]);
		if (!function.empty()) {
			return make_uniq<FunctionExpression>(Identifier(function), vector<unique_ptr<ParsedExpression>>());
		}
	}
	if (ref.Is(SemanticRefKind::NONE) && names.size() >= 2) {
		// as in DuckDB, a name that is not a column may be a struct field of a shorter one (info.s, c.info.s)
		for (idx_t length = names.size() - 1; length >= 1; length--) {
			string prefix_error;
			vector<string> prefix(names.begin(), names.begin() + NumericCast<int64_t>(length));
			if (model.Lookup(prefix, context, prefix_error).Is(SemanticRefKind::NONE)) {
				continue;
			}
			auto result = ResolveColumnRef(model, ColumnRefExpression(StringsToIdentifiers(prefix)), context);
			for (idx_t field = length; field < names.size(); field++) {
				vector<unique_ptr<ParsedExpression>> arguments;
				arguments.push_back(std::move(result));
				arguments.push_back(make_uniq<ConstantExpression>(Literal::String(names[field])));
				result = make_uniq<FunctionExpression>(Identifier("struct_extract"), std::move(arguments));
			}
			return result;
		}
	}
	switch (ref.kind) {
	case SemanticRefKind::MEMBER:
		if (!context.allow_private && model.members[ref.index].IsPrivate()) {
			throw SemanticDefinitionException("%sSemantic expression \"%s\" is PRIVATE and cannot be queried",
			                                  ErrorPrefix(context), model.MemberDisplayName(ref.index));
		}
		return SemanticMarkers::Member(ref.index);
	case SemanticRefKind::COLUMN:
		return SemanticMarkers::Column(ref.index, ref.column);
	default:
		throw SemanticDefinitionException("%s%s", ErrorPrefix(context), error);
	}
}

static void ResolveRecursive(const SemanticModel &model, unique_ptr<ParsedExpression> &expr,
                             const SemanticResolveContext &context, LambdaScopes &lambdas) {
	if (lambdas.IsParameter(*expr)) {
		return;
	}
	LambdaScopes::Guard lambda(lambdas, *expr);
	if (expr->GetExpressionClass() == ExpressionClass::COLUMN_REF) {
		auto &colref = expr->Cast<ColumnRefExpression>();
		if (!SemanticMarkers::Classify(colref).Is(SemanticRefKind::NONE)) {
			return;
		}
		auto alias = expr->GetAlias();
		expr = ResolveColumnRef(model, colref, context);
		expr->SetAlias(Identifier(alias));
		return;
	}
	if (expr->GetExpressionClass() == ExpressionClass::STAR) {
		throw SemanticDefinitionException("%s'*' is not supported here", ErrorPrefix(context));
	}
	// markers are only produced after resolution (semantic_transforms.hpp)
	if (SemanticMarkers::IsTransformMarkerCall(*expr)) {
		throw SemanticDefinitionException("%sfunction \"%s\" is reserved for internal use", ErrorPrefix(context),
		                                  expr->Cast<FunctionExpression>().FunctionName().GetIdentifierName());
	}
	// subqueries are left untouched (they must be uncorrelated); their IN-operand is resolved
	ParsedExpressionIterator::EnumerateChildren(
	    *expr, [&](unique_ptr<ParsedExpression> &child) { ResolveRecursive(model, child, context, lambdas); });
}

unique_ptr<ParsedExpression> SemanticModel::Resolve(const ParsedExpression &expr,
                                                    const SemanticResolveContext &context) const {
	auto result = expr.Copy();
	LambdaScopes lambdas;
	ResolveRecursive(*this, result, context, lambdas);
	return result;
}

} // namespace duckdb
