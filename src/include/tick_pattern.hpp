#pragma once

#include "duckdb.hpp"

namespace duckdb {

//! A half-open interval [lo, hi) of microseconds since the epoch
struct TickInterval {
	int64_t lo;
	int64_t hi;
};

//! Expands a TICK pattern into sorted, non-overlapping intervals; throws InvalidInputException on bad input
vector<TickInterval> ExpandTickPattern(const string &pattern);

} // namespace duckdb
