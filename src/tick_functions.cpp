#include "tick_functions.hpp"
#include "tick_pattern.hpp"

#include "duckdb/common/exception.hpp"
#include "duckdb/function/function_binder.hpp"
#include "duckdb/function/scalar_function.hpp"
#include "duckdb/main/extension/extension_loader.hpp"
#include "duckdb/planner/expression/bound_function_expression.hpp"
#include "duckdb/storage/statistics/numeric_stats.hpp"

#include <algorithm>

namespace duckdb {

struct TickInBindData final : public FunctionData {
	explicit TickInBindData(vector<TickInterval> intervals_p) : intervals(std::move(intervals_p)) {
	}

	vector<TickInterval> intervals;

	unique_ptr<FunctionData> Copy() const override {
		return make_uniq<TickInBindData>(intervals);
	}

	bool Equals(const FunctionData &other_p) const override {
		auto &other = other_p.Cast<TickInBindData>();
		if (intervals.size() != other.intervals.size()) {
			return false;
		}
		for (idx_t i = 0; i < intervals.size(); i++) {
			if (intervals[i].lo != other.intervals[i].lo || intervals[i].hi != other.intervals[i].hi) {
				return false;
			}
		}
		return true;
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

static unique_ptr<FunctionData> TickInBind(BindScalarFunctionInput &input) {
	auto pattern = input.TryGetConstant(1);
	if (!pattern) {
		throw BinderException("tick_in: the TICK pattern must be a constant");
	}
	if (pattern->IsNull()) {
		return make_uniq<TickInBindData>(vector<TickInterval>());
	}
	return make_uniq<TickInBindData>(ExpandTickPattern(StringValue::Get(*pattern)));
}

static void TickInFunction(DataChunk &args, ExpressionState &state, Vector &result) {
	auto &info = state.expr.Cast<BoundFunctionExpression>().BindInfo()->Cast<TickInBindData>();
	BinaryExecutor::Execute<timestamp_t, string_t, bool>(
	    args.data[0], args.data[1], result, [&](timestamp_t ts, string_t) { return info.Contains(ts.value); });
}

//! Prunes row groups and files whose [min, max] does not overlap any interval
static FilterPropagateResult TickInFilterPrune(const FunctionStatisticsPruneInput &input) {
	auto stats = input.ChildStats(0);
	if (!input.bind_data || !stats || !NumericStats::HasMinMax(*stats)) {
		return FilterPropagateResult::NO_PRUNING_POSSIBLE;
	}
	auto &info = input.bind_data->Cast<TickInBindData>();
	auto min = NumericStats::GetMin<timestamp_t>(*stats).value;
	auto max = NumericStats::GetMax<timestamp_t>(*stats).value;
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

void RegisterTickFunctions(ExtensionLoader &loader) {
	ScalarFunctionSet tick_in("tick_in");

	ScalarFunction timestamp_fun({LogicalType::TIMESTAMP, LogicalType::VARCHAR}, LogicalType::BOOLEAN, TickInFunction,
	                             TickInBind);
	timestamp_fun.SetFilterPruneCallback(TickInFilterPrune);
	tick_in.AddFunction(timestamp_fun);

	ScalarFunction string_fun({LogicalType::VARCHAR, LogicalType::VARCHAR}, LogicalType::BOOLEAN,
	                          TickInStringFunction);
	string_fun.SetBindExpressionCallback(TickInStringBindExpression);
	tick_in.AddFunction(string_fun);

	loader.RegisterFunction(tick_in);
}

} // namespace duckdb
