#include "semantic_sql_rewrite.hpp"

#include "duckdb/catalog/catalog.hpp"
#include "duckdb/catalog/entry_lookup_info.hpp"
#include "duckdb/common/exception/binder_exception.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/parser/expression/constant_expression.hpp"
#include "duckdb/parser/query_node/select_node.hpp"
#include "duckdb/parser/tableref/basetableref.hpp"
#include "duckdb/parser/tableref/table_function_ref.hpp"
#include "semantic_ast_helpers.hpp"
#include "semantic_catalog.hpp"
#include "semantic_lowering.hpp"
#include "semantic_statement_visitor.hpp"
#include "semantic_layer_registry.hpp"
#include "semantic_standard_form.hpp"

namespace duckdb {

namespace {

//! Rewrites the query nodes that read from a semantic view by name
class SemanticReferenceRewriter : public SemanticStatementVisitor {
public:
	SemanticReferenceRewriter(ClientContext &context, SemanticViewsState &state) : context(context), state(state) {
	}

	bool rewritten = false;

protected:
	void VisitTableRef(unique_ptr<TableRef> &ref, bool in_join) override {
		VisitSemanticQuery(*ref);
		if (in_join && ref->type == TableReferenceType::BASE_TABLE && SemanticViewFor(ref->Cast<BaseTableRef>())) {
			throw BinderException("Semantic view \"%s\" cannot be joined in the FROM "
			                      "clause; use SEMANTIC_VIEW(...) "
			                      "or a subquery instead",
			                      ref->Cast<BaseTableRef>().Table().GetIdentifierName());
		}
	}

	//! DESCRIBE <semantic view>: the columns of its catalog placeholder
	bool VisitShowQuery(ShowRef &show) override {
		if (show.show_type != ShowType::DESCRIBE || show.query->type != QueryNodeType::SELECT_NODE) {
			return false;
		}
		auto &select = show.query->Cast<SelectNode>();
		if (select.select_list.size() != 1 || select.select_list[0]->GetExpressionClass() != ExpressionClass::STAR ||
		    !select.from_table || select.from_table->type != TableReferenceType::BASE_TABLE) {
			return false;
		}
		auto model = SemanticViewFor(select.from_table->Cast<BaseTableRef>());
		if (!model) {
			return false;
		}
		show.query = SemanticDescribeNode(*model);
		rewritten = true;
		return true;
	}

	//! Before its subqueries are rewritten, names an unaliased select item reading a subquery as written, as DuckDB
	//! would
	void EnterSelect(SelectNode &select) override {
		for (auto &expr : select.select_list) {
			if (expr->GetAlias().empty() && expr->GetExpressionClass() != ExpressionClass::COLUMN_REF &&
			    expr->HasSubquery()) {
				expr->SetAlias(Identifier(expr->ToString()));
			}
		}
	}

	void VisitSelect(SelectNode &select) override {
		if (!select.from_table || select.from_table->type != TableReferenceType::BASE_TABLE) {
			return;
		}
		auto &base = select.from_table->Cast<BaseTableRef>();
		auto model = SemanticViewFor(base);
		if (model) {
			RewriteStandardForm(context, *model, base, select);
			rewritten = true;
		}
	}

private:
	//! SEMANTIC_VIEW(...), lowered by the parser: rewrites the queries by name in its expressions (subqueries of its
	//! WHERE clause)
	void VisitSemanticQuery(TableRef &ref) {
		auto argument = SemanticLowering::QueryArgument(ref);
		if (!argument) {
			return;
		}
		auto query = SemanticLowering::Decode<ParsedSemanticQuery>(ConstantValue(**argument));
		auto was_rewritten = rewritten;
		rewritten = false;
		if (query->where) {
			VisitExpression(query->where);
		}
		for (auto &item : query->items) {
			if (item.expression) {
				VisitExpression(item.expression);
			}
		}
		if (rewritten) {
			*argument = ConstantExpression::FromValue(SemanticLowering::Encode(*query));
		}
		rewritten = rewritten || was_rewritten;
	}

	shared_ptr<SemanticModel> SemanticViewFor(const BaseTableRef &ref) {
		auto &qualified_name = ref.GetQualifiedName();
		if (qualified_name.Path().size() > 3) {
			// a table in a nested schema: semantic views live in catalog.schema
			return nullptr;
		}
		bool unqualified = qualified_name.Path().size() == 1;
		if (unqualified && IsCTEName(ref.Table().GetIdentifierName())) {
			return nullptr;
		}
		auto name = SemanticQualifiedName::FromQualifiedName(qualified_name);
		auto model = state.registry.Lookup(context, name);
		if (!model) {
			return nullptr;
		}
		// a real table or view of that name (another catalog's, or a CTE) takes precedence over the semantic view; its
		// own catalog entry is a placeholder
		auto entry = Catalog::GetEntry(context, EntryLookupInfo(CatalogType::TABLE_ENTRY, qualified_name),
		                               OnEntryNotFound::RETURN_NULL);
		if (entry && !IsSemanticPlaceholder(*entry)) {
			return nullptr;
		}
		return model;
	}

	ClientContext &context;
	SemanticViewsState &state;
};

} // namespace

bool RewriteSemanticViewReferences(ClientContext &context, SemanticViewsState &state, SQLStatement &statement) {
	SemanticReferenceRewriter rewriter(context, state);
	rewriter.VisitStatement(statement);
	return rewriter.rewritten;
}

} // namespace duckdb
