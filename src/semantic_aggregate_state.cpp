#include "semantic_aggregate_state.hpp"

#include "duckdb/common/string_util.hpp"
#include "duckdb/parser/expression/cast_expression.hpp"
#include "duckdb/parser/expression/constant_expression.hpp"
#include "duckdb/parser/expression/operator_expression.hpp"
#include "semantic_ast_helpers.hpp"

namespace duckdb {

//===--------------------------------------------------------------------===//
// Aggregate state
//===--------------------------------------------------------------------===//
string SemanticAggregateKindToString(SemanticAggregateKind kind) {
	switch (kind) {
	case SemanticAggregateKind::SUM:
		return "SUM";
	case SemanticAggregateKind::COUNT:
		return "COUNT";
	case SemanticAggregateKind::AVG:
		return "AVG";
	case SemanticAggregateKind::MIN:
		return "MIN";
	case SemanticAggregateKind::MAX:
		return "MAX";
	default:
		return "OPAQUE";
	}
}

SemanticAggregateKind ClassifySemanticAggregate(const ParsedExpression &expr, const LogicalType &type) {
	auto &function = expr.Cast<FunctionExpression>();
	if (function.Distinct() || function.ExportState() || (function.OrderBy() && !function.OrderBy()->orders.empty())) {
		return SemanticAggregateKind::OPAQUE;
	}
	auto name = StringUtil::Lower(function.FunctionName().GetIdentifierName());
	if (name == "count_star" && ArgumentCount(function) == 0) {
		return SemanticAggregateKind::COUNT;
	}
	if (ArgumentCount(function) != 1) {
		return SemanticAggregateKind::OPAQUE;
	}
	if (name == "sum") {
		return SemanticAggregateKind::SUM;
	}
	if (name == "count") {
		return SemanticAggregateKind::COUNT;
	}
	if ((name == "avg" || name == "mean") && type.id() == LogicalTypeId::DOUBLE) {
		// sum / count reproduces the average of numbers (DOUBLE); averages of other types are stored as is
		return SemanticAggregateKind::AVG;
	}
	if (name == "min") {
		return SemanticAggregateKind::MIN;
	}
	if (name == "max") {
		return SemanticAggregateKind::MAX;
	}
	return SemanticAggregateKind::OPAQUE;
}

vector<string> SemanticMaterializedAtom::StateColumns() const {
	switch (kind) {
	case SemanticAggregateKind::SUM:
	case SemanticAggregateKind::AVG:
		return {prefix + "_sum", prefix + "_cnt"};
	case SemanticAggregateKind::COUNT:
		return {prefix + "_cnt"};
	case SemanticAggregateKind::MIN:
		return {prefix + "_min"};
	case SemanticAggregateKind::MAX:
		return {prefix + "_max"};
	default:
		return {prefix + "_val"};
	}
}

static unique_ptr<ParsedExpression> Call(const string &name, vector<unique_ptr<ParsedExpression>> children) {
	return make_uniq<FunctionExpression>(Identifier(name), std::move(children));
}

static unique_ptr<ParsedExpression> Call(const string &name, unique_ptr<ParsedExpression> child) {
	vector<unique_ptr<ParsedExpression>> children;
	children.push_back(std::move(child));
	return Call(name, std::move(children));
}

vector<unique_ptr<ParsedExpression>> SemanticStateExpressions(const SemanticMaterializedAtom &atom,
                                                              const FunctionExpression &compiled) {
	// the same aggregate (and FILTER) over the same argument, decomposed into its state
	auto over_argument = [&](const string &name) {
		auto result = Call(name, Argument(compiled, 0).Copy());
		result->Cast<FunctionExpression>().FilterMutable() = compiled.Filter() ? compiled.Filter()->Copy() : nullptr;
		return result;
	};
	vector<unique_ptr<ParsedExpression>> result;
	switch (atom.kind) {
	case SemanticAggregateKind::SUM:
	case SemanticAggregateKind::AVG:
		result.push_back(over_argument("sum"));
		result.push_back(over_argument("count"));
		break;
	default:
		result.push_back(compiled.Copy());
		break;
	}
	auto columns = atom.StateColumns();
	for (idx_t c = 0; c < result.size(); c++) {
		result[c]->SetAlias(Identifier(columns[c]));
	}
	return result;
}

unique_ptr<ParsedExpression> SemanticRollupExpression(const SemanticMaterializedAtom &atom, const string &alias) {
	auto column = [&](const string &suffix) {
		return ColumnRef(atom.prefix + suffix, alias);
	};
	unique_ptr<ParsedExpression> result;
	switch (atom.kind) {
	case SemanticAggregateKind::SUM:
		result = Call("sum", column("_sum"));
		break;
	case SemanticAggregateKind::COUNT: {
		// COUNT over no rows is 0, SUM over no rows is NULL
		auto coalesce = make_uniq<OperatorExpression>(ExpressionType::OPERATOR_COALESCE);
		coalesce->GetChildrenMutable().push_back(Call("sum", column("_cnt")));
		coalesce->GetChildrenMutable().push_back(ConstantExpression::FromValue(Value::BIGINT(0)));
		result = std::move(coalesce);
		break;
	}
	case SemanticAggregateKind::AVG: {
		vector<unique_ptr<ParsedExpression>> count;
		count.push_back(Call("sum", column("_cnt")));
		count.push_back(ConstantExpression::FromValue(Value::BIGINT(0)));
		vector<unique_ptr<ParsedExpression>> division;
		division.push_back(make_uniq<CastExpression>(LogicalType::DOUBLE, Call("sum", column("_sum"))));
		division.push_back(Call("nullif", std::move(count)));
		result = make_uniq<FunctionExpression>("/", std::move(division), nullptr, nullptr, false, true);
		break;
	}
	case SemanticAggregateKind::MIN:
		result = Call("min", column("_min"));
		break;
	case SemanticAggregateKind::MAX:
		result = Call("max", column("_max"));
		break;
	default:
		result = Call("any_value", column("_val"));
		break;
	}
	return make_uniq<CastExpression>(atom.type, std::move(result));
}

SemanticAggregateMaintenance PlanSemanticAggregateMaintenance(const SemanticMaterializedAtom &atom) {
	SemanticAggregateMaintenance plan;
	plan.columns = atom.StateColumns();
	auto &columns = plan.columns;
	auto plus = [](const string &column) {
		return "coalesce(__sv_m." + column + ", 0) + coalesce(__sv_d." + column + ", 0)";
	};
	auto additive_delta = [&](const string &column) {
		plan.delta.push_back("sum(CASE WHEN __sv_sign > 0 THEN coalesce(" + column + ", 0) ELSE -coalesce(" + column +
		                     ", 0) END) AS " + column);
	};
	switch (atom.kind) {
	case SemanticAggregateKind::SUM:
	case SemanticAggregateKind::AVG:
		for (auto &column : columns) {
			additive_delta(column);
		}
		plan.updates.push_back(columns[0] + " = CASE WHEN " + plus(columns[1]) + " = 0 THEN NULL ELSE " +
		                       plus(columns[0]) + " END");
		plan.updates.push_back(columns[1] + " = " + plus(columns[1]));
		plan.inserts.push_back("CASE WHEN coalesce(__sv_d." + columns[1] + ", 0) = 0 THEN NULL ELSE __sv_d." +
		                       columns[0] + " END");
		plan.inserts.push_back("coalesce(__sv_d." + columns[1] + ", 0)");
		break;
	case SemanticAggregateKind::COUNT:
		additive_delta(columns[0]);
		plan.updates.push_back(columns[0] + " = " + plus(columns[0]));
		plan.inserts.push_back("coalesce(__sv_d." + columns[0] + ", 0)");
		break;
	case SemanticAggregateKind::MIN:
	case SemanticAggregateKind::MAX: {
		auto function = atom.kind == SemanticAggregateKind::MIN ? "min" : "max";
		auto merge = atom.kind == SemanticAggregateKind::MIN ? "least" : "greatest";
		plan.delta.push_back(string(function) + "(CASE WHEN __sv_sign > 0 THEN " + columns[0] + " END) AS " +
		                     columns[0]);
		plan.updates.push_back(columns[0] + " = " + merge + "(__sv_m." + columns[0] + ", __sv_d." + columns[0] + ")");
		plan.inserts.push_back("__sv_d." + columns[0]);
		plan.recompute = SemanticRecompute::DELETIONS;
		break;
	}
	case SemanticAggregateKind::OPAQUE:
		plan.inserts.push_back("CAST(NULL AS " + atom.type.ToString() + ")");
		plan.recompute = SemanticRecompute::ALWAYS;
		break;
	}
	return plan;
}

} // namespace duckdb
