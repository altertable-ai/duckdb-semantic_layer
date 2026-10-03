#include "semantic_join_scope.hpp"

#include "duckdb/common/exception.hpp"
#include "duckdb/common/exception/binder_exception.hpp"
#include "duckdb/parser/expression/operator_expression.hpp"
#include "duckdb/parser/tableref/joinref.hpp"
#include "semantic_ast_helpers.hpp"

namespace duckdb {

SemanticJoinScope::SemanticJoinScope(const SemanticModel &model, SemanticAliasGenerator &aliases, idx_t base,
                                     vector<idx_t> using_relationships, string prefix)
    : model(model), aliases(aliases), base(base), using_relationships(std::move(using_relationships)),
      prefix(std::move(prefix)) {
	present.insert(base);
}

SemanticJoinScope SemanticJoinScope::Nested(idx_t nested_base, idx_t context) {
	SemanticJoinScope result(model, aliases, nested_base, using_relationships, aliases.NestedPrefix());
	result.outer = this;
	result.outer_context = context;
	return result;
}

string SemanticJoinScope::AliasOf(idx_t table) const {
	return prefix + model.tables[table].name;
}

bool SemanticJoinScope::BelongsToOuter(idx_t table) const {
	return outer && model.Reaches(outer_context, table);
}

string SemanticJoinScope::Ensure(idx_t table) {
	if (BelongsToOuter(table)) {
		return outer->Ensure(table);
	}
	idx_t current = base;
	for (auto &edge : model.Path(base, table, using_relationships)) {
		auto &rel = model.relationships[edge.relationship];
		if (rel.is_range) {
			throw BinderException("Semantic view \"%s\": logical table \"%s\" is reached through the range "
			                      "relationship \"%s\" and cannot be used here",
			                      model.Name().name, model.tables[table].name, rel.name);
		}
		if (present.insert(edge.target).second) {
			joins.push_back(Joined {edge.target, edge, current});
		}
		current = edge.target;
	}
	return AliasOf(table);
}

unique_ptr<ParsedExpression> SemanticJoinScope::ColumnReference(idx_t table, const string &column) {
	if (BelongsToOuter(table)) {
		return outer->ColumnReference(table, column);
	}
	auto path = model.Path(base, table, using_relationships);
	for (idx_t i = 0; i < path.size(); i++) {
		if (model.relationships[path[i].relationship].is_range) {
			return RangeLookup(path, i, table, column);
		}
	}
	return ColumnRef(column, Ensure(table));
}

//! (SELECT <column> FROM <range table> [joins] WHERE (start IS NULL OR key >= start) AND (end IS NULL OR key < end))
unique_ptr<ParsedExpression> SemanticJoinScope::RangeLookup(const vector<SemanticEdge> &path, idx_t range_at,
                                                            idx_t table, const string &column) {
	auto source = range_at == 0 ? base : path[range_at - 1].target;
	auto &rel = model.relationships[path[range_at].relationship];
	auto source_alias = Ensure(source);
	auto range_table = path[range_at].target;
	SemanticJoinScope inner(model, aliases, range_table, using_relationships, aliases.NestedPrefix());
	auto value = inner.ColumnReference(table, column);
	auto range_alias = inner.AliasOf(range_table);
	auto bound = [&](const string &bound_column, ExpressionType comparison) {
		auto open =
		    make_uniq<OperatorExpression>(ExpressionType::OPERATOR_IS_NULL, ColumnRef(bound_column, range_alias));
		auto compare =
		    Compare(comparison, ColumnRef(rel.from_columns[0], source_alias), ColumnRef(bound_column, range_alias));
		return make_uniq<ConjunctionExpression>(ExpressionType::CONJUNCTION_OR, std::move(open), std::move(compare));
	};
	auto node = make_uniq<SelectNode>();
	node->select_list.push_back(std::move(value));
	node->from_table = inner.BuildFrom();
	vector<unique_ptr<ParsedExpression>> conditions;
	// Open bounds remove limits on known values; they do not turn a NULL referencing value into a match.
	conditions.push_back(make_uniq<OperatorExpression>(ExpressionType::OPERATOR_IS_NOT_NULL,
	                                                   ColumnRef(rel.from_columns[0], source_alias)));
	conditions.push_back(bound(rel.range_start, ExpressionType::COMPARE_GREATERTHANOREQUALTO));
	conditions.push_back(bound(rel.range_end, ExpressionType::COMPARE_LESSTHAN));
	node->where_clause = AndAll(std::move(conditions));
	return MakeScalarSubquery(std::move(node));
}

unique_ptr<TableRef> SemanticJoinScope::BuildFrom() const {
	auto result = model.tables[base].source->Copy();
	result->alias = AliasOf(base);
	for (auto &join : joins) {
		auto &rel = model.relationships[join.edge.relationship];
		auto &parent_columns = join.edge.reversed ? rel.to_columns : rel.from_columns;
		auto &child_columns = join.edge.reversed ? rel.from_columns : rel.to_columns;
		vector<unique_ptr<ParsedExpression>> conditions;
		for (idx_t c = 0; c < parent_columns.size(); c++) {
			// ASOF: the latest row of the referenced table at or before the referencing value
			auto comparison =
			    c == rel.asof_index ? ExpressionType::COMPARE_GREATERTHANOREQUALTO : ExpressionType::COMPARE_EQUAL;
			conditions.push_back(Compare(comparison, ColumnRef(parent_columns[c], AliasOf(join.parent_table)),
			                             ColumnRef(child_columns[c], AliasOf(join.table))));
		}
		auto join_ref =
		    make_uniq<JoinRef>(rel.asof_index != DConstants::INVALID_INDEX ? JoinRefType::ASOF : JoinRefType::REGULAR);
		join_ref->type = JoinType::LEFT;
		join_ref->left = std::move(result);
		join_ref->right = model.tables[join.table].source->Copy();
		join_ref->right->alias = AliasOf(join.table);
		join_ref->condition = AndAll(std::move(conditions));
		result = std::move(join_ref);
	}
	return result;
}

} // namespace duckdb
