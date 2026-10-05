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
#include "duckdb/common/error_data.hpp"
#include "duckdb/parser/expression/cast_expression.hpp"
#include "duckdb/parser/expression/constant_expression.hpp"
#include "duckdb/parser/expression/star_expression.hpp"
#include "duckdb/parser/parsed_expression_iterator.hpp"
#include "duckdb/parser/query_node/select_node.hpp"
#include "duckdb/parser/statement/select_statement.hpp"
#include "duckdb/parser/tableref/basetableref.hpp"
#include "duckdb/parser/tableref/emptytableref.hpp"
#include "duckdb/parser/tableref/subqueryref.hpp"
#include "duckdb/planner/binder.hpp"
#include "semantic_analysis.hpp"
#include "semantic_model.hpp"
#include "semantic_materialization.hpp"
#include "semantic_query_compiler.hpp"

namespace duckdb {

namespace {

//! A cycle of the directed graph (`next[node]`: the targets of node's edges), as the nodes along it with the first
//! node repeated at the end; empty when the graph is acyclic
vector<idx_t> FindCycle(const vector<vector<idx_t>> &next) {
	enum class Mark : uint8_t { UNVISITED, ON_PATH, DONE };
	vector<Mark> marks(next.size(), Mark::UNVISITED);
	vector<idx_t> path;
	vector<idx_t> cycle;
	std::function<bool(idx_t)> visit = [&](idx_t node) {
		marks[node] = Mark::ON_PATH;
		path.push_back(node);
		for (auto target : next[node]) {
			if (marks[target] == Mark::ON_PATH) {
				cycle.assign(std::find(path.begin(), path.end(), target), path.end());
				cycle.push_back(target);
				return true;
			}
			if (marks[target] == Mark::UNVISITED && visit(target)) {
				return true;
			}
		}
		path.pop_back();
		marks[node] = Mark::DONE;
		return false;
	};
	for (idx_t node = 0; node < next.size(); node++) {
		if (marks[node] == Mark::UNVISITED && visit(node)) {
			break;
		}
	}
	return cycle;
}

class SemanticModelBuilder {
public:
	SemanticModelBuilder(ClientContext &context, const ParsedSemanticView &parsed, const QualifiedName &name)
	    : context(context), model(NewModel(parsed, name)), definition(*model->definition) {
	}

	shared_ptr<SemanticModel> Build() {
		if (definition.dimensions.empty() && definition.metrics.empty()) {
			Fail("A semantic view must define at least one dimension or metric");
		}
		model->created_on = timestamp_tz_t(Timestamp::GetCurrentTimestamp());
		BuildTables();
		BuildRelationships();
		BuildGraph();
		BuildMembers();
		ResolveMembers();
		ResolveOffsets();
		CheckMemberCycles();
		CheckMemberKinds();
		ProbeSemanticModelTypes(context, *model);
		PlanSemanticMaterializations(context, *model);
		return model;
	}

private:
	static shared_ptr<SemanticModel> NewModel(const ParsedSemanticView &parsed, const QualifiedName &name) {
		auto result = make_shared_ptr<SemanticModel>();
		result->definition = parsed.Copy();
		result->definition->name = name;
		return result;
	}

	template <typename... ARGS>
	[[noreturn]] void Fail(const string &message, ARGS... params) {
		throw BinderException("Invalid semantic view \"%s\": " + message, model->Name().name, params...);
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
		case_insensitive_set_t seen;
		for (auto &column : columns) {
			auto resolved = ResolveColumn(table, column, what);
			if (!seen.insert(resolved).second) {
				Fail("Duplicate %s column \"%s\" in logical table \"%s\"", what, resolved, table.name);
			}
			result.push_back(std::move(resolved));
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
				// `x.table`: x may be a schema of the default database or a database (as in DuckDB's binder)
				auto catalog_name = base.catalog_name;
				auto schema_name = base.schema_name;
				Binder::BindSchemaOrCatalog(context, catalog_name, schema_name);
				EntryLookupInfo lookup(CatalogType::TABLE_ENTRY, base.table_name);
				auto &entry = Catalog::GetEntry(context, catalog_name, schema_name, lookup);
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
				ResolveRangeReference(parsed, rel);
			} else {
				ResolveKeyReference(parsed, rel);
			}
			rel.one_to_one =
			    !rel.is_range && rel.asof_index == DConstants::INVALID_INDEX && from.IsKey(rel.from_columns);
			model->relationships.push_back(std::move(rel));
		}
	}

	//! REFERENCES t (BETWEEN start AND end EXCLUSIVE): needs a matching DISTINCT RANGE constraint on t
	void ResolveRangeReference(const ParsedSemanticRelationship &parsed, SemanticRelationship &rel) {
		auto &to = model->tables[rel.to_table];
		rel.is_range = true;
		rel.range_start = ResolveColumn(to, parsed.range_start, "Relationship");
		rel.range_end = ResolveColumn(to, parsed.range_end, "Relationship");
		if (rel.from_columns.size() != 1) {
			Fail("Relationship \"%s\": a range relationship takes exactly one column", rel.name);
		}
		for (auto &range : to.range_constraints) {
			if (StringUtil::CIEquals(range.start_column, rel.range_start) &&
			    StringUtil::CIEquals(range.end_column, rel.range_end)) {
				return;
			}
		}
		Fail("Relationship \"%s\": logical table \"%s\" needs a CONSTRAINT ... DISTINCT RANGE BETWEEN %s AND %s "
		     "EXCLUSIVE",
		     rel.name, to.name, rel.range_start, rel.range_end);
	}

	//! REFERENCES t [(columns)]: the referenced columns default to t's key and must be a key unless ASOF
	void ResolveKeyReference(const ParsedSemanticRelationship &parsed, SemanticRelationship &rel) {
		auto &from = model->tables[rel.from_table];
		auto &to = model->tables[rel.to_table];
		if (parsed.to_columns.empty()) {
			if (!to.primary_key.empty()) {
				rel.to_columns = to.primary_key;
			} else if (to.unique_keys.size() == 1) {
				rel.to_columns = to.unique_keys[0];
			} else {
				Fail("Relationship \"%s\": logical table \"%s\" has no PRIMARY KEY or single UNIQUE key; specify the "
				     "referenced columns",
				     rel.name, to.name);
			}
		} else {
			rel.to_columns = ResolveColumns(to, parsed.to_columns, "Relationship");
			rel.asof_index = parsed.asof_index;
			if (rel.asof_index == DConstants::INVALID_INDEX && !to.IsKey(rel.to_columns)) {
				Fail("Relationship \"%s\": referenced columns (%s) of logical table \"%s\" must be the PRIMARY KEY or "
				     "a UNIQUE key",
				     rel.name, StringUtil::Join(rel.to_columns, ", "), to.name);
			}
		}
		if (rel.from_columns.size() != rel.to_columns.size()) {
			Fail("Relationship \"%s\": column count mismatch between %s (%d) and %s (%d)", rel.name, from.name,
			     rel.from_columns.size(), to.name, rel.to_columns.size());
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
		auto cycle = FindCycle(declared);
		if (!cycle.empty()) {
			vector<string> names;
			for (auto table : cycle) {
				names.push_back(model->tables[table].name);
			}
			Fail("Relationships form a circular path: %s", StringUtil::Join(names, " -> "));
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
			member.parsed = &parsed;
			if (parsed.table.empty()) {
				member.table = DConstants::INVALID_INDEX;
				for (auto &existing : model->members) {
					if (existing.Kind() == SemanticMemberKind::METRIC &&
					    StringUtil::CIEquals(existing.Name(), member.Name())) {
						Fail("Derived metric \"%s\" has the same name as another metric", member.Name());
					}
				}
			} else {
				member.table = RequireTable(parsed.table, "\"" + parsed.FullName() + "\"");
			}
			for (auto &existing : model->members) {
				if (existing.table == member.table && StringUtil::CIEquals(existing.Name(), member.Name())) {
					Fail("Duplicate semantic expression \"%s\"", parsed.FullName());
				}
				if (existing.IsDerived() && member.Kind() == SemanticMemberKind::METRIC &&
				    StringUtil::CIEquals(existing.Name(), member.Name())) {
					Fail("Metric \"%s\" has the same name as a derived metric", parsed.FullName());
				}
			}
			for (auto &rel_name : parsed.using_relationships) {
				if (member.Kind() != SemanticMemberKind::METRIC) {
					Fail("USING is only allowed on metrics (\"%s\")", parsed.FullName());
				}
				idx_t found = DConstants::INVALID_INDEX;
				for (idx_t r = 0; r < model->relationships.size(); r++) {
					if (StringUtil::CIEquals(model->relationships[r].name, rel_name)) {
						found = r;
					}
				}
				if (found == DConstants::INVALID_INDEX) {
					Fail("Unknown relationship \"%s\" in USING of \"%s\"", rel_name, parsed.FullName());
				}
				member.using_relationships.push_back(found);
			}
			if (!parsed.non_additive_by.empty() && member.Kind() != SemanticMemberKind::METRIC) {
				Fail("NON ADDITIVE BY is only allowed on metrics (\"%s\")", parsed.FullName());
			}
			if (parsed.filter && member.Kind() != SemanticMemberKind::METRIC) {
				Fail("FILTER is only allowed on metrics (\"%s\")", parsed.FullName());
			}
			if (!parsed.offset_dimension.empty() && member.Kind() != SemanticMemberKind::METRIC) {
				Fail("OFFSET is only allowed on metrics (\"%s\")", parsed.FullName());
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
		auto kind = SemanticModel::KindDisplayName(member.Kind());
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
			member.resolved = model->Resolve(member.Expression(), resolve_context);
			for (auto &order : member.parsed->non_additive_by) {
				SemanticNonAdditive entry;
				resolve_context.self_member = DConstants::INVALID_INDEX;
				entry.resolved = model->Resolve(*order.expression, resolve_context);
				entry.type = order.type == OrderType::DESCENDING ? OrderType::DESCENDING : OrderType::ASCENDING;
				entry.null_order = order.null_order;
				if (entry.null_order == OrderByNullType::ORDER_DEFAULT) {
					// Snowflake defaults: ASC NULLS LAST, DESC NULLS FIRST
					entry.null_order =
					    entry.type == OrderType::ASCENDING ? OrderByNullType::NULLS_LAST : OrderByNullType::NULLS_FIRST;
				}
				member.non_additive.push_back(std::move(entry));
			}
			if (member.parsed->filter) {
				resolve_context.self_member = DConstants::INVALID_INDEX;
				resolve_context.what = What(m) + " FILTER";
				member.filter = model->Resolve(*member.parsed->filter, resolve_context);
			}
		}
	}

	static bool ContainsColumnOrSubquery(const ParsedExpression &expr) {
		if (expr.GetExpressionClass() == ExpressionClass::COLUMN_REF ||
		    expr.GetExpressionClass() == ExpressionClass::SUBQUERY) {
			return true;
		}
		bool found = false;
		ParsedExpressionIterator::EnumerateChildren(
		    expr, [&](const ParsedExpression &child) { found = found || ContainsColumnOrSubquery(child); });
		return found;
	}

	//! OFFSET dimensions resolve to a dimension defined as a column; the interval must be constant and shift the
	//! column's type
	void ResolveOffsets() {
		for (idx_t m = 0; m < model->members.size(); m++) {
			auto &member = model->members[m];
			auto &parsed = *member.parsed;
			if (parsed.offset_dimension.empty()) {
				continue;
			}
			auto what = What(m) + " OFFSET";
			auto name = StringUtil::Join(parsed.offset_dimension, ".");
			SemanticResolveContext resolve_context;
			resolve_context.owner_table = member.table;
			resolve_context.what = what;
			string error;
			auto ref = model->Lookup(parsed.offset_dimension, resolve_context, error);
			if (ref.Is(SemanticRefKind::NONE)) {
				Fail("%s: %s", what, error.empty() ? "unknown dimension \"" + name + "\"" : error);
			}
			if (!ref.Is(SemanticRefKind::MEMBER) || model->members[ref.index].Kind() != SemanticMemberKind::DIMENSION) {
				Fail("%s: \"%s\" is not a dimension", what, name);
			}
			auto column = SemanticMarkers::Classify(*model->members[ref.index].resolved);
			if (!column.Is(SemanticRefKind::COLUMN)) {
				Fail("%s: dimension \"%s\" must be defined as a column of its logical table", what, name);
			}
			if (ContainsColumnOrSubquery(*parsed.offset_interval)) {
				Fail("%s: the interval must be a constant expression", what);
			}
			auto &table = model->tables[column.index];
			auto offset = make_uniq<SemanticOffset>();
			offset->dimension = ref.index;
			offset->table = column.index;
			offset->column = column.column;
			offset->type = table.column_types[table.columns.at(column.column)];
			offset->interval = parsed.offset_interval->Copy();
			CheckOffsetType(*offset, what);
			member.offset = std::move(offset);
		}
	}

	//! Binds SELECT CAST(CAST(NULL AS <type>) + <interval> AS <type>)
	void CheckOffsetType(const SemanticOffset &offset, const string &what) {
		auto node = make_uniq<SelectNode>();
		node->select_list.push_back(SemanticShiftColumn(
		    make_uniq<CastExpression>(offset.type, make_uniq<ConstantExpression>(Value())), offset));
		node->from_table = make_uniq<EmptyTableRef>();
		auto statement = make_uniq<SelectStatement>();
		statement->node = std::move(node);
		try {
			auto binder = Binder::CreateBinder(context);
			binder->Bind(statement->Cast<SQLStatement>());
		} catch (std::exception &ex) {
			ErrorData error(ex);
			Fail("%s: cannot shift column \"%s.%s\" (%s) by %s: %s", what, model->tables[offset.table].name,
			     offset.column, offset.type.ToString(), offset.interval->ToString(), error.RawMessage());
		}
	}

	static void CollectMemberRefs(const ParsedExpression &expr, vector<idx_t> &out) {
		auto ref = SemanticMarkers::Classify(expr);
		if (ref.Is(SemanticRefKind::MEMBER)) {
			out.push_back(ref.index);
			return;
		}
		ParsedExpressionIterator::EnumerateChildren(
		    expr, [&](const ParsedExpression &child) { CollectMemberRefs(child, out); });
	}

	void CheckMemberCycles() {
		auto count = model->members.size();
		vector<vector<idx_t>> deps(count);
		for (idx_t m = 0; m < count; m++) {
			CollectMemberRefs(*model->members[m].resolved, deps[m]);
		}
		auto cycle = FindCycle(deps);
		if (!cycle.empty()) {
			vector<string> names;
			for (auto member : cycle) {
				names.push_back(model->MemberDisplayName(member));
			}
			Fail("Circular reference between semantic expressions: %s", StringUtil::Join(names, " -> "));
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
		auto ref = SemanticMarkers::Classify(expr);
		if (ref.Is(SemanticRefKind::MEMBER)) {
			auto member = ref.index;
			if (model->members[member].Kind() == SemanticMemberKind::METRIC) {
				vector<SemanticAtom> atoms;
				analyzer.DecomposeMetric(member, atoms, what);
				for (auto &atom : atoms) {
					if (!atom.context.offsets.empty()) {
						Fail("%s cannot reference metric \"%s\", which uses OFFSET", what,
						     model->MemberDisplayName(member));
					}
					if (atom.home == table || !model->Reaches(atom.home, table) || model->Reaches(table, atom.home)) {
						Fail("%s cannot reference metric \"%s\": its logical table must have a higher granularity "
						     "than \"%s\"",
						     what, model->MemberDisplayName(member), model->tables[table].name);
					}
				}
			}
			return;
		}
		ParsedExpressionIterator::EnumerateChildren(
		    expr, [&](const ParsedExpression &child) { CheckChildAggregations(analyzer, child, table, what); });
	}

	void CheckMemberKinds() {
		SemanticAnalyzer analyzer(*model, context);
		for (idx_t m = 0; m < model->members.size(); m++) {
			auto what = What(m);
			CheckNoWindowMetricReferences(m, what);
			if (model->members[m].IsRowLevel()) {
				CheckRowLevelMember(analyzer, m, what);
			} else {
				CheckMetric(analyzer, m, what);
			}
		}
	}

	//! Window-function metrics cannot be used by other semantic expressions
	void CheckNoWindowMetricReferences(idx_t member, const string &what) {
		vector<idx_t> references;
		CollectMemberRefs(*model->members[member].resolved, references);
		for (auto reference : references) {
			auto &target = model->members[reference];
			if (target.Kind() == SemanticMemberKind::METRIC && SemanticAnalyzer::ContainsWindow(*target.resolved)) {
				Fail("%s cannot reference window function metric \"%s\"", what, model->MemberDisplayName(reference));
			}
		}
	}

	//! Facts and dimensions read their own table and the tables it references
	void CheckRowLevelMember(SemanticAnalyzer &analyzer, idx_t m, const string &what) {
		auto &member = model->members[m];
		CheckChildAggregations(analyzer, *member.resolved, member.table, what);
		set<idx_t> tables;
		analyzer.RowTables(*member.resolved, member.table, tables, what);
		for (auto table : tables) {
			if (!model->Reaches(member.table, table)) {
				Fail("%s references logical table \"%s\", which is not reachable from \"%s\" through many-to-one "
				     "relationships",
				     what, model->tables[table].name, model->tables[member.table].name);
			}
		}
	}

	void CheckMetric(SemanticAnalyzer &analyzer, idx_t m, const string &what) {
		auto &member = model->members[m];
		if (member.filter &&
		    (analyzer.ContainsAggregateOrMetric(*member.filter) || SemanticAnalyzer::ContainsWindow(*member.filter))) {
			Fail("%s: FILTER must be a row-level condition (dimensions, facts and columns only)", what);
		}
		if (SemanticAnalyzer::ContainsWindow(*member.resolved)) {
			set<idx_t> dimensions;
			analyzer.WindowDimensions(*member.resolved, dimensions);
			for (auto dimension : dimensions) {
				if (!member.IsDerived() && !model->Reaches(member.table, model->members[dimension].table)) {
					Fail("%s: window dimension \"%s\" is not accessible from logical table \"%s\"", what,
					     model->MemberDisplayName(dimension), model->tables[member.table].name);
				}
			}
		}
		if (!analyzer.ContainsAggregateOrMetric(*member.resolved)) {
			Fail("%s must use an aggregate function", what);
		}
		for (auto &entry : member.non_additive) {
			set<idx_t> tables;
			analyzer.RowTables(*entry.resolved, DConstants::INVALID_INDEX, tables, what + " NON ADDITIVE BY");
			for (auto table : tables) {
				if (member.IsDerived() || !model->Reaches(member.table, table)) {
					Fail("%s: NON ADDITIVE BY dimensions must belong to its logical table or tables it references",
					     what);
				}
			}
		}
		vector<SemanticAtom> atoms;
		analyzer.DecomposeMetric(m, atoms, what);
		if (member.IsDerived()) {
			return;
		}
		for (auto &atom : atoms) {
			if (!model->Reaches(atom.home, member.table) && !model->Reaches(member.table, atom.home)) {
				Fail("%s aggregates rows of logical table \"%s\", which is not related to \"%s\"", what,
				     model->tables[atom.home].name, model->tables[member.table].name);
			}
		}
	}

private:
	ClientContext &context;
	shared_ptr<SemanticModel> model;
	//! The model's own copy of the definition (members point into it)
	ParsedSemanticView &definition;
};

} // namespace

shared_ptr<SemanticModel> BuildSemanticModel(ClientContext &context, const ParsedSemanticView &definition,
                                             const QualifiedName &resolved_name) {
	SemanticModelBuilder builder(context, definition, resolved_name);
	return builder.Build();
}

} // namespace duckdb
