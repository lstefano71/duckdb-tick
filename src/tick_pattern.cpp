#include "tick_pattern.hpp"

#include "duckdb/common/exception.hpp"
#include "duckdb/common/types/date.hpp"
#include "duckdb/common/types/timestamp.hpp"

#include <algorithm>

namespace duckdb {

static bool TryParseDigits(const string &text, idx_t &pos, idx_t count, int32_t &result) {
	if (pos + count > text.size()) {
		return false;
	}
	result = 0;
	for (idx_t i = 0; i < count; i++) {
		auto c = text[pos + i];
		if (c < '0' || c > '9') {
			return false;
		}
		result = result * 10 + (c - '0');
	}
	pos += count;
	return true;
}

static bool TryParseDay(const string &text, date_t &result) {
	idx_t pos = 0;
	int32_t year, month, day;
	if (!TryParseDigits(text, pos, 4, year) || pos >= text.size() || text[pos++] != '-' ||
	    !TryParseDigits(text, pos, 2, month) || pos >= text.size() || text[pos++] != '-' ||
	    !TryParseDigits(text, pos, 2, day) || pos != text.size()) {
		return false;
	}
	if (!Date::IsValid(year, month, day)) {
		return false;
	}
	result = Date::FromDate(year, month, day);
	return true;
}

void MergeTickIntervals(vector<TickInterval> &intervals) {
	std::sort(intervals.begin(), intervals.end(),
	          [](const TickInterval &a, const TickInterval &b) { return a.lo < b.lo; });
	idx_t count = 0;
	for (auto &interval : intervals) {
		if (count > 0 && interval.lo <= intervals[count - 1].hi) {
			intervals[count - 1].hi = MaxValue(intervals[count - 1].hi, interval.hi);
		} else {
			intervals[count++] = interval;
		}
	}
	intervals.resize(count);
}

vector<TickInterval> ExpandTickPattern(const string &pattern, TickEnvironment &env) {
	date_t day;
	if (!TryParseDay(pattern, day)) {
		throw InvalidInputException("Invalid TICK pattern: '%s'", pattern);
	}
	auto lo = Timestamp::FromDatetime(day, dtime_t(0)).value;
	vector<int64_t> bounds {lo, lo + Interval::MICROS_PER_DAY};
	env.time_zones.LocalToUtc(env.time_zone, bounds);
	return {{bounds[0], bounds[1]}};
}

} // namespace duckdb
