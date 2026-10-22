#include "tick_functions.hpp"
#include "tick_pattern.hpp"

#include "duckdb/common/exception.hpp"
#include "duckdb/common/vector/vector_writer.hpp"
#include "duckdb/function/function_binder.hpp"
#include "duckdb/function/scalar_function.hpp"
#include "duckdb/function/table_function.hpp"
#include "duckdb/main/config.hpp"
#include "duckdb/main/extension/extension_loader.hpp"
#include "duckdb/planner/expression/bound_comparison_expression.hpp"
#include "duckdb/planner/expression/bound_conjunction_expression.hpp"
#include "duckdb/planner/expression/bound_constant_expression.hpp"
#include "duckdb/planner/expression/bound_function_expression.hpp"
#include "duckdb/storage/statistics/numeric_stats.hpp"
#include "duckdb/transaction/meta_transaction.hpp"

#include <algorithm>

namespace duckdb {

static constexpr const char *TICK_NOW_SETTING = "tick_now";

//! Expands a pattern for the current query: UTC instants for TIMESTAMPTZ, wall-clock times in TimeZone otherwise
static vector<TickInterval> ExpandForContext(ClientContext &context, const string &pattern, bool utc) {
	int64_t now = MetaTransaction::Get(context).start_timestamp.value;
	Value value;
	if (context.TryGetCurrentSetting(TICK_NOW_SETTING, value) && !value.IsNull()) {
		now = value.GetValue<timestamp_tz_t>().value;
	}
	string time_zone = "UTC";
	if (context.TryGetCurrentSetting("TimeZone", value) && !value.IsNull()) {
		time_zone = value.ToString();
	}
	auto time_zones = CreateTickTimeZones(context);
	TickEnvironment env(now, time_zone, *time_zones);
	auto intervals = ExpandTickPattern(pattern, env);
	if (utc) {
		return intervals;
	}
	vector<int64_t> bounds;
	bounds.reserve(intervals.size() * 2);
	for (auto &interval : intervals) {
		bounds.push_back(interval.lo);
		bounds.push_back(interval.hi);
	}
	time_zones->UtcToLocal(time_zone, bounds);
	for (idx_t i = 0; i < intervals.size(); i++) {
		intervals[i] = {bounds[2 * i], bounds[2 * i + 1]};
	}
	// a fall-back DST transition can make wall-clock intervals overlap
	MergeTickIntervals(intervals);
	return intervals;
}

struct TickInBindData final : public FunctionData {
	explicit TickInBindData(vector<TickInterval> intervals_p) : intervals(std::move(intervals_p)) {
	}

	vector<TickInterval> intervals;

	unique_ptr<FunctionData> Copy() const override {
		return make_uniq<TickInBindData>(intervals);
	}

	bool Equals(const FunctionData &other_p) const override {
		return intervals == other_p.Cast<TickInBindData>().intervals;
	}

	//! The first interval whose end lies after value, or end() if there is none
	vector<TickInterval>::const_iterator FirstEndingAfter(int64_t value) const {
		return std::upper_bound(intervals.begin(), intervals.end(), value,
		                        [](int64_t v, const TickInterval &interval) { return v < interval.hi; });
	}

	bool Contains(int64_t value) const {
		auto it = FirstEndingAfter(value);
		return it != intervals.end() && it->lo <= value;
	}
};

//! Multiplies, saturating at the int64 range
static int64_t ScaleSaturating(int64_t value, int64_t scale) {
	if (value > NumericLimits<int64_t>::Maximum() / scale) {
		return NumericLimits<int64_t>::Maximum();
	}
	if (value < NumericLimits<int64_t>::Minimum() / scale) {
		return NumericLimits<int64_t>::Minimum();
	}
	return value * scale;
}

//! SCALE converts the microsecond intervals to the unit of the column (1000 for nanoseconds)
template <bool UTC, int64_t SCALE>
static unique_ptr<FunctionData> TickInBind(BindScalarFunctionInput &input) {
	auto pattern = input.TryGetConstant(1);
	if (!pattern) {
		throw BinderException("tick_in: the TICK pattern must be a constant");
	}
	if (pattern->IsNull()) {
		return make_uniq<TickInBindData>(vector<TickInterval>());
	}
	auto &context = input.GetClientContext();
	auto intervals = ExpandForContext(context, StringValue::Get(*pattern), UTC);
	if (SCALE != 1) {
		for (auto &interval : intervals) {
			interval = {ScaleSaturating(interval.lo, SCALE), ScaleSaturating(interval.hi, SCALE)};
		}
	}
	return make_uniq<TickInBindData>(std::move(intervals));
}

template <class T>
static void TickInFunction(DataChunk &args, ExpressionState &state, Vector &result) {
	auto &info = state.expr.Cast<BoundFunctionExpression>().BindInfo()->Cast<TickInBindData>();
	BinaryExecutor::Execute<T, string_t, bool>(args.data[0], args.data[1], result,
	                                           [&](T ts, string_t) { return info.Contains(ts.value); });
}

//! Prunes row groups and files whose [min, max] does not overlap any interval
template <class T>
static FilterPropagateResult TickInFilterPrune(const FunctionStatisticsPruneInput &input) {
	auto stats = input.ChildStats(0);
	if (!input.bind_data || !stats || !NumericStats::HasMinMax(*stats)) {
		return FilterPropagateResult::NO_PRUNING_POSSIBLE;
	}
	auto &info = input.bind_data->Cast<TickInBindData>();
	auto min = NumericStats::GetMin<T>(*stats).value;
	auto max = NumericStats::GetMax<T>(*stats).value;
	auto it = info.FirstEndingAfter(min);
	if (it == info.intervals.end() || it->lo > max) {
		return FilterPropagateResult::FILTER_ALWAYS_FALSE;
	}
	if (it->lo <= min && max < it->hi) {
		return stats->CanHaveNull() ? FilterPropagateResult::FILTER_TRUE_OR_NULL
		                            : FilterPropagateResult::FILTER_ALWAYS_TRUE;
	}
	return FilterPropagateResult::NO_PRUNING_POSSIBLE;
}

static Value MakeValue(timestamp_t value) {
	return Value::TIMESTAMP(value);
}

static Value MakeValue(timestamp_tz_t value) {
	return Value::TIMESTAMPTZ(value);
}

static Value MakeValue(timestamp_ns_t value) {
	return Value::TIMESTAMPNS(value);
}

static Value MakeValue(timestamp_tz_ns_t value) {
	return Value::TIMESTAMPTZNS(value);
}

//! Wraps tick_in in `x >= first AND x < last AND tick_in(...)`: scanners that cannot evaluate tick_in can still
//! push down the two plain comparisons
template <class T>
static unique_ptr<Expression> TickInBindExpression(FunctionBindExpressionInput &input) {
	if (!input.bind_data || input.children[0]->IsVolatile()) {
		return nullptr;
	}
	auto &intervals = input.bind_data->Cast<TickInBindData>().intervals;
	if (intervals.empty()) {
		return nullptr;
	}
	auto &value = input.children[0];
	auto lower = BoundComparisonExpression::Create(ExpressionType::COMPARE_GREATERTHANOREQUALTO, value->Copy(),
	                                               make_uniq<BoundConstantExpression>(MakeValue(T(intervals.front().lo))));
	auto upper = BoundComparisonExpression::Create(ExpressionType::COMPARE_LESSTHAN, value->Copy(),
	                                               make_uniq<BoundConstantExpression>(MakeValue(T(intervals.back().hi))));
	auto tick_in =
	    make_uniq<BoundFunctionExpression>(input.bound_function, std::move(input.children), input.bind_data->Copy());
	auto result =
	    make_uniq<BoundConjunctionExpression>(ExpressionType::CONJUNCTION_AND, std::move(lower), std::move(upper));
	result->GetChildrenMutable().push_back(std::move(tick_in));
	return std::move(result);
}

template <class T, bool UTC, int64_t SCALE = 1>
static ScalarFunction GetTickInFunction(const LogicalType &type) {
	ScalarFunction function({type, LogicalType::VARCHAR}, LogicalType::BOOLEAN, TickInFunction<T>,
	                        TickInBind<UTC, SCALE>);
	function.SetFilterPruneCallback(TickInFilterPrune<T>);
	function.SetBindExpressionCallback(TickInBindExpression<T>);
	// depends on the transaction start time and the TimeZone setting
	function.SetStability(FunctionStability::CONSISTENT_WITHIN_QUERY);
	return function;
}

static void TickInStringFunction(DataChunk &args, ExpressionState &state, Vector &result) {
	throw InternalException("tick_in(VARCHAR, VARCHAR) should have been rewritten to contains");
}

//! `'ell' IN 'hello'` keeps DuckDB's meaning: contains('hello', 'ell')
static unique_ptr<Expression> TickInStringBindExpression(FunctionBindExpressionInput &input) {
	vector<unique_ptr<Expression>> children;
	children.push_back(std::move(input.children[1]));
	children.push_back(std::move(input.children[0]));
	return FunctionBinder(input.context).BindScalarFunction(Identifier::DefaultSchema(), "contains",
	                                                        std::move(children));
}

struct TickIntervalsBindData final : public TableFunctionData {
	vector<TickInterval> intervals;
};

struct TickIntervalsState final : public GlobalTableFunctionState {
	idx_t offset = 0;
};

static unique_ptr<FunctionData> TickIntervalsBind(ClientContext &context, TableFunctionBindInput &input,
                                                  vector<LogicalType> &return_types, vector<Identifier> &names) {
	return_types = {LogicalType::TIMESTAMP_TZ, LogicalType::TIMESTAMP_TZ};
	names = {Identifier("lo"), Identifier("hi")};
	auto result = make_uniq<TickIntervalsBindData>();
	if (!input.inputs[0].IsNull()) {
		result->intervals = ExpandForContext(context, StringValue::Get(input.inputs[0]), true);
	}
	return std::move(result);
}

static unique_ptr<GlobalTableFunctionState> TickIntervalsInit(ClientContext &context, TableFunctionInitInput &input) {
	return make_uniq<TickIntervalsState>();
}

static void TickIntervalsFunction(ClientContext &context, TableFunctionInput &data, DataChunk &output) {
	auto &intervals = data.bind_data->Cast<TickIntervalsBindData>().intervals;
	auto &state = data.global_state->Cast<TickIntervalsState>();
	auto count = MinValue<idx_t>(STANDARD_VECTOR_SIZE, intervals.size() - state.offset);
	{
		auto lo_writer = FlatVector::Writer<timestamp_tz_t>(output.data[0], count);
		auto hi_writer = FlatVector::Writer<timestamp_tz_t>(output.data[1], count);
		for (idx_t i = 0; i < count; i++) {
			lo_writer.WriteValue(timestamp_tz_t(intervals[state.offset + i].lo));
			hi_writer.WriteValue(timestamp_tz_t(intervals[state.offset + i].hi));
		}
	}
	output.SetCardinality(count);
	state.offset += count;
}

void RegisterTickFunctions(ExtensionLoader &loader) {
	auto &config = DBConfig::GetConfig(loader.GetDatabaseInstance());
	config.AddExtensionOption(TICK_NOW_SETTING, "Overrides the instant $now refers to in TICK patterns",
	                          LogicalType::TIMESTAMP_TZ);

	ScalarFunctionSet tick_in("tick_in");
	tick_in.AddFunction(GetTickInFunction<timestamp_t, false>(LogicalType::TIMESTAMP));
	tick_in.AddFunction(GetTickInFunction<timestamp_tz_t, true>(LogicalType::TIMESTAMP_TZ));
	tick_in.AddFunction(GetTickInFunction<timestamp_ns_t, false, Interval::NANOS_PER_MICRO>(LogicalType::TIMESTAMP_NS));
	tick_in.AddFunction(
	    GetTickInFunction<timestamp_tz_ns_t, true, Interval::NANOS_PER_MICRO>(LogicalType::TIMESTAMP_TZ_NS));
	ScalarFunction string_fun({LogicalType::VARCHAR, LogicalType::VARCHAR}, LogicalType::BOOLEAN,
	                          TickInStringFunction);
	string_fun.SetBindExpressionCallback(TickInStringBindExpression);
	tick_in.AddFunction(string_fun);
	loader.RegisterFunction(tick_in);

	TableFunction tick_intervals("tick_intervals", {LogicalType::VARCHAR}, TickIntervalsFunction, TickIntervalsBind,
	                             TickIntervalsInit);
	loader.RegisterFunction(tick_intervals);
}

} // namespace duckdb
