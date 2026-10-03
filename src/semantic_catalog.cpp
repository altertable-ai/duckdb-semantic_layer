#include "semantic_catalog.hpp"

#include "duckdb/catalog/catalog.hpp"
#include "duckdb/catalog/entry_lookup_info.hpp"
#include "duckdb/common/case_insensitive_map.hpp"
#include "duckdb/common/exception/binder_exception.hpp"
#include "duckdb/common/exception/catalog_exception.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/main/attached_database.hpp"
#include "duckdb/main/extension/extension_loader.hpp"
#include "duckdb/parser/expression/cast_expression.hpp"
#include "duckdb/parser/expression/constant_expression.hpp"
#include "duckdb/parser/expression/function_expression.hpp"
#include "duckdb/parser/expression/star_expression.hpp"
#include "duckdb/parser/parsed_data/create_view_info.hpp"
#include "duckdb/parser/parsed_data/drop_info.hpp"
#include "duckdb/parser/result_modifier.hpp"
#include "duckdb/parser/tableref/emptytableref.hpp"
#include "duckdb/parser/tableref/table_function_ref.hpp"
#include "duckdb/planner/binder.hpp"
#include "semantic_ast_helpers.hpp"
#include "semantic_view_registry.hpp"
#include "semantic_views_functions.hpp"

namespace duckdb {

vector<SemanticCatalogColumn> SemanticCatalogColumns(const SemanticModel &model) {
	vector<idx_t> members;
	case_insensitive_map_t<idx_t> name_count;
	for (auto kind : {SemanticMemberKind::FACT, SemanticMemberKind::DIMENSION, SemanticMemberKind::METRIC}) {
		for (idx_t m = 0; m < model.members.size(); m++) {
			auto &member = model.members[m];
			if (member.Kind() == kind && !member.IsPrivate()) {
				members.push_back(m);
				name_count[member.Name()]++;
			}
		}
	}
	vector<SemanticCatalogColumn> result;
	for (auto m : members) {
		auto &member = model.members[m];
		SemanticCatalogColumn column;
		column.name = name_count[member.Name()] > 1 ? model.MemberDisplayName(m) : member.Name();
		column.type = member.type;
		column.comment = member.parsed->comment.empty() ? Value() : Value(member.parsed->comment);
		result.push_back(std::move(column));
	}
	return result;
}

//! The catalog holding the placeholder of `name`, or nullptr when it gets none: a database or schema that does not
//! exist, read-only databases, and catalogs that are not DuckDB's own (an attached Postgres or SQLite database would
//! get a real view)
static optional_ptr<Catalog> PlaceholderCatalog(ClientContext &context, const QualifiedName &name) {
	auto catalog = Catalog::GetCatalogEntry(context, name.catalog);
	if (!catalog || catalog->IsSystemCatalog() || !catalog->IsDuckCatalog() || catalog->GetAttached().IsReadOnly() ||
	    !catalog->GetSchema(context, name.schema, OnEntryNotFound::RETURN_NULL)) {
		return nullptr;
	}
	return catalog;
}

static optional_ptr<CatalogEntry> ExistingEntry(ClientContext &context, const QualifiedName &name) {
	EntryLookupInfo lookup(CatalogType::TABLE_ENTRY, name.name);
	return Catalog::GetEntry(context, name.catalog, name.schema, lookup, OnEntryNotFound::RETURN_NULL);
}

static bool SameView(const QualifiedName &a, const QualifiedName &b) {
	return StringUtil::CIEquals(a.catalog, b.catalog) && StringUtil::CIEquals(a.schema, b.schema) &&
	       StringUtil::CIEquals(a.name, b.name);
}

void BindSemanticViewChange(ClientContext &context, optional_ptr<Binder> binder, const QualifiedName &before,
                            const QualifiedName &after) {
	for (auto *name : {&before, &after}) {
		auto catalog = name->name.empty() ? nullptr : PlaceholderCatalog(context, *name);
		if (!catalog) {
			continue;
		}
		if (name == &before) {
			// dropped or renamed: the catalog only changes when the placeholder exists
			auto entry = ExistingEntry(context, before);
			if (!entry || !IsSemanticPlaceholder(*entry)) {
				continue;
			}
		} else if (!SameView(before, after)) {
			auto entry = ExistingEntry(context, after);
			if (entry && !IsSemanticPlaceholder(*entry)) {
				throw CatalogException(
				    "Cannot create semantic view \"%s\": a table or view with that name already exists", after.name);
			}
		}
		if (binder) {
			binder->GetStatementProperties().RegisterDBModify(*catalog, context,
			                                                  DatabaseModificationType::CREATE_CATALOG_ENTRY |
			                                                      DatabaseModificationType::DROP_CATALOG_ENTRY);
		}
	}
}

//! SELECT * FROM __semantic_view_placeholder('<catalog>', '<schema>', '<name>')
static unique_ptr<SelectStatement> PlaceholderQuery(const QualifiedName &name) {
	vector<unique_ptr<ParsedExpression>> arguments;
	for (auto &part : {name.catalog, name.schema, name.name}) {
		arguments.push_back(make_uniq<ConstantExpression>(Value(part)));
	}
	auto function = make_uniq<TableFunctionRef>();
	function->function = make_uniq<FunctionExpression>(SEMANTIC_VIEW_PLACEHOLDER_FUNCTION, std::move(arguments));
	auto node = make_uniq<SelectNode>();
	node->select_list.push_back(make_uniq<StarExpression>());
	node->from_table = std::move(function);
	return MakeSelectStatement(std::move(node));
}

static void CreatePlaceholder(ClientContext &context, const SemanticModel &model) {
	auto &name = model.Name();
	auto catalog = PlaceholderCatalog(context, name);
	if (!catalog) {
		return;
	}
	CreateViewInfo info(name.catalog, name.schema, name.name);
	info.on_conflict = OnCreateConflict::REPLACE_ON_CONFLICT;
	info.temporary = catalog->IsTemporaryCatalog();
	info.comment = model.Comment().empty() ? Value() : Value(model.Comment());
	for (auto &column : SemanticCatalogColumns(model)) {
		info.names.push_back(column.name);
		info.types.push_back(column.type);
		if (!column.comment.IsNull()) {
			info.column_comments_map[column.name] = column.comment;
		}
	}
	info.tags[SEMANTIC_VIEW_PLACEHOLDER_TAG] = "true";
	info.query = PlaceholderQuery(name);
	catalog->CreateView(context, info);
}

static bool DropPlaceholder(ClientContext &context, const QualifiedName &name) {
	auto catalog = PlaceholderCatalog(context, name);
	if (!catalog) {
		return false;
	}
	auto entry = ExistingEntry(context, name);
	if (!entry || !IsSemanticPlaceholder(*entry)) {
		return false;
	}
	DropInfo info;
	info.type = CatalogType::VIEW_ENTRY;
	info.catalog = name.catalog;
	info.schema = name.schema;
	info.name = name.name;
	info.if_not_found = OnEntryNotFound::RETURN_NULL;
	catalog->DropEntry(context, info);
	return true;
}

bool ApplySemanticViewChange(ClientContext &context, const QualifiedName &before,
                             optional_ptr<const SemanticModel> after) {
	bool dropped = false;
	if (!before.name.empty() && !(after && SameView(before, after->Name()))) {
		dropped = DropPlaceholder(context, before);
	}
	if (after) {
		// replaces the placeholder of the same name, if any
		CreatePlaceholder(context, *after);
	}
	return dropped;
}

bool IsSemanticPlaceholder(const CatalogEntry &entry) {
	return entry.type == CatalogType::VIEW_ENTRY && entry.tags.find(SEMANTIC_VIEW_PLACEHOLDER_TAG) != entry.tags.end();
}

unique_ptr<SelectNode> SemanticDescribeNode(const SemanticModel &model) {
	auto node = make_uniq<SelectNode>();
	for (auto &column : SemanticCatalogColumns(model)) {
		auto value = make_uniq<CastExpression>(column.type, make_uniq<ConstantExpression>(Value()));
		value->alias = column.name;
		node->select_list.push_back(std::move(value));
	}
	auto limit = make_uniq<LimitModifier>();
	limit->limit = make_uniq<ConstantExpression>(Value::BIGINT(0));
	node->modifiers.push_back(std::move(limit));
	node->from_table = make_uniq<EmptyTableRef>();
	return node;
}

//===--------------------------------------------------------------------===//
// __semantic_view_placeholder: reached only when the standard-SQL rewrite does not apply
//===--------------------------------------------------------------------===//
static unique_ptr<FunctionData> PlaceholderBind(ClientContext &, TableFunctionBindInput &input, vector<LogicalType> &,
                                                vector<string> &) {
	auto name = NameArguments(input, 0);
	if (GetSemanticViewsState(input).registry.Get(name)) {
		throw BinderException("Semantic view \"%s\" cannot be read as a table here: query it with SEMANTIC_VIEW(...), "
		                      "or by name with dimensions in GROUP BY and metrics in AGG(...)",
		                      name.name);
	}
	throw BinderException(
	    "Semantic view \"%s\" is not defined in this session: semantic views are kept in memory only; "
	    "run CREATE SEMANTIC VIEW again",
	    name.name);
}

void RegisterSemanticPlaceholderFunction(ExtensionLoader &loader, shared_ptr<SemanticViewsState> state) {
	loader.RegisterFunction(SemanticViewsTableFunction(SEMANTIC_VIEW_PLACEHOLDER_FUNCTION,
	                                                   vector<LogicalType>(3, LogicalType::VARCHAR), RowsFunctionScan,
	                                                   PlaceholderBind, RowsFunctionInit, std::move(state)));
}

} // namespace duckdb
