#include "tick_pattern.hpp"

#include "duckdb/common/exception.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/common/types/date.hpp"
#include "duckdb/common/types/interval.hpp"
#include "duckdb/common/types/timestamp.hpp"

#include <algorithm>

// The semantics follow QuestDB's implementation, see docs/TICK_SEMANTICS.md. Intervals are built as inclusive
// [lo, hi] wall-clock times, like QuestDB, and become half-open UTC intervals at the very end.

namespace duckdb {

namespace {

constexpr idx_t MAX_INTERVALS = 1000000;
constexpr int64_t MAX_NUMBER = 999999999;

[[noreturn]] void Fail(const string &message) {
	throw InvalidInputException(message);
}

bool IsDigit(char c) {
	return c >= '0' && c <= '9';
}

bool IsSpace(char c) {
	return c == ' ' || c == '\t' || c == '\n' || c == '\r';
}

string Trim(const string &text) {
	idx_t begin = 0;
	idx_t end = text.size();
	while (begin < end && IsSpace(text[begin])) {
		begin++;
	}
	while (end > begin && IsSpace(text[end - 1])) {
		end--;
	}
	return text.substr(begin, end - begin);
}

//! The first position of `c` outside brackets in [begin, end), or npos
idx_t FindOutsideBrackets(const string &text, char c, idx_t begin = 0, idx_t end = string::npos) {
	end = MinValue<idx_t>(end, text.size());
	idx_t depth = 0;
	for (idx_t i = begin; i < end; i++) {
		if (text[i] == '[') {
			depth++;
		} else if (text[i] == ']') {
			if (depth > 0) {
				depth--;
			}
		} else if (text[i] == c && depth == 0) {
			return i;
		}
	}
	return string::npos;
}

//! The last position of `c` outside brackets, or npos
idx_t FindLastOutsideBrackets(const string &text, char c) {
	idx_t result = string::npos;
	idx_t pos = 0;
	while ((pos = FindOutsideBrackets(text, c, pos)) != string::npos) {
		result = pos++;
	}
	return result;
}

vector<string> SplitOutsideBrackets(const string &text, char separator) {
	vector<string> result;
	idx_t begin = 0;
	while (true) {
		auto pos = FindOutsideBrackets(text, separator, begin);
		if (pos == string::npos) {
			result.push_back(text.substr(begin));
			return result;
		}
		result.push_back(text.substr(begin, pos - begin));
		begin = pos + 1;
	}
}

//! The position of the ']' matching the '[' at `open`, or npos
idx_t MatchingBracket(const string &text, idx_t open) {
	idx_t depth = 0;
	for (idx_t i = open; i < text.size(); i++) {
		if (text[i] == '[') {
			depth++;
		} else if (text[i] == ']' && --depth == 0) {
			return i;
		}
	}
	return string::npos;
}

//! Where a time part starts: 'T' followed by a digit or '[', or (for dates) a space followed by a digit; npos if
//! there is none
idx_t FindTimeStart(const string &text, bool allow_space = true) {
	for (idx_t i = 0; i + 1 < text.size(); i++) {
		auto next = text[i + 1];
		if (text[i] == 'T' && (IsDigit(next) || next == '[')) {
			return i;
		}
		if (allow_space && text[i] == ' ' && IsDigit(next)) {
			return i;
		}
	}
	return string::npos;
}

int64_t DayStart(date_t date) {
	return Timestamp::FromDatetime(date, dtime_t(0)).value;
}

date_t DateOf(int64_t micros) {
	return Timestamp::GetDate(timestamp_t(micros));
}

int64_t FloorDay(int64_t micros) {
	return DayStart(DateOf(micros));
}

bool IsWeekend(date_t date) {
	return Date::ExtractISODayOfTheWeek(date) >= 6;
}

string FormatDay(date_t date) {
	int32_t year, month, day;
	Date::Convert(date, year, month, day);
	return StringUtil::Format("%04d-%02d-%02d", year, month, day);
}

//! Adds months to a timestamp, clamping the day to the end of the target month
int64_t AddMonths(int64_t micros, int64_t months) {
	auto ts = timestamp_t(micros);
	int32_t year, month, day;
	Date::Convert(Timestamp::GetDate(ts), year, month, day);
	auto total = int64_t(year) * 12 + (month - 1) + months;
	auto new_year = int32_t(total >= 0 ? total / 12 : (total - 11) / 12);
	auto new_month = int32_t(total - int64_t(new_year) * 12) + 1;
	day = MinValue(day, Date::MonthDays(new_year, new_month));
	return Timestamp::FromDatetime(Date::FromDate(new_year, new_month, day), Timestamp::GetTime(ts)).value;
}

//===--------------------------------------------------------------------===//
// Durations
//===--------------------------------------------------------------------===//
struct DurationPart {
	int64_t amount;
	char unit;
};

int64_t AddPart(int64_t micros, const DurationPart &part) {
	switch (part.unit) {
	case 'y':
		return AddMonths(micros, part.amount * 12);
	case 'M':
		return AddMonths(micros, part.amount);
	case 'w':
		return micros + part.amount * Interval::MICROS_PER_WEEK;
	case 'd':
		return micros + part.amount * Interval::MICROS_PER_DAY;
	case 'h':
		return micros + part.amount * Interval::MICROS_PER_HOUR;
	case 'm':
		return micros + part.amount * Interval::MICROS_PER_MINUTE;
	case 's':
		return micros + part.amount * Interval::MICROS_PER_SEC;
	case 'T':
		return micros + part.amount * Interval::MICROS_PER_MSEC;
	case 'u':
		return micros + part.amount;
	case 'n':
		return micros + part.amount / Interval::NANOS_PER_MICRO;
	default:
		throw InternalException("Unknown TICK unit '%c'", part.unit);
	}
}

int64_t AddParts(int64_t micros, const vector<DurationPart> &parts) {
	for (auto &part : parts) {
		micros = AddPart(micros, part);
	}
	return micros;
}

bool IsDurationUnit(char c) {
	return c == 'y' || c == 'M' || c == 'w' || c == 'd' || c == 'h' || c == 'm' || c == 's' || c == 'T' ||
	       c == 'u' || c == 'n';
}

//! Parses digits with '_' separators into `result`; returns false if they are malformed or too large
bool TryParseNumber(const string &text, idx_t &pos, int64_t &result) {
	auto begin = pos;
	result = 0;
	bool last_underscore = false;
	while (pos < text.size() && (IsDigit(text[pos]) || text[pos] == '_')) {
		if (text[pos] == '_') {
			if (pos == begin || last_underscore) {
				return false;
			}
			last_underscore = true;
		} else {
			last_underscore = false;
			result = result * 10 + (text[pos] - '0');
			if (result > MAX_NUMBER) {
				return false;
			}
		}
		pos++;
	}
	return pos > begin && !last_underscore;
}

//! `1h30m`, `-3d2h`, `+3d`: the sign of each part applies to that part only
vector<DurationPart> ParseDuration(const string &text) {
	vector<DurationPart> parts;
	idx_t pos = 0;
	while (pos < text.size()) {
		int64_t sign = 1;
		if (text[pos] == '+' || text[pos] == '-') {
			sign = text[pos] == '-' ? -1 : 1;
			pos++;
		}
		if (pos >= text.size() || !IsDigit(text[pos])) {
			Fail(StringUtil::Format("Expected number before unit '%s'", pos < text.size() ? text.substr(pos, 1) : ""));
		}
		auto number_start = pos;
		int64_t amount;
		if (!TryParseNumber(text, pos, amount)) {
			Fail("Duration not a number: " + text.substr(number_start));
		}
		if (pos >= text.size()) {
			Fail("Missing unit at end of duration");
		}
		auto unit = text[pos++];
		if (unit == 'D') {
			unit = 'd';
		}
		if (!IsDurationUnit(unit)) {
			Fail(StringUtil::Format("Invalid duration unit: %c", unit));
		}
		parts.push_back({sign * amount, unit});
	}
	if (parts.empty()) {
		Fail("Expected number before unit ''");
	}
	return parts;
}

//===--------------------------------------------------------------------===//
// Day filters
//===--------------------------------------------------------------------===//
struct DayFilter {
	//! Bit d - 1 set for ISO weekday d (1 = Monday); 0 means no filter
	uint8_t mask = 0;

	bool Matches(date_t date) const {
		return mask == 0 || (mask & (1 << (Date::ExtractISODayOfTheWeek(date) - 1)));
	}
};

DayFilter ParseDayFilter(const string &text) {
	if (text.empty()) {
		Fail("Empty day filter after '#'");
	}
	DayFilter result;
	auto lower = StringUtil::Lower(text);
	if (lower == "workday" || lower == "wd") {
		result.mask = 0x1F;
		return result;
	}
	if (lower == "weekend") {
		result.mask = 0x60;
		return result;
	}
	static const char *const SHORT_NAMES[] = {"mon", "tue", "wed", "thu", "fri", "sat", "sun"};
	static const char *const LONG_NAMES[] = {"monday", "tuesday", "wednesday", "thursday",
	                                         "friday", "saturday", "sunday"};
	for (auto &name : StringUtil::Split(lower, ',')) {
		if (name.empty()) {
			continue;
		}
		bool found = false;
		for (idx_t d = 0; d < 7; d++) {
			if (name == SHORT_NAMES[d] || name == LONG_NAMES[d]) {
				result.mask |= uint8_t(1 << d);
				found = true;
			}
		}
		if (!found) {
			// report the name as written
			auto begin = lower.find(name);
			Fail("Invalid day name: " + text.substr(begin, name.size()));
		}
	}
	if (result.mask == 0) {
		Fail("Invalid day filter: " + text);
	}
	return result;
}

//===--------------------------------------------------------------------===//
// Concrete dates: no brackets, lists or variables left
//===--------------------------------------------------------------------===//
enum class DatePrecision : uint8_t { YEAR, MONTH, WEEK, DAY };

struct ParsedDate {
	//! The first and last day covered
	date_t first;
	date_t last;
	DatePrecision precision;
};

bool TryParseFixedDigits(const string &text, idx_t &pos, idx_t count, int32_t &result) {
	if (pos + count > text.size()) {
		return false;
	}
	result = 0;
	for (idx_t i = 0; i < count; i++) {
		if (!IsDigit(text[pos + i])) {
			return false;
		}
		result = result * 10 + (text[pos + i] - '0');
	}
	pos += count;
	return true;
}

//! Monday of ISO week 1 of an ISO week-year: the week containing January 4th
date_t IsoWeekOneMonday(int32_t year) {
	auto jan4 = Date::FromDate(year, 1, 4);
	return date_t(jan4.days - (Date::ExtractISODayOfTheWeek(jan4) - 1));
}

int32_t IsoWeeksInYear(int32_t year) {
	return (IsoWeekOneMonday(year + 1).days - IsoWeekOneMonday(year).days) / 7;
}

bool TryParseDatePart(const string &text, ParsedDate &result) {
	idx_t pos = 0;
	int32_t year, month, day;
	if (!TryParseFixedDigits(text, pos, 4, year)) {
		return false;
	}
	if (pos == text.size()) {
		result = {Date::FromDate(year, 1, 1), Date::FromDate(year, 12, 31), DatePrecision::YEAR};
		return true;
	}
	if (text[pos++] != '-') {
		return false;
	}
	if (pos < text.size() && text[pos] == 'W') {
		pos++;
		int32_t week;
		if (!TryParseFixedDigits(text, pos, 2, week) || week < 1 || week > IsoWeeksInYear(year)) {
			return false;
		}
		auto monday = date_t(IsoWeekOneMonday(year).days + (week - 1) * 7);
		if (pos == text.size()) {
			result = {monday, date_t(monday.days + 6), DatePrecision::WEEK};
			return true;
		}
		int32_t weekday;
		if (text[pos++] != '-' || !TryParseFixedDigits(text, pos, 1, weekday) || weekday < 1 || weekday > 7 ||
		    pos != text.size()) {
			return false;
		}
		auto date = date_t(monday.days + (weekday - 1));
		result = {date, date, DatePrecision::DAY};
		return true;
	}
	if (!TryParseFixedDigits(text, pos, 2, month) || month < 1 || month > 12) {
		return false;
	}
	if (pos == text.size()) {
		result = {Date::FromDate(year, month, 1), Date::FromDate(year, month, Date::MonthDays(year, month)),
		          DatePrecision::MONTH};
		return true;
	}
	if (text[pos++] != '-' || !TryParseFixedDigits(text, pos, 2, day) || pos != text.size() ||
	    !Date::IsValid(year, month, day)) {
		return false;
	}
	auto date = Date::FromDate(year, month, day);
	result = {date, date, DatePrecision::DAY};
	return true;
}

struct ParsedTime {
	int64_t micros;
	//! Length of the time's precision unit minus one microsecond
	int64_t span;
	//! An explicit UTC offset (`Z`, `+01`, `-05:30`) makes the time an absolute point
	bool has_offset = false;
	int64_t offset = 0;
};

bool TryParseTimePart(const string &text, ParsedTime &result) {
	idx_t pos = 0;
	int32_t hour, minute = 0, second = 0;
	if (!TryParseFixedDigits(text, pos, 2, hour) || hour > 23) {
		return false;
	}
	result.span = Interval::MICROS_PER_HOUR - 1;
	if (pos < text.size() && text[pos] == ':') {
		pos++;
		if (!TryParseFixedDigits(text, pos, 2, minute) || minute > 59) {
			return false;
		}
		result.span = Interval::MICROS_PER_MINUTE - 1;
		if (pos < text.size() && text[pos] == ':') {
			pos++;
			if (!TryParseFixedDigits(text, pos, 2, second) || second > 59) {
				return false;
			}
			result.span = Interval::MICROS_PER_SEC - 1;
		}
	}
	result.micros = ((int64_t(hour) * 60 + minute) * 60 + second) * Interval::MICROS_PER_SEC;
	bool has_seconds = result.span < Interval::MICROS_PER_MINUTE - 1;
	if (has_seconds && pos < text.size() && text[pos] == '.') {
		pos++;
		idx_t digits = 0;
		int64_t fraction = 0;
		while (pos < text.size() && IsDigit(text[pos])) {
			if (digits < 6) {
				fraction = fraction * 10 + (text[pos] - '0');
			}
			digits++;
			pos++;
		}
		if (digits == 0 || digits > 9) {
			return false;
		}
		int64_t scale = 1;
		for (idx_t i = MinValue<idx_t>(digits, 6); i < 6; i++) {
			scale *= 10;
		}
		result.micros += fraction * scale;
		// digits beyond microseconds truncate to a point
		result.span = digits >= 6 ? 0 : scale - 1;
	}
	if (pos == text.size()) {
		return true;
	}
	if (!has_seconds) {
		return false;
	}
	auto zone = text.substr(pos);
	if (zone == "Z") {
		result.has_offset = true;
		return true;
	}
	if (TryParseFixedOffset(zone, result.offset) && (zone[0] == '+' || zone[0] == '-')) {
		result.has_offset = true;
		return true;
	}
	return false;
}

//===--------------------------------------------------------------------===//
// Bracket expansion
//===--------------------------------------------------------------------===//
struct ExpandedText {
	string text;
	//! A time zone attached to an element of a time list, which wins over all others
	string time_zone;
};

vector<int64_t> ParseBracketValues(const string &content) {
	if (Trim(content).empty()) {
		Fail("Empty bracket expansion");
	}
	vector<int64_t> values;
	idx_t pos = 0;
	auto skip_spaces = [&]() {
		while (pos < content.size() && IsSpace(content[pos])) {
			pos++;
		}
	};
	auto parse_number = [&](int64_t &value) {
		auto begin = pos;
		value = 0;
		while (pos < content.size() && IsDigit(content[pos])) {
			value = value * 10 + (content[pos++] - '0');
			if (value > MAX_NUMBER) {
				return false;
			}
		}
		return pos > begin;
	};
	while (true) {
		skip_spaces();
		if (pos >= content.size()) {
			break;
		}
		int64_t start;
		if (!parse_number(start)) {
			Fail("Expected number in bracket expansion");
		}
		skip_spaces();
		auto end = start;
		if (content.compare(pos, 2, "..") == 0) {
			pos += 2;
			skip_spaces();
			if (!parse_number(end)) {
				Fail("Expected number after '..'");
			}
			if (end < start) {
				Fail(StringUtil::Format("Range must be ascending: %lld..%lld", start, end));
			}
			skip_spaces();
		}
		if (values.size() + idx_t(end - start) >= MAX_INTERVALS) {
			Fail(StringUtil::Format("Bracket expansion produces too many intervals (max %llu)", MAX_INTERVALS));
		}
		for (auto value = start; value <= end; value++) {
			values.push_back(value);
		}
		if (pos >= content.size()) {
			break;
		}
		if (content[pos] != ',') {
			Fail("Expected ',' or end of bracket");
		}
		pos++;
	}
	return values;
}

//! Zero padding of a bracket value, from the text before the bracket
idx_t PaddingWidth(const string &before) {
	if (before.empty() || before.back() == '.') {
		return 0;
	}
	// ISO weekday: Www-
	auto size = before.size();
	if (size >= 4 && before[size - 1] == '-' && IsDigit(before[size - 2]) && IsDigit(before[size - 3]) &&
	    before[size - 4] == 'W') {
		return 1;
	}
	return 2;
}

void ExpandBrackets(const string &done, const string &rest, const string &time_zone, vector<ExpandedText> &result) {
	auto open = rest.find('[');
	if (open == string::npos) {
		if (result.size() >= MAX_INTERVALS) {
			Fail(StringUtil::Format("Bracket expansion produces too many intervals (max %llu)", MAX_INTERVALS));
		}
		result.push_back({done + rest, time_zone});
		return;
	}
	auto close = rest.find(']', open);
	if (close == string::npos) {
		Fail("Unclosed '[' in interval");
	}
	auto content = rest.substr(open + 1, close - open - 1);
	auto before = done + rest.substr(0, open);
	auto after = rest.substr(close + 1);
	if (content.find(':') != string::npos) {
		if (content.find('[') != string::npos) {
			Fail("Nested brackets not supported in time list. Use separate expansions: T[09:00,09:30] instead of "
			     "T[09:[00,30]]");
		}
		for (auto &element : StringUtil::Split(content, ',')) {
			auto time = Trim(element);
			if (time.empty()) {
				Fail("Empty element in time list");
			}
			auto element_zone = time_zone;
			auto at = time.find('@');
			if (at != string::npos) {
				element_zone = Trim(time.substr(at + 1));
				if (element_zone.empty()) {
					Fail("Invalid timezone: ");
				}
				time = Trim(time.substr(0, at));
			}
			ExpandBrackets(before + time, after, element_zone, result);
		}
		return;
	}
	if (content.find('[') != string::npos) {
		Fail("Expected number in bracket expansion");
	}
	auto values = ParseBracketValues(content);
	if (values.empty()) {
		Fail("Empty bracket expansion");
	}
	if (!before.empty() && before.back() == 'T') {
		for (auto value : values) {
			if (value >= 100) {
				Fail("Military time format not supported in bracket expansion. Use colons: [09:00,14:30] instead "
				     "of [0900,1430]");
			}
		}
	}
	auto width = PaddingWidth(before);
	for (auto value : values) {
		auto digits = std::to_string(value);
		if (digits.size() < width) {
			digits = string(width - digits.size(), '0') + digits;
		}
		ExpandBrackets(before + digits, after, time_zone, result);
	}
}

//===--------------------------------------------------------------------===//
// The expander
//===--------------------------------------------------------------------===//
//! An inclusive wall-clock interval waiting for its time zone conversion
struct LocalInterval {
	int64_t lo;
	int64_t hi;
	string time_zone;
	//! Already a UTC instant (raw epoch value or explicit offset)
	bool absolute;
};

class TickExpander {
public:
	explicit TickExpander(TickEnvironment &env_p) : env(env_p) {
		vector<int64_t> now {env.now};
		env.time_zones.UtcToLocal(env.time_zone, now);
		now_local = now[0];
	}

	vector<TickInterval> Expand(const string &pattern) {
		ParseTopLevel(Trim(pattern));
		return Finalize();
	}

private:
	void ParseTopLevel(string text) {
		if (text.empty()) {
			Fail("Invalid date: " + text);
		}
		// the day filter runs from '#' to the first ';'
		auto semicolon = FindOutsideBrackets(text, ';');
		auto hash = FindOutsideBrackets(text, '#', 0, semicolon);
		if (hash != string::npos) {
			auto filter_end = semicolon == string::npos ? text.size() : semicolon;
			global_filter = ParseDayFilter(text.substr(hash + 1, filter_end - hash - 1));
			text.erase(hash, filter_end - hash);
		}
		semicolon = FindOutsideBrackets(text, ';');
		if (semicolon != string::npos) {
			ParseDurationSpec(text.substr(semicolon + 1), text);
			text = text.substr(0, semicolon);
		}
		auto at = FindLastOutsideBrackets(text, '@');
		if (at != string::npos) {
			global_time_zone = Trim(text.substr(at + 1));
			if (global_time_zone.empty()) {
				Fail("Invalid timezone: ");
			}
			text = Trim(text.substr(0, at));
		}
		if (text.empty()) {
			Fail("Invalid date: " + text);
		}

		if (text[0] == '[') {
			auto close = MatchingBracket(text, 0);
			if (close == string::npos) {
				Fail("Unclosed '[' in date list");
			}
			auto suffix = text.substr(close + 1);
			if (suffix.empty() || suffix[0] == 'T' || IsSpace(suffix[0])) {
				ParseDateList(text.substr(1, close - 1), Trim(suffix));
				return;
			}
			// a bracket followed by more date text is a field expansion, e.g. [2018,2019]-01-10
			ProcessStatic(text, string(), global_filter, global_time_zone);
			return;
		}
		if (text[0] == '$') {
			auto time_start = FindTimeStart(text, false);
			auto element = time_start == string::npos ? text : text.substr(0, time_start);
			auto suffix = time_start == string::npos ? string() : text.substr(time_start);
			if (FindOutsideBrackets(element, ',') != string::npos) {
				Fail("comma-separated date lists require brackets, e.g., [$now,$tomorrow]");
			}
			ProcessElement(element, suffix);
			return;
		}
		ProcessStatic(text, string(), global_filter, global_time_zone);
	}

	void ParseDurationSpec(const string &spec, const string &text) {
		auto parts = StringUtil::Split(spec, ';');
		if (spec.empty()) {
			Fail("Expected number before unit ''");
		}
		if (parts.size() == 2) {
			Fail("Invalid interval format: " + text);
		}
		if (parts.size() > 3) {
			Fail("Invalid interval format (too many semicolons): " + text);
		}
		duration = ParseDuration(parts[0]);
		has_duration = true;
		if (parts.size() == 3) {
			ParseRepeat(parts[1], parts[2]);
		}
	}

	//! Legacy repeating intervals: date;duration;period;count
	void ParseRepeat(const string &period_text, const string &count_text) {
		idx_t pos = 0;
		int64_t sign = 1;
		if (pos < period_text.size() && period_text[pos] == '-') {
			sign = -1;
			pos++;
		}
		int64_t amount;
		if (!TryParseNumber(period_text, pos, amount)) {
			Fail("Period not a number");
		}
		if (pos + 1 != period_text.size()) {
			Fail("Period not a number");
		}
		auto unit = period_text[pos];
		if (unit != 'y' && unit != 'M' && unit != 'd' && unit != 'h' && unit != 'm' && unit != 's') {
			Fail(StringUtil::Format("Unknown period: %c at %llu", unit, pos));
		}
		pos = 0;
		int64_t count;
		if (!TryParseNumber(count_text, pos, count) || pos != count_text.size()) {
			if (!count_text.empty() && count_text[0] == '-') {
				Fail("Count must be positive");
			}
			Fail("Count not a number");
		}
		if (count <= 0) {
			Fail("Count must be positive");
		}
		repeat_period = {sign * amount, unit};
		repeat_count = count;
	}

	void ParseDateList(const string &content, const string &suffix) {
		if (Trim(content).empty()) {
			Fail("Empty date list");
		}
		for (auto &element : SplitOutsideBrackets(content, ',')) {
			auto trimmed = Trim(element);
			if (trimmed.empty()) {
				Fail("Empty element in date list");
			}
			if (trimmed[0] == '[') {
				Fail("Expected ',' or end of bracket");
			}
			ProcessElement(trimmed, suffix);
		}
	}

	//! A date list element (or a bare variable expression), which may carry its own @tz and #filter
	void ProcessElement(string element, const string &suffix) {
		auto filter = global_filter;
		auto time_zone = global_time_zone;
		auto hash = FindOutsideBrackets(element, '#');
		if (hash != string::npos) {
			filter = ParseDayFilter(element.substr(hash + 1));
			element = Trim(element.substr(0, hash));
		}
		auto at = FindLastOutsideBrackets(element, '@');
		if (at != string::npos) {
			time_zone = Trim(element.substr(at + 1));
			if (time_zone.empty()) {
				Fail("Invalid timezone: ");
			}
			element = Trim(element.substr(0, at));
		}
		if (element.empty() || element[0] != '$') {
			ProcessStatic(element, suffix, filter, time_zone);
			return;
		}
		auto range = element.find("..");
		if (range == string::npos) {
			ProcessVariable(EvaluateVariable(element), suffix, filter, time_zone);
			return;
		}
		auto end_text = Trim(element.substr(range + 2));
		if (end_text.empty()) {
			Fail("Empty end expression in date range");
		}
		ProcessRange(EvaluateVariable(element.substr(0, range)), EvaluateVariable(end_text), end_text, suffix,
		             filter, time_zone);
	}

	void ProcessVariable(int64_t value, const string &suffix, const DayFilter &filter, const string &time_zone) {
		if (value == FloorDay(value)) {
			// a midnight value is a whole day and takes the suffix
			ProcessStatic(FormatDay(DateOf(value)), suffix, filter, time_zone);
			return;
		}
		// any other value is a point: a time suffix is ignored, the duration still applies
		AddWithDuration(value, value, filter, time_zone, false);
	}

	void ProcessRange(int64_t start, int64_t end, const string &end_text, const string &suffix,
	                  const DayFilter &filter, const string &time_zone) {
		if (start > end) {
			Fail("Invalid date range: start is after end");
		}
		if (start != FloorDay(start) && end != FloorDay(end)) {
			// both ends have a time of day: one continuous interval
			if (filter.Matches(DateOf(start))) {
				AddLocal(start, end, time_zone, false);
			}
			return;
		}
		auto lower = StringUtil::Lower(end_text);
		bool business_days = lower.size() >= 2 && lower.compare(lower.size() - 2, 2, "bd") == 0;
		auto last = DateOf(end);
		for (auto day = DateOf(start); day <= last; day = date_t(day.days + 1)) {
			if (business_days && IsWeekend(day)) {
				continue;
			}
			ProcessStatic(FormatDay(day), suffix, filter, time_zone);
		}
	}

	//! `$var [(+|-) N unit]`, as a wall-clock time in the session time zone
	int64_t EvaluateVariable(const string &expression) {
		auto text = Trim(expression);
		idx_t pos = 1;
		while (pos < text.size() && StringUtil::CharacterIsAlpha(text[pos])) {
			pos++;
		}
		auto name = StringUtil::Lower(text.substr(1, pos - 1));
		int64_t value;
		auto today = FloorDay(now_local);
		if (name == "now") {
			value = now_local;
		} else if (name == "today") {
			value = today;
		} else if (name == "yesterday") {
			value = today - Interval::MICROS_PER_DAY;
		} else if (name == "tomorrow") {
			value = today + Interval::MICROS_PER_DAY;
		} else {
			Fail("Unknown date variable: " + text.substr(1, MaxValue<idx_t>(pos, 2) - 1));
		}
		auto skip_spaces = [&]() {
			while (pos < text.size() && text[pos] == ' ') {
				pos++;
			}
		};
		skip_spaces();
		if (pos == text.size()) {
			return value;
		}
		if (text[pos] != '+' && text[pos] != '-') {
			if (text[pos] == '.' || StringUtil::CharacterIsAlpha(text[pos])) {
				Fail("Unknown date variable: " + text.substr(1));
			}
			Fail("Expected '+' or '-' operator");
		}
		int64_t sign = text[pos++] == '-' ? -1 : 1;
		skip_spaces();
		if (pos == text.size() || !IsDigit(text[pos])) {
			Fail("Expected number after operator");
		}
		int64_t amount;
		if (!TryParseNumber(text, pos, amount)) {
			Fail("Invalid number in date expression");
		}
		skip_spaces();
		if (pos == text.size()) {
			Fail("Expected time unit after number");
		}
		if (pos + 1 < text.size() && tolower(text[pos]) == 'b' && tolower(text[pos + 1]) == 'd') {
			pos += 2;
			value = AddBusinessDays(value, sign * amount);
		} else {
			auto unit = text[pos++];
			if (unit == 'D') {
				unit = 'd';
			}
			if (!IsDurationUnit(unit)) {
				if (unit == 'b' || unit == 'B') {
					Fail("Unexpected characters after unit");
				}
				Fail(StringUtil::Format("Invalid time unit: %c", unit));
			}
			value = AddPart(value, {sign * amount, unit});
		}
		skip_spaces();
		if (pos != text.size()) {
			Fail("Unexpected characters after unit");
		}
		return value;
	}

	//! Steps day by day, counting only Monday to Friday; the start day itself is never adjusted
	static int64_t AddBusinessDays(int64_t value, int64_t count) {
		auto step = count < 0 ? -1 : 1;
		auto remaining = count < 0 ? -count : count;
		while (remaining > 0) {
			value += step * Interval::MICROS_PER_DAY;
			if (!IsWeekend(DateOf(value))) {
				remaining--;
			}
		}
		return value;
	}

	//! A date (with brackets, a time, or both) and its shared time suffix
	void ProcessStatic(const string &element, const string &suffix, const DayFilter &filter,
	                   const string &time_zone) {
		auto text = FindTimeStart(element) == string::npos ? element + suffix : element;
		vector<ExpandedText> expanded;
		ExpandBrackets(string(), text, string(), expanded);
		for (auto &entry : expanded) {
			ProcessConcrete(entry.text, filter, entry.time_zone.empty() ? time_zone : entry.time_zone);
		}
	}

	void ProcessConcrete(const string &text, const DayFilter &filter, const string &time_zone) {
		// a raw epoch value in microseconds is a point
		if (text.size() > 4 && std::all_of(text.begin(), text.end(), IsDigit)) {
			int64_t value = 0;
			for (auto c : text) {
				value = value * 10 + (c - '0');
				if (value > NumericLimits<int64_t>::Maximum() / 10) {
					Fail("Invalid date: " + text);
				}
			}
			AddWithDuration(value, value, filter, time_zone, true);
			return;
		}
		auto time_start = FindTimeStart(text);
		auto date_text = time_start == string::npos ? text : text.substr(0, time_start);
		ParsedDate date;
		if (!TryParseDatePart(date_text, date)) {
			Fail("Invalid date: " + text);
		}
		if (time_start == string::npos) {
			if (date.precision == DatePrecision::DAY) {
				AddWithDuration(DayStart(date.first), DayStart(date.first) + Interval::MICROS_PER_DAY - 1, filter,
				                time_zone, false);
			} else if (has_duration) {
				// a month, year or week with a duration expands to every day
				for (auto day = date.first; day <= date.last; day = date_t(day.days + 1)) {
					AddWithDuration(DayStart(day), DayStart(day) + Interval::MICROS_PER_DAY - 1, filter, time_zone,
					                false);
				}
			} else {
				// a multi-day interval: the day filter selects whole days
				for (auto day = date.first; day <= date.last; day = date_t(day.days + 1)) {
					if (filter.Matches(day)) {
						AddLocal(DayStart(day), DayStart(day) + Interval::MICROS_PER_DAY - 1, time_zone, false);
					}
				}
			}
			return;
		}
		auto separator = text[time_start];
		ParsedTime time;
		if (!TryParseTimePart(text.substr(time_start + 1), time)) {
			Fail("Invalid date: " + text);
		}
		bool is_week_day = date_text.find('W') != string::npos;
		if (separator == ' ' && !is_week_day) {
			// a space separator makes the time a point
			time.span = 0;
		}
		// a month, year or week with a time expands to every day
		for (auto day = date.first; day <= date.last; day = date_t(day.days + 1)) {
			auto lo = DayStart(day) + time.micros;
			if (time.has_offset) {
				lo -= time.offset;
				AddWithDuration(lo, lo, filter, time_zone, true);
			} else {
				AddWithDuration(lo, lo + time.span, filter, time_zone, false);
			}
		}
	}

	//! Applies the duration (replacing the precision span) and the day filter, which checks lo's day
	void AddWithDuration(int64_t lo, int64_t hi, const DayFilter &filter, const string &time_zone, bool absolute) {
		if (!has_duration) {
			if (filter.Matches(DateOf(lo))) {
				AddLocal(lo, hi, time_zone, absolute);
			}
			return;
		}
		for (int64_t i = 0; i < repeat_count; i++) {
			auto anchor = i == 0 ? lo : AddPart(lo, {repeat_period.amount * i, repeat_period.unit});
			auto end = AddParts(anchor, duration);
			auto interval_lo = end >= anchor ? anchor : end;
			auto interval_hi = end >= anchor ? end - 1 : anchor - 1;
			if (filter.Matches(DateOf(interval_lo))) {
				AddLocal(interval_lo, interval_hi, time_zone, absolute);
			}
		}
	}

	void AddLocal(int64_t lo, int64_t hi, const string &time_zone, bool absolute) {
		if (intervals.size() >= MAX_INTERVALS) {
			Fail(StringUtil::Format("Bracket expansion produces too many intervals (max %llu)", MAX_INTERVALS));
		}
		intervals.push_back({lo, hi, time_zone, absolute});
	}

	//! Converts every interval to UTC, one time zone at a time, and returns them half-open and merged
	vector<TickInterval> Finalize() {
		vector<TickInterval> result;
		result.reserve(intervals.size());
		case_insensitive_map_t<vector<idx_t>> by_zone;
		for (idx_t i = 0; i < intervals.size(); i++) {
			auto &interval = intervals[i];
			if (interval.hi < interval.lo) {
				// e.g. a zero duration: matches nothing
				continue;
			}
			if (interval.absolute) {
				result.push_back({interval.lo, interval.hi + 1});
				continue;
			}
			by_zone[interval.time_zone.empty() ? env.time_zone : interval.time_zone].push_back(i);
		}
		for (auto &entry : by_zone) {
			vector<int64_t> bounds;
			bounds.reserve(entry.second.size() * 2);
			for (auto i : entry.second) {
				bounds.push_back(intervals[i].lo);
				bounds.push_back(intervals[i].hi);
			}
			env.time_zones.LocalToUtc(entry.first, bounds);
			for (idx_t i = 0; i < entry.second.size(); i++) {
				if (bounds[2 * i + 1] >= bounds[2 * i]) {
					result.push_back({bounds[2 * i], bounds[2 * i + 1] + 1});
				}
			}
		}
		MergeTickIntervals(result);
		return result;
	}

	TickEnvironment &env;
	int64_t now_local;
	DayFilter global_filter;
	string global_time_zone;
	vector<DurationPart> duration;
	bool has_duration = false;
	DurationPart repeat_period {0, 'd'};
	int64_t repeat_count = 1;
	vector<LocalInterval> intervals;
};

} // namespace

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
	TickExpander expander(env);
	return expander.Expand(pattern);
}

} // namespace duckdb
