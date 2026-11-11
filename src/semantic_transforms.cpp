#include "semantic_transforms.hpp"

#include "duckdb/common/error_data.hpp"
#include "duckdb/common/exception.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/function/scalar_function.hpp"
#include "duckdb/main/extension/extension_loader.hpp"
#include "duckdb/parser/parsed_data/create_scalar_function_info.hpp"
#include "duckdb/parser/expression/cast_expression.hpp"
#include "duckdb/parser/expression/constant_expression.hpp"
#include "duckdb/parser/expression/function_expression.hpp"
#include "duckdb/parser/parsed_expression_iterator.hpp"
#include "duckdb/parser/query_node/select_node.hpp"
#include "duckdb/parser/statement/select_statement.hpp"
#include "duckdb/parser/tableref/emptytableref.hpp"
#include "duckdb/planner/binder.hpp"
#include "semantic_analysis.hpp"
#include "semantic_cumulative.hpp"
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

string SemanticBindError(ClientContext &context, unique_ptr<ParsedExpression> expr, optional_ptr<LogicalType> type) {
	auto node = make_uniq<SelectNode>();
	node->select_list.push_back(std::move(expr));
	node->from_table = make_uniq<EmptyTableRef>();
	auto statement = make_uniq<SelectStatement>();
	statement->node = std::move(node);
	try {
		auto binder = Binder::CreateBinder(context);
		auto bound = binder->Bind(statement->Cast<SQLStatement>());
		if (type) {
			*type = bound.types[0];
		}
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

SemanticCumulative SemanticMakeCumulative(ClientContext &context, SemanticAnalyzer &analyzer,
                                          const SemanticModel &model, idx_t dimension, const ParsedExpression *window,
                                          const string &part, SemanticCumulativePeriod period, const string &what) {
	auto column = SemanticMarkers::Classify(*model.members[dimension].resolved);
	if (!column.Is(SemanticRefKind::COLUMN)) {
		throw InternalException("CUMULATIVE dimension \"%s\" is not defined as a column",
		                        model.MemberDisplayName(dimension));
	}
	auto &table = model.tables[column.index];
	SemanticCumulative cumulative;
	cumulative.dimension = dimension;
	cumulative.table = column.index;
	cumulative.column = column.column;
	cumulative.type = table.column_types[table.columns.at(column.column)];
	cumulative.period = period;
	cumulative.kind = SemanticCumulativeKind::ALL;
	if (window) {
		if (!SemanticIsConstant(analyzer, *window)) {
			throw SemanticDefinitionException("%s: the window must be a constant expression", what);
		}
		cumulative.kind = SemanticCumulativeKind::WINDOW;
		cumulative.window = window->Copy();
	} else if (!part.empty()) {
		if (!SemanticIsTruncationPart(part)) {
			throw SemanticDefinitionException(
			    "%s: \"%s\" is not a date part to truncate to (e.g. day, week, month, quarter, year)", what, part);
		}
		cumulative.kind = SemanticCumulativeKind::GRAIN;
		cumulative.part = part;
	}
	// binds SELECT <window condition over CAST(NULL AS <type>)>
	auto null = [&]() {
		return make_uniq<CastExpression>(cumulative.type, ConstantExpression::FromValue(Value()));
	};
	auto error = SemanticBindError(context, SemanticCumulativeWindow(cumulative, null(), null()));
	if (error.empty()) {
		return cumulative;
	}
	auto description = "column \"" + table.name + "." + cumulative.column + "\" (" + cumulative.type.ToString() + ")";
	switch (cumulative.kind) {
	case SemanticCumulativeKind::WINDOW:
		throw SemanticDefinitionException("%s: cannot use %s as the window of %s: %s", what,
		                                  cumulative.window->ToString(), description, error);
	case SemanticCumulativeKind::GRAIN:
		throw SemanticDefinitionException("%s: cannot truncate %s to %s: %s", what, description, cumulative.part,
		                                  error);
	default:
		throw SemanticDefinitionException("%s: %s cannot be ordered: %s", what, description, error);
	}
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
			auto alias = node->GetAlias();
			node = IsTimeShift(call) ? TimeShift(call) : Cumulative(call);
			node->SetAlias(alias);
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

	//! cumulative(<metric> [, trailing_window := <interval> | to_date := '<date part>']
	//!            [, period := 'first' | 'last' | 'average'] [, dimension := <dimension>])
	unique_ptr<ParsedExpression> Cumulative(const FunctionExpression &call) {
		auto call_what = CallWhat(call, what);
		auto arguments =
		    SemanticSplitTransformArguments(call, 1, {"trailing_window", "to_date", "period", "dimension"}, call_what);
		auto named = [&](const char *name) -> optional_ptr<const ParsedExpression> {
			auto entry = arguments.named.find(name);
			return entry == arguments.named.end() ? nullptr : entry->second;
		};
		auto window = named("trailing_window");
		auto to_date = named("to_date");
		if (window && to_date) {
			throw SemanticDefinitionException("%s: trailing_window and to_date cannot be combined", call_what);
		}
		if (window && SemanticIsConstant(analyzer, *window)) {
			LogicalType type;
			auto error = SemanticBindError(context, window->Copy(), type);
			if (!error.empty() || type.id() != LogicalTypeId::INTERVAL) {
				throw SemanticDefinitionException("%s: trailing_window must be an interval (e.g. INTERVAL 7 DAYS)%s",
				                                  call_what, Got(*window));
			}
		}
		string part;
		if (to_date) {
			if (!StringConstant(*to_date, part)) {
				throw SemanticDefinitionException("%s: to_date must be a date part string (e.g. to_date := 'month')%s",
				                                  call_what, Got(*to_date));
			}
			part = StringUtil::Lower(part);
		}
		auto period = SemanticCumulativePeriod::LAST;
		auto period_argument = named("period");
		if (period_argument) {
			string value; // empty unless a string literal
			StringConstant(*period_argument, value);
			if (StringUtil::CIEquals(value, "first")) {
				period = SemanticCumulativePeriod::FIRST;
			} else if (StringUtil::CIEquals(value, "average")) {
				period = SemanticCumulativePeriod::AVERAGE;
			} else if (!StringUtil::CIEquals(value, "last")) {
				throw SemanticDefinitionException("%s: period must be 'first', 'last' or 'average'%s", call_what,
				                                  Got(*period_argument));
			}
		}
		auto &metric = *arguments.positional[0];
		CheckNoWindow(metric, call_what);
		// before the dimension's reachability: a metric that cannot be cumulated says why
		CheckCumulativeComposition(metric, call_what);
		vector<SemanticAtom> atoms;
		auto post = analyzer.Decompose(metric, SemanticMetricContext(), atoms, what);
		for (auto &atom : atoms) {
			// CheckCumulativeComposition names the culprit; this guards the invariant Decompose relies on
			if (!atom.context.offsets.empty() || atom.context.cumulative != DConstants::INVALID_INDEX ||
			    atom.context.non_additive_member != DConstants::INVALID_INDEX) {
				throw SemanticDefinitionException(
				    "%s: the metric expression cannot use OFFSET, CUMULATIVE or NON ADDITIVE BY", call_what);
			}
		}
		// AVERAGE averages each aggregate: only a metric that is a single aggregate has its value averaged
		if (period == SemanticCumulativePeriod::AVERAGE &&
		    (atoms.size() != 1 || !SemanticMarkers::Classify(*post).Is(SemanticRefKind::ATOM))) {
			throw SemanticDefinitionException(
			    "%s: period := 'average' requires a metric computed from a single aggregate (averaging each aggregate "
			    "of an expression would not average its value)",
			    call_what);
		}
		auto dimension = SemanticTransformDimension(model, named("dimension"), atoms, call_what);
		auto cumulative =
		    SemanticMakeCumulative(context, analyzer, model, dimension, window.get(), part, period, call_what);
		auto index = analyzer.Transforms().AddCumulative(std::move(cumulative));
		return SemanticMarkers::Transform(SemanticTransformKind::CUMULATIVE, index, metric.Copy());
	}

	//! ", got <value>" for a constant argument (other arguments hold internal markers)
	string Got(const ParsedExpression &argument) {
		return SemanticIsConstant(analyzer, argument) ? ", got " + argument.ToString() : string();
	}

	//! The value of a string literal
	static bool StringConstant(const ParsedExpression &expr, string &result) {
		if (expr.GetExpressionClass() != ExpressionClass::CONSTANT) {
			return false;
		}
		auto value = expr.Cast<ConstantExpression>().GetLiteral().ToValue();
		if (value.IsNull() || value.type().id() != LogicalTypeId::VARCHAR) {
			return false;
		}
		result = StringValue::Get(value);
		return true;
	}

	//! Like CUMULATIVE metrics, cumulative() builds on plain metrics only: no OFFSET, CUMULATIVE or NON ADDITIVE BY
	//! below it (time_shift() / cumulative() calls are markers by now)
	void CheckCumulativeComposition(const ParsedExpression &expr, const string &call_what) {
		auto transform = SemanticMarkers::ClassifyTransform(expr);
		if (transform.kind != SemanticTransformKind::NONE) {
			throw SemanticDefinitionException("%s: the metric expression cannot contain %s()", call_what,
			                                  transform.kind == SemanticTransformKind::OFFSET ? TIME_SHIFT_FUNCTION
			                                                                                  : CUMULATIVE_FUNCTION);
		}
		auto ref = SemanticMarkers::Classify(expr);
		if (ref.Is(SemanticRefKind::MEMBER)) {
			if (model.members[ref.index].Kind() == SemanticMemberKind::METRIC) {
				CheckCumulativeMetric(ref.index, call_what);
			}
			return;
		}
		ParsedExpressionIterator::EnumerateChildren(
		    expr, [&](const ParsedExpression &child) { CheckCumulativeComposition(child, call_what); });
	}

	//! `metric` and the metrics below it (CheckCumulativeComposition of the model builder)
	void CheckCumulativeMetric(idx_t metric, const string &call_what) {
		auto &member = model.members[metric];
		const char *clause = member.offset.IsValid()        ? "OFFSET"
		                     : member.cumulative.IsValid()  ? "CUMULATIVE"
		                     : !member.non_additive.empty() ? "NON ADDITIVE BY"
		                                                    : nullptr;
		if (clause) {
			throw SemanticDefinitionException("%s: the metric expression cannot reference metric \"%s\", which uses %s",
			                                  call_what, model.MemberDisplayName(metric), clause);
		}
		CheckCumulativeComposition(*member.resolved, call_what);
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

//===--------------------------------------------------------------------===//
// Catalog entries
//===--------------------------------------------------------------------===//
// The calls are rewritten before binding inside semantic queries, so these entries are never executed: they make
// the functions discoverable in duckdb_functions(), and their bind fails anywhere else.
static unique_ptr<FunctionData> SemanticTransformOutsideQuery(const string &name) {
	throw BinderException("%s() can only be used in the METRICS of a semantic query (SEMANTIC_VIEW(...) or a query of "
	                      "a semantic view by name)",
	                      name);
}

static unique_ptr<FunctionData> TimeShiftBind(BindScalarFunctionInput &) {
	return SemanticTransformOutsideQuery("time_shift");
}

static unique_ptr<FunctionData> CumulativeBind(BindScalarFunctionInput &) {
	return SemanticTransformOutsideQuery("cumulative");
}

static void SemanticTransformExecute(DataChunk &, ExpressionState &, Vector &) {
	throw InternalException("query-time transforms are rewritten before execution");
}

static void RegisterSemanticTransform(ExtensionLoader &loader, ScalarFunction function, string description,
                                      vector<string> examples) {
	function.SetNullHandling(FunctionNullHandling::SPECIAL_HANDLING);
	CreateScalarFunctionInfo info(std::move(function));
	FunctionDescription entry;
	entry.description = std::move(description);
	entry.examples = std::move(examples);
	info.descriptions.push_back(std::move(entry));
	loader.RegisterFunction(std::move(info));
}

void RegisterSemanticTransformFunctions(ExtensionLoader &loader) {
	RegisterSemanticTransform(
	    loader,
	    ScalarFunction("time_shift",
	                   {FunctionParameter("metric", LogicalType::ANY), FunctionParameter("interval", LogicalType::ANY),
	                    FunctionParameter("dimension", LogicalType::ANY, Value(LogicalType::SQLNULL))},
	                   LogicalType::ANY, SemanticTransformExecute, TimeShiftBind),
	    "Query-time form of an OFFSET metric: the metric computed over the rows dated one interval earlier (a "
	    "negative interval looks forward). Only valid in the METRICS of a semantic query; dimension defaults to "
	    "metric_time.day.",
	    {"SELECT * FROM SEMANTIC_VIEW(sales DIMENSIONS metric_time.month METRICS time_shift(revenue, INTERVAL 1 YEAR) "
	     "AS revenue_ly)",
	     "SELECT metric_time.month, time_shift(AGG(revenue), INTERVAL 1 MONTH, dimension := orders.order_date) FROM "
	     "sales GROUP BY metric_time.month"});
	RegisterSemanticTransform(
	    loader,
	    ScalarFunction("cumulative",
	                   {FunctionParameter("metric", LogicalType::ANY),
	                    FunctionParameter("trailing_window", LogicalType::ANY, Value(LogicalType::SQLNULL)),
	                    FunctionParameter("to_date", LogicalType::ANY, Value(LogicalType::SQLNULL)),
	                    FunctionParameter("period", LogicalType::ANY, Value(LogicalType::SQLNULL)),
	                    FunctionParameter("dimension", LogicalType::ANY, Value(LogicalType::SQLNULL))},
	                   LogicalType::ANY, SemanticTransformExecute, CumulativeBind),
	    "Query-time form of a CUMULATIVE metric: a running total, a trailing window (trailing_window) or a period to "
	    "date (to_date) of the metric. Only valid in the METRICS of a semantic query; dimension defaults to "
	    "metric_time.day.",
	    {"SELECT * FROM SEMANTIC_VIEW(sales DIMENSIONS metric_time.month METRICS cumulative(revenue) AS running)",
	     "SELECT * FROM SEMANTIC_VIEW(sales DIMENSIONS metric_time.day METRICS cumulative(revenue, trailing_window := "
	     "INTERVAL 7 DAYS) AS revenue_7d)",
	     "SELECT * FROM SEMANTIC_VIEW(sales DIMENSIONS metric_time.month METRICS cumulative(revenue, to_date := "
	     "'year', period := 'first') AS revenue_ytd)"});
}

} // namespace duckdb
