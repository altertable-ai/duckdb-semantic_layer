#include "semantic_transforms.hpp"

#include "duckdb/common/error_data.hpp"
#include "duckdb/common/exception.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/parser/expression/cast_expression.hpp"
#include "duckdb/parser/expression/constant_expression.hpp"
#include "duckdb/parser/expression/function_expression.hpp"
#include "duckdb/parser/parsed_expression_iterator.hpp"
#include "duckdb/parser/query_node/select_node.hpp"
#include "duckdb/parser/statement/select_statement.hpp"
#include "duckdb/parser/tableref/emptytableref.hpp"
#include "duckdb/planner/binder.hpp"
#include "semantic_analysis.hpp"
#include "semantic_definition_exception.hpp"
#include "semantic_offsets.hpp"
#include "semantic_time_spine.hpp"

namespace duckdb {

const SemanticOffset &SemanticTransforms::Offset(idx_t index) const {
	if (index < model.offsets.size()) {
		return model.offsets[index];
	}
	D_ASSERT(index - model.offsets.size() < offsets.size());
	return *offsets[index - model.offsets.size()];
}

const SemanticCumulative &SemanticTransforms::Cumulative(idx_t index) const {
	if (index < model.cumulatives.size()) {
		return model.cumulatives[index];
	}
	D_ASSERT(index - model.cumulatives.size() < cumulatives.size());
	return *cumulatives[index - model.cumulatives.size()];
}

idx_t SemanticTransforms::AddOffset(SemanticOffset offset) {
	for (idx_t i = 0; i < model.offsets.size(); i++) {
		if (model.offsets[i].Equals(offset)) {
			return i;
		}
	}
	for (idx_t i = 0; i < offsets.size(); i++) {
		if (offsets[i]->Equals(offset)) {
			return model.offsets.size() + i;
		}
	}
	offsets.push_back(make_uniq<SemanticOffset>(std::move(offset)));
	return model.offsets.size() + offsets.size() - 1;
}

idx_t SemanticTransforms::AddCumulative(SemanticCumulative cumulative) {
	for (idx_t i = 0; i < model.cumulatives.size(); i++) {
		if (model.cumulatives[i].Equals(cumulative)) {
			return i;
		}
	}
	for (idx_t i = 0; i < cumulatives.size(); i++) {
		if (cumulatives[i]->Equals(cumulative)) {
			return model.cumulatives.size() + i;
		}
	}
	cumulatives.push_back(make_uniq<SemanticCumulative>(std::move(cumulative)));
	return model.cumulatives.size() + cumulatives.size() - 1;
}

//===--------------------------------------------------------------------===//
// Validation shared with the definition clauses
//===--------------------------------------------------------------------===//
bool SemanticIsConstant(SemanticAnalyzer &analyzer, const ParsedExpression &expr) {
	return expr.IsScalar() && !expr.HasParameter() && !analyzer.ContainsAggregateOrMetric(expr) &&
	       !SemanticAnalyzer::ContainsWindow(expr);
}

string SemanticBindError(ClientContext &context, unique_ptr<ParsedExpression> expr) {
	auto node = make_uniq<SelectNode>();
	node->select_list.push_back(std::move(expr));
	node->from_table = make_uniq<EmptyTableRef>();
	auto statement = make_uniq<SelectStatement>();
	statement->node = std::move(node);
	try {
		auto binder = Binder::CreateBinder(context);
		binder->Bind(statement->Cast<SQLStatement>());
	} catch (std::exception &ex) {
		ErrorData error(ex);
		return error.RawMessage();
	}
	return string();
}

SemanticOffset SemanticMakeOffset(ClientContext &context, SemanticAnalyzer &analyzer, const SemanticModel &model,
                                  idx_t dimension, const ParsedExpression &interval, const string &what) {
	auto column = SemanticMarkers::Classify(*model.members[dimension].resolved);
	if (!column.Is(SemanticRefKind::COLUMN)) {
		throw InternalException("OFFSET dimension \"%s\" is not defined as a column",
		                        model.MemberDisplayName(dimension));
	}
	if (!SemanticIsConstant(analyzer, interval)) {
		throw SemanticDefinitionException("%s: the interval must be a constant expression", what);
	}
	auto &table = model.tables[column.index];
	SemanticOffset offset;
	offset.dimension = dimension;
	offset.table = column.index;
	offset.column = column.column;
	offset.type = table.column_types[table.columns.at(column.column)];
	offset.interval = interval.Copy();
	// binds SELECT CAST(CAST(NULL AS <type>) + <interval> AS <type>)
	auto error = SemanticBindError(
	    context,
	    SemanticShiftColumn(make_uniq<CastExpression>(offset.type, ConstantExpression::FromValue(Value())), offset));
	if (!error.empty()) {
		throw SemanticDefinitionException("%s: cannot shift column \"%s.%s\" (%s) by %s: %s", what, table.name,
		                                  offset.column, offset.type.ToString(), offset.interval->ToString(), error);
	}
	return offset;
}

//===--------------------------------------------------------------------===//
// Query-time transforms
//===--------------------------------------------------------------------===//
namespace {

constexpr const char *TIME_SHIFT_FUNCTION = "time_shift";
constexpr const char *CUMULATIVE_FUNCTION = "cumulative";

bool IsTransformCall(const ParsedExpression &expr) {
	if (expr.GetExpressionClass() != ExpressionClass::FUNCTION) {
		return false;
	}
	auto &call = expr.Cast<FunctionExpression>();
	if (call.IsOperator() || call.GetQualifiedName().Path().size() != 1) {
		return false;
	}
	auto &name = call.FunctionName().GetIdentifierName();
	return StringUtil::CIEquals(name, TIME_SHIFT_FUNCTION) || StringUtil::CIEquals(name, CUMULATIVE_FUNCTION);
}

bool IsTimeShift(const FunctionExpression &call) {
	return StringUtil::CIEquals(call.FunctionName().GetIdentifierName(), TIME_SHIFT_FUNCTION);
}

string Usage(const FunctionExpression &call) {
	if (IsTimeShift(call)) {
		return "time_shift(<metric>, <interval> [, dimension := <dimension>])";
	}
	return "cumulative(<metric> [, trailing_window := <interval> | to_date := '<date part>'] [, period := 'first' | "
	       "'last' | 'average'] [, dimension := <dimension>])";
}

//! "<item what>: <name>()", the call as written
string CallWhat(const FunctionExpression &call, const string &what) {
	return what + ": " + call.FunctionName().GetIdentifierName() + "()";
}

//! A window function metric `expr` references (following metric references), or invalid
optional_idx WindowMetric(const SemanticModel &model, const ParsedExpression &expr) {
	auto ref = SemanticMarkers::Classify(expr);
	if (ref.Is(SemanticRefKind::MEMBER)) {
		auto &member = model.members[ref.index];
		if (member.Kind() != SemanticMemberKind::METRIC) {
			return optional_idx();
		}
		if (SemanticAnalyzer::ContainsWindow(*member.resolved)) {
			return ref.index;
		}
		return WindowMetric(model, *member.resolved);
	}
	optional_idx result;
	ParsedExpressionIterator::EnumerateChildren(expr, [&](const ParsedExpression &child) {
		if (!result.IsValid()) {
			result = WindowMetric(model, child);
		}
	});
	return result;
}

//! The name of a resolved dimension argument, for messages
string ArgumentName(const SemanticModel &model, const ParsedExpression &argument) {
	auto ref = SemanticMarkers::Classify(argument);
	if (ref.Is(SemanticRefKind::MEMBER)) {
		return model.MemberDisplayName(ref.index);
	}
	if (ref.Is(SemanticRefKind::COLUMN)) {
		return model.tables[ref.index].name + "." + ref.column;
	}
	return argument.ToString();
}

class TransformResolver {
public:
	TransformResolver(ClientContext &context, SemanticAnalyzer &analyzer, const SemanticModel &model,
	                  const string &what)
	    : context(context), analyzer(analyzer), model(model), what(what) {
	}

	//! Innermost calls first, so that the argument of a call holds markers only
	void Rewrite(unique_ptr<ParsedExpression> &node, bool in_aggregate) {
		if (IsTransformCall(*node)) {
			auto &call = node->Cast<FunctionExpression>();
			if (in_aggregate) {
				throw SemanticDefinitionException("%s: %s() cannot be used inside an aggregate", what,
				                                  call.FunctionName().GetIdentifierName());
			}
			for (auto &argument : call.GetArgumentsMutable()) {
				Rewrite(argument.GetExpressionMutable(), false);
			}
			if (IsTimeShift(call)) {
				auto alias = node->GetAlias();
				node = TimeShift(call);
				node->SetAlias(alias);
			}
			return;
		}
		auto aggregate = in_aggregate || analyzer.IsAggregate(*node);
		ParsedExpressionIterator::EnumerateChildren(
		    *node, [&](unique_ptr<ParsedExpression> &child) { Rewrite(child, aggregate); });
	}

private:
	//! time_shift(<metric>, <interval> [, dimension := <dimension>])
	unique_ptr<ParsedExpression> TimeShift(const FunctionExpression &call) {
		auto call_what = CallWhat(call, what);
		auto arguments = SemanticSplitTransformArguments(call, 2, {"dimension"}, call_what);
		auto &metric = *arguments.positional[0];
		CheckNoWindow(metric, call_what);
		vector<SemanticAtom> atoms;
		analyzer.Decompose(metric, SemanticMetricContext(), atoms, what);
		auto entry = arguments.named.find("dimension");
		optional_ptr<const ParsedExpression> dimension_argument;
		if (entry != arguments.named.end()) {
			dimension_argument = entry->second;
		}
		auto dimension = SemanticTransformDimension(model, dimension_argument, atoms, call_what);
		auto offset = SemanticMakeOffset(context, analyzer, model, dimension, *arguments.positional[1], call_what);
		auto index = analyzer.Transforms().AddOffset(std::move(offset));
		return SemanticMarkers::Transform(SemanticTransformKind::OFFSET, index, metric.Copy());
	}

	void CheckNoWindow(const ParsedExpression &metric, const string &call_what) {
		if (SemanticAnalyzer::ContainsWindow(metric)) {
			throw SemanticDefinitionException("%s: the metric expression cannot contain a window function", call_what);
		}
		auto window_metric = WindowMetric(model, metric);
		if (window_metric.IsValid()) {
			throw SemanticDefinitionException(
			    "%s: the metric expression cannot contain a window function (metric \"%s\" is a window function "
			    "metric)",
			    call_what, model.MemberDisplayName(window_metric.GetIndex()));
		}
	}

	ClientContext &context;
	SemanticAnalyzer &analyzer;
	const SemanticModel &model;
	const string &what;
};

} // namespace

optional_ptr<const FunctionExpression> SemanticFindTransformCall(const ParsedExpression &expr) {
	if (IsTransformCall(expr)) {
		return &expr.Cast<FunctionExpression>();
	}
	optional_ptr<const FunctionExpression> result;
	ParsedExpressionIterator::EnumerateChildren(expr, [&](const ParsedExpression &child) {
		if (!result) {
			result = SemanticFindTransformCall(child);
		}
	});
	return result;
}

SemanticTransformArguments SemanticSplitTransformArguments(const FunctionExpression &call, idx_t positional,
                                                           const vector<string> &names, const string &what) {
	SemanticTransformArguments result;
	for (auto &argument : call.GetArguments()) {
		auto &expr = argument.GetExpression();
		auto &name = argument.HasName() ? argument.GetName().GetIdentifierName() : expr.GetAlias().GetIdentifierName();
		if (name.empty()) {
			result.positional.push_back(&expr);
			continue;
		}
		auto known = std::find_if(names.begin(), names.end(),
		                          [&](const string &candidate) { return StringUtil::CIEquals(candidate, name); });
		if (known == names.end()) {
			throw SemanticDefinitionException("%s: unknown argument \"%s\" (named arguments: %s; usage: %s)", what,
			                                  name, StringUtil::Join(names, ", "), Usage(call));
		}
		if (!result.named.emplace(name, &expr).second) {
			throw SemanticDefinitionException("%s: repeated argument \"%s\" (usage: %s)", what, name, Usage(call));
		}
	}
	if (result.positional.size() != positional) {
		throw SemanticDefinitionException("%s takes %d positional argument%s, got %d (usage: %s)", what, positional,
		                                  positional == 1 ? "" : "s", result.positional.size(), Usage(call));
	}
	return result;
}

idx_t SemanticTransformDimension(const SemanticModel &model, optional_ptr<const ParsedExpression> argument,
                                 const vector<SemanticAtom> &atoms, const string &what) {
	auto reaches = [&](idx_t table) {
		return std::all_of(atoms.begin(), atoms.end(),
		                   [&](const SemanticAtom &atom) { return model.Reaches(atom.home, table); });
	};
	auto fail = [&](const string &message) {
		vector<string> valid;
		for (idx_t m = 0; m < model.members.size(); m++) {
			auto &member = model.members[m];
			if (member.Kind() != SemanticMemberKind::DIMENSION || member.IsPrivate()) {
				continue;
			}
			auto column = SemanticMarkers::Classify(*member.resolved);
			if (column.Is(SemanticRefKind::COLUMN) && reaches(column.index)) {
				valid.push_back(model.MemberDisplayName(m));
			}
		}
		throw SemanticDefinitionException("%s; valid dimensions: %s", message,
		                                  valid.empty() ? "(none)" : StringUtil::Join(valid, ", "));
	};
	idx_t dimension = DConstants::INVALID_INDEX;
	if (!argument) {
		if (!model.time_spine.IsValid()) {
			fail(StringUtil::Format("%s needs dimension := <dimension> (semantic view \"%s\" has no TIME)", what,
			                        model.Name().name));
		}
		dimension = model.FindMember(model.time_spine.GetIndex(), SEMANTIC_TIME_SPINE_COLUMN).GetIndex();
	} else {
		auto ref = SemanticMarkers::Classify(*argument);
		if (!ref.Is(SemanticRefKind::MEMBER) || model.members[ref.index].Kind() != SemanticMemberKind::DIMENSION) {
			fail(StringUtil::Format("%s: \"%s\" is not a dimension", what, ArgumentName(model, *argument)));
		}
		dimension = ref.index;
	}
	auto column = SemanticMarkers::Classify(*model.members[dimension].resolved);
	if (!column.Is(SemanticRefKind::COLUMN)) {
		fail(StringUtil::Format("%s: dimension \"%s\" must be defined as a column of its logical table", what,
		                        model.MemberDisplayName(dimension)));
	}
	for (auto &atom : atoms) {
		if (!model.Reaches(atom.home, column.index)) {
			fail(StringUtil::Format("%s: dimension \"%s\" is not reachable from logical table \"%s\"", what,
			                        model.MemberDisplayName(dimension), model.tables[atom.home].name));
		}
	}
	return dimension;
}

unique_ptr<ParsedExpression> SemanticResolveTransforms(ClientContext &context, SemanticAnalyzer &analyzer,
                                                       const SemanticModel &model,
                                                       unique_ptr<ParsedExpression> resolved, SemanticMemberKind kind,
                                                       const string &what) {
	if (kind != SemanticMemberKind::METRIC) {
		auto call = SemanticFindTransformCall(*resolved);
		if (call) {
			throw SemanticDefinitionException("%s: %s() is only allowed in METRICS", what,
			                                  call->FunctionName().GetIdentifierName());
		}
		return resolved;
	}
	TransformResolver resolver(context, analyzer, model, what);
	resolver.Rewrite(resolved, false);
	return resolved;
}

} // namespace duckdb
