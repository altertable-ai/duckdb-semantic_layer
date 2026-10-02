#include "duckdb/catalog/catalog.hpp"
#include "duckdb/catalog/catalog_entry.hpp"
#include "duckdb/catalog/catalog_entry/schema_catalog_entry.hpp"
#include "duckdb/catalog/entry_lookup_info.hpp"
#include "duckdb/common/exception.hpp"
#include "duckdb/common/exception/binder_exception.hpp"
#include "duckdb/common/exception/catalog_exception.hpp"
#include "duckdb/common/to_string.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/main/client_context.hpp"
#include "duckdb/parser/expression/star_expression.hpp"
#include "duckdb/parser/parsed_expression_iterator.hpp"
#include "duckdb/parser/query_node/select_node.hpp"
#include "duckdb/parser/statement/select_statement.hpp"
#include "duckdb/parser/tableref/basetableref.hpp"
#include "duckdb/parser/tableref/subqueryref.hpp"
#include "duckdb/planner/binder.hpp"
#include "semantic_analysis.hpp"
#include "semantic_model.hpp"
#include "semantic_query_compiler.hpp"

namespace duckdb {

namespace {

class SemanticModelBuilder {
public:
	SemanticModelBuilder(ClientContext &context, const ParsedSemanticView &definition, const QualifiedName &name)
	    : context(context), definition(definition), model(make_shared_ptr<SemanticModel>()) {
		model->name = name;
	}

	shared_ptr<SemanticModel> Build() {
		if (definition.dimensions.empty() && definition.metrics.empty()) {
			Fail("A semantic view must define at least one dimension or metric");
		}
		model->definition = definition.Copy();
		model->definition->name = model->name;
		model->definition_sql = definition.ToSQL();
		model->comment = definition.comment;
		model->created_on = timestamp_tz_t(Timestamp::GetCurrentTimestamp());
		BuildTables();
		BuildRelationships();
		BuildGraph();
		BuildMembers();
		ResolveMembers();
		CheckMemberCycles();
		CheckMemberKinds();
		ProbeSemanticModelTypes(context, *model);
		return model;
	}

private:
	template <typename... ARGS>
	[[noreturn]] void Fail(const string &message, ARGS... params) {
		throw BinderException("Invalid semantic view \"%s\": " + message, model->name.name, params...);
	}

	string ResolveColumn(const SemanticTable &table, const string &column, const string &what) {
		auto entry = table.columns.find(column);
		if (entry == table.columns.end()) {
			Fail("%s column \"%s\" does not exist in logical table \"%s\"", what, column, table.name);
		}
		return table.column_names[entry->second];
	}

	vector<string> ResolveColumns(const SemanticTable &table, const vector<string> &columns, const string &what) {
		vector<string> result;
		for (auto &column : columns) {
			result.push_back(ResolveColumn(table, column, what));
		}
		return result;
	}

	void BindColumns(SemanticTable &table) {
		auto node = make_uniq<SelectNode>();
		node->select_list.push_back(make_uniq<StarExpression>());
		node->from_table = table.source->Copy();
		SelectStatement statement;
		statement.node = std::move(node);
		auto binder = Binder::CreateBinder(context);
		auto bound = binder->Bind(statement.Cast<SQLStatement>());
		table.column_names = bound.names;
		table.column_types = bound.types;
		for (idx_t i = 0; i < table.column_names.size(); i++) {
			table.columns[table.column_names[i]] = i;
		}
	}

	void BuildTables() {
		for (auto &parsed : definition.tables) {
			SemanticTable table;
			if (parsed.source->type == TableReferenceType::BASE_TABLE) {
				auto &base = parsed.source->Cast<BaseTableRef>();
				EntryLookupInfo lookup(CatalogType::TABLE_ENTRY, base.table_name);
				auto &entry = Catalog::GetEntry(context, base.catalog_name, base.schema_name, lookup);
				table.catalog_name = entry.ParentCatalog().GetName();
				table.schema_name = entry.ParentSchema().name;
				table.table_name = entry.name;
				auto qualified = make_uniq<BaseTableRef>();
				qualified->catalog_name = table.catalog_name;
				qualified->schema_name = table.schema_name;
				qualified->table_name = table.table_name;
				table.source = std::move(qualified);
				table.name = parsed.alias.empty() ? base.table_name : parsed.alias;
			} else {
				if (parsed.alias.empty()) {
					Fail("a logical table defined by a query needs an alias (<alias> AS (<query>))");
				}
				table.source = parsed.source->Copy();
				table.name = parsed.alias;
			}
			if (model->FindTable(table.name).IsValid()) {
				Fail("Duplicate logical table name \"%s\"", table.name);
			}
			BindColumns(table);
			table.primary_key = ResolveColumns(table, parsed.primary_key, "PRIMARY KEY");
			for (auto &unique : parsed.unique_keys) {
				table.unique_keys.push_back(ResolveColumns(table, unique, "UNIQUE"));
			}
			for (auto &range : parsed.range_constraints) {
				ParsedSemanticRangeConstraint constraint;
				constraint.name = range.name;
				constraint.start_column = ResolveColumn(table, range.start_column, "DISTINCT RANGE");
				constraint.end_column = ResolveColumn(table, range.end_column, "DISTINCT RANGE");
				if (StringUtil::CIEquals(constraint.start_column, constraint.end_column)) {
					Fail("DISTINCT RANGE on logical table \"%s\" needs two different columns", table.name);
				}
				table.range_constraints.push_back(std::move(constraint));
			}
			table.synonyms = parsed.synonyms;
			table.comment = parsed.comment;
			model->tables.push_back(std::move(table));
		}
	}

	idx_t RequireTable(const string &name, const string &where) {
		auto table = model->FindTable(name);
		if (!table.IsValid()) {
			Fail("Unknown logical table \"%s\" in %s", name, where);
		}
		return table.GetIndex();
	}

	void BuildRelationships() {
		case_insensitive_set_t names;
		for (auto &parsed : definition.relationships) {
			SemanticRelationship rel;
			rel.from_table = RequireTable(parsed.from_table, "RELATIONSHIPS");
			rel.to_table = RequireTable(parsed.to_table, "RELATIONSHIPS");
			auto &from = model->tables[rel.from_table];
			auto &to = model->tables[rel.to_table];
			rel.name = parsed.name;
			if (rel.name.empty()) {
				rel.name = from.name + "_to_" + to.name;
				auto base_name = rel.name;
				for (idx_t n = 2; names.count(rel.name); n++) {
					rel.name = base_name + "_" + to_string(n);
				}
			}
			if (names.count(rel.name)) {
				Fail("Duplicate relationship name \"%s\"", rel.name);
			}
			names.insert(rel.name);
			if (rel.from_table == rel.to_table) {
				Fail("Relationship \"%s\": logical table \"%s\" cannot reference itself", rel.name, from.name);
			}
			rel.from_columns = ResolveColumns(from, parsed.from_columns, "Relationship");
			if (parsed.is_range) {
				rel.is_range = true;
				rel.range_start = ResolveColumn(to, parsed.range_start, "Relationship");
				rel.range_end = ResolveColumn(to, parsed.range_end, "Relationship");
				if (rel.from_columns.size() != 1) {
					Fail("Relationship \"%s\": a range relationship takes exactly one column", rel.name);
				}
				bool has_constraint = false;
				for (auto &range : to.range_constraints) {
					if (StringUtil::CIEquals(range.start_column, rel.range_start) &&
					    StringUtil::CIEquals(range.end_column, rel.range_end)) {
						has_constraint = true;
					}
				}
				if (!has_constraint) {
					Fail("Relationship \"%s\": logical table \"%s\" needs a CONSTRAINT ... DISTINCT RANGE BETWEEN %s "
					     "AND %s EXCLUSIVE",
					     rel.name, to.name, rel.range_start, rel.range_end);
				}
			} else {
				if (parsed.to_columns.empty()) {
					if (!to.primary_key.empty()) {
						rel.to_columns = to.primary_key;
					} else if (to.unique_keys.size() == 1) {
						rel.to_columns = to.unique_keys[0];
					} else {
						Fail("Relationship \"%s\": logical table \"%s\" has no PRIMARY KEY or single UNIQUE key; "
						     "specify the referenced columns",
						     rel.name, to.name);
					}
				} else {
					rel.to_columns = ResolveColumns(to, parsed.to_columns, "Relationship");
					rel.asof_index = parsed.asof_index;
					vector<string> key_columns;
					for (idx_t i = 0; i < rel.to_columns.size(); i++) {
						if (i != rel.asof_index) {
							key_columns.push_back(rel.to_columns[i]);
						}
					}
					if (rel.asof_index == DConstants::INVALID_INDEX && !to.IsKey(rel.to_columns)) {
						Fail("Relationship \"%s\": referenced columns (%s) of logical table \"%s\" must be the "
						     "PRIMARY KEY or a UNIQUE key",
						     rel.name, StringUtil::Join(rel.to_columns, ", "), to.name);
					}
				}
				if (rel.from_columns.size() != rel.to_columns.size()) {
					Fail("Relationship \"%s\": column count mismatch between %s (%d) and %s (%d)", rel.name, from.name,
					     rel.from_columns.size(), to.name, rel.to_columns.size());
				}
			}
			rel.one_to_one = !rel.is_range && rel.asof_index == DConstants::INVALID_INDEX && from.IsKey(rel.from_columns);
			model->relationships.push_back(std::move(rel));
		}
	}

	void BuildGraph() {
		auto table_count = model->tables.size();
		model->edges.resize(table_count);
		vector<vector<idx_t>> declared(table_count);
		for (idx_t r = 0; r < model->relationships.size(); r++) {
			auto &rel = model->relationships[r];
			model->edges[rel.from_table].push_back(SemanticEdge {r, false, rel.to_table});
			declared[rel.from_table].push_back(rel.to_table);
			if (rel.one_to_one) {
				model->edges[rel.to_table].push_back(SemanticEdge {r, true, rel.from_table});
			}
		}
		// no circular relationships (declared direction)
		vector<int> state(table_count, 0);
		vector<idx_t> stack;
		std::function<void(idx_t)> visit = [&](idx_t node) {
			state[node] = 1;
			stack.push_back(node);
			for (auto next : declared[node]) {
				if (state[next] == 1) {
					vector<string> cycle;
					bool in_cycle = false;
					for (auto entry : stack) {
						in_cycle = in_cycle || entry == next;
						if (in_cycle) {
							cycle.push_back(model->tables[entry].name);
						}
					}
					cycle.push_back(model->tables[next].name);
					Fail("Relationships form a circular path: %s", StringUtil::Join(cycle, " -> "));
				}
				if (state[next] == 0) {
					visit(next);
				}
			}
			stack.pop_back();
			state[node] = 2;
		};
		for (idx_t t = 0; t < table_count; t++) {
			if (state[t] == 0) {
				visit(t);
			}
		}
		// count simple paths (capped at 2)
		model->path_count.assign(table_count, vector<idx_t>(table_count, 0));
		for (idx_t source = 0; source < table_count; source++) {
			auto &counts = model->path_count[source];
			vector<bool> visited(table_count, false);
			idx_t steps = 0;
			std::function<void(idx_t)> walk = [&](idx_t node) {
				if (++steps > 100000) {
					Fail("relationship graph is too complex");
				}
				counts[node] = MinValue<idx_t>(counts[node] + 1, 2);
				visited[node] = true;
				for (auto &edge : model->edges[node]) {
					if (!visited[edge.target]) {
						walk(edge.target);
					}
				}
				visited[node] = false;
			};
			walk(source);
			counts[source] = 1;
		}
	}

	void AddMembers(const vector<ParsedSemanticMember> &parsed_members) {
		for (auto &parsed : parsed_members) {
			SemanticMember member;
			member.kind = parsed.kind;
			member.name = parsed.name;
			member.is_private = parsed.is_private;
			member.expression = parsed.expression->Copy();
			member.parsed = parsed.Copy();
			if (parsed.table.empty()) {
				member.table = DConstants::INVALID_INDEX;
				for (auto &existing : model->members) {
					if (existing.kind == SemanticMemberKind::METRIC && StringUtil::CIEquals(existing.name, member.name)) {
						Fail("Derived metric \"%s\" has the same name as another metric", member.name);
					}
				}
			} else {
				member.table = RequireTable(parsed.table, "\"" + parsed.FullName() + "\"");
			}
			for (auto &existing : model->members) {
				if (existing.table == member.table && StringUtil::CIEquals(existing.name, member.name)) {
					Fail("Duplicate semantic expression \"%s\"", parsed.FullName());
				}
				if (existing.IsDerived() && member.kind == SemanticMemberKind::METRIC &&
				    StringUtil::CIEquals(existing.name, member.name)) {
					Fail("Metric \"%s\" has the same name as a derived metric", parsed.FullName());
				}
			}
			if (!parsed.using_relationships.empty()) {
				throw NotImplementedException("USING (...) on metrics is not supported yet");
			}
			if (!parsed.non_additive_by.empty()) {
				throw NotImplementedException("NON ADDITIVE BY is not supported yet");
			}
			model->members.push_back(std::move(member));
		}
	}

	void BuildMembers() {
		AddMembers(definition.facts);
		AddMembers(definition.dimensions);
		AddMembers(definition.metrics);
	}

	string What(idx_t member_idx) {
		auto &member = model->members[member_idx];
		auto kind = SemanticModel::KindDisplayName(member.kind);
		if (member.IsDerived()) {
			kind = "Derived metric";
		}
		return kind + " \"" + model->MemberDisplayName(member_idx) + "\"";
	}

	void ResolveMembers() {
		for (idx_t m = 0; m < model->members.size(); m++) {
			auto &member = model->members[m];
			SemanticResolveContext resolve_context;
			resolve_context.owner_table = member.table;
			resolve_context.self_member = m;
			resolve_context.what = What(m);
			member.resolved = model->Resolve(*member.expression, resolve_context);
		}
	}

	static void CollectMemberRefs(const ParsedExpression &expr, vector<idx_t> &out) {
		idx_t member;
		if (SemanticModel::IsMemberMarker(expr, member)) {
			out.push_back(member);
			return;
		}
		ParsedExpressionIterator::EnumerateChildren(expr,
		                                            [&](const ParsedExpression &child) { CollectMemberRefs(child, out); });
	}

	void CheckMemberCycles() {
		auto count = model->members.size();
		vector<vector<idx_t>> deps(count);
		for (idx_t m = 0; m < count; m++) {
			CollectMemberRefs(*model->members[m].resolved, deps[m]);
		}
		vector<int> state(count, 0);
		vector<idx_t> stack;
		std::function<void(idx_t)> visit = [&](idx_t node) {
			state[node] = 1;
			stack.push_back(node);
			for (auto next : deps[node]) {
				if (state[next] == 1) {
					vector<string> cycle;
					bool in_cycle = false;
					for (auto entry : stack) {
						in_cycle = in_cycle || entry == next;
						if (in_cycle) {
							cycle.push_back(model->MemberDisplayName(entry));
						}
					}
					cycle.push_back(model->MemberDisplayName(next));
					Fail("Circular reference between semantic expressions: %s", StringUtil::Join(cycle, " -> "));
				}
				if (state[next] == 0) {
					visit(next);
				}
			}
			stack.pop_back();
			state[node] = 2;
		};
		for (idx_t m = 0; m < count; m++) {
			if (state[m] == 0) {
				visit(m);
			}
		}
	}

	//! Child aggregations in a row-level expression on `table` must aggregate rows of finer tables
	void CheckChildAggregations(SemanticAnalyzer &analyzer, const ParsedExpression &expr, idx_t table,
	                            const string &what) {
		if (analyzer.IsAggregate(expr)) {
			auto home = analyzer.AggregateHome(expr.Cast<FunctionExpression>(), table, what);
			if (home == table || !model->Reaches(home, table) || model->Reaches(table, home)) {
				Fail("%s cannot contain an aggregate over rows of logical table \"%s\"; only rows of tables with "
				     "a higher granularity (that reference \"%s\") can be aggregated",
				     what, model->tables[home].name, model->tables[table].name);
			}
			return;
		}
		idx_t member;
		if (SemanticModel::IsMemberMarker(expr, member)) {
			auto &entry = model->members[member];
			if (entry.kind == SemanticMemberKind::METRIC) {
				vector<SemanticAtom> atoms;
				analyzer.Decompose(*entry.resolved, entry.table, atoms, what);
				for (auto &atom : atoms) {
					if (atom.home == table || !model->Reaches(atom.home, table) || model->Reaches(table, atom.home)) {
						Fail("%s cannot reference metric \"%s\": its logical table must have a higher granularity "
						     "than \"%s\"",
						     what, model->MemberDisplayName(member), model->tables[table].name);
					}
				}
			}
			return;
		}
		ParsedExpressionIterator::EnumerateChildren(expr, [&](const ParsedExpression &child) {
			CheckChildAggregations(analyzer, child, table, what);
		});
	}

	void CheckMemberKinds() {
		SemanticAnalyzer analyzer(*model, context);
		for (idx_t m = 0; m < model->members.size(); m++) {
			auto &member = model->members[m];
			auto what = What(m);
			if (member.IsRowLevel()) {
				CheckChildAggregations(analyzer, *member.resolved, member.table, what);
				set<idx_t> tables;
				analyzer.RowTables(*member.resolved, member.table, tables, what);
				for (auto table : tables) {
					if (!model->Reaches(member.table, table)) {
						Fail("%s references logical table \"%s\", which is not reachable from \"%s\" through "
						     "many-to-one relationships",
						     what, model->tables[table].name, model->tables[member.table].name);
					}
				}
				continue;
			}
			if (!analyzer.ContainsAggregateOrMetric(*member.resolved)) {
				Fail("%s must use an aggregate function", what);
			}
			vector<SemanticAtom> atoms;
			analyzer.Decompose(*member.resolved, member.table, atoms, what);
			if (member.IsDerived()) {
				continue;
			}
			for (auto &atom : atoms) {
				if (!model->Reaches(atom.home, member.table) && !model->Reaches(member.table, atom.home)) {
					Fail("%s aggregates rows of logical table \"%s\", which is not related to \"%s\"", what,
					     model->tables[atom.home].name, model->tables[member.table].name);
				}
			}
		}
	}

private:
	ClientContext &context;
	const ParsedSemanticView &definition;
	shared_ptr<SemanticModel> model;
};

} // namespace

shared_ptr<SemanticModel> BuildSemanticModel(ClientContext &context, const ParsedSemanticView &definition,
                                             const QualifiedName &resolved_name) {
	SemanticModelBuilder builder(context, definition, resolved_name);
	return builder.Build();
}

} // namespace duckdb
