#pragma once

#include "duckdb.hpp"
#include "tick_timezone.hpp"

namespace duckdb {

//! A half-open interval [lo, hi) of microseconds since the epoch
struct TickInterval {
	int64_t lo;
	int64_t hi;

	bool operator==(const TickInterval &other) const {
		return lo == other.lo && hi == other.hi;
	}
};

//! What a pattern is expanded against
struct TickEnvironment {
	TickEnvironment(int64_t now_p, string time_zone_p, TickTimeZones &time_zones_p)
	    : now(now_p), time_zone(std::move(time_zone_p)), time_zones(time_zones_p) {
	}

	//! The instant $now refers to, in UTC microseconds
	int64_t now;
	//! The zone of patterns without @timezone (and of $today)
	string time_zone;
	TickTimeZones &time_zones;
};

//! Expands a TICK pattern into sorted, non-overlapping UTC intervals; throws InvalidInputException on bad input
vector<TickInterval> ExpandTickPattern(const string &pattern, TickEnvironment &env);

//! Sorts intervals and merges the ones that overlap or touch
void MergeTickIntervals(vector<TickInterval> &intervals);

} // namespace duckdb
