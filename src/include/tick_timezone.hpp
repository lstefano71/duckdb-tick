#pragma once

#include "duckdb.hpp"

namespace duckdb {

//! Converts between wall-clock times in a time zone and UTC instants, both as microseconds since the epoch.
//! Fixed offsets (+03:00, +0300, +03, UTC, GMT, Z) are handled directly; named zones go through ICU.
class TickTimeZones {
public:
	virtual ~TickTimeZones() = default;

	//! Wall-clock times in `tz` to UTC instants, in place; throws InvalidInputException for an unknown zone
	virtual void LocalToUtc(const string &tz, vector<int64_t> &values) = 0;
	//! UTC instants to wall-clock times in `tz`, in place
	virtual void UtcToLocal(const string &tz, vector<int64_t> &values) = 0;
};

//! Parses a fixed UTC offset; returns false if `tz` is not one
bool TryParseFixedOffset(const string &tz, int64_t &offset_micros);

//! Time zone conversions for a client context, using the ICU extension for named zones
unique_ptr<TickTimeZones> CreateTickTimeZones(ClientContext &context);

} // namespace duckdb
