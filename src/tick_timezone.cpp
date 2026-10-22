#include "tick_timezone.hpp"

#include "duckdb/common/exception.hpp"
#include "duckdb/common/types/data_chunk.hpp"
#include "duckdb/common/types/timestamp.hpp"
#include "duckdb/common/vector/vector_writer.hpp"
#include "duckdb/execution/expression_executor.hpp"
#include "duckdb/function/function_binder.hpp"
#include "duckdb/main/extension_helper.hpp"
#include "duckdb/planner/expression/bound_constant_expression.hpp"
#include "duckdb/planner/expression/bound_reference_expression.hpp"

namespace duckdb {

static bool TryParseTwoDigits(const string &text, idx_t pos, int64_t &result) {
	if (pos + 2 > text.size() || !StringUtil::CharacterIsDigit(text[pos]) ||
	    !StringUtil::CharacterIsDigit(text[pos + 1])) {
		return false;
	}
	result = (text[pos] - '0') * 10 + (text[pos + 1] - '0');
	return true;
}

bool TryParseFixedOffset(const string &tz, int64_t &offset_micros) {
	if (StringUtil::CIEquals(tz, "UTC") || StringUtil::CIEquals(tz, "GMT") || StringUtil::CIEquals(tz, "Z")) {
		offset_micros = 0;
		return true;
	}
	// +HH, +HHMM, +HH:MM (and the same with '-')
	if (tz.empty() || (tz[0] != '+' && tz[0] != '-')) {
		return false;
	}
	int64_t hours, minutes = 0;
	if (!TryParseTwoDigits(tz, 1, hours)) {
		return false;
	}
	if (tz.size() == 5) {
		if (!TryParseTwoDigits(tz, 3, minutes)) {
			return false;
		}
	} else if (tz.size() == 6) {
		if (tz[3] != ':' || !TryParseTwoDigits(tz, 4, minutes)) {
			return false;
		}
	} else if (tz.size() != 3) {
		return false;
	}
	if (hours > 18 || minutes > 59) {
		return false;
	}
	offset_micros = (hours * Interval::MINS_PER_HOUR + minutes) * Interval::MICROS_PER_MINUTE;
	if (tz[0] == '-') {
		offset_micros = -offset_micros;
	}
	return true;
}

class ICUTickTimeZones final : public TickTimeZones {
public:
	explicit ICUTickTimeZones(ClientContext &context_p) : context(context_p) {
	}

	void LocalToUtc(const string &tz, vector<int64_t> &values) override {
		int64_t offset;
		if (TryParseFixedOffset(tz, offset)) {
			Shift(values, -offset);
			return;
		}
		Convert(tz, LogicalType::TIMESTAMP, values);
	}

	void UtcToLocal(const string &tz, vector<int64_t> &values) override {
		int64_t offset;
		if (TryParseFixedOffset(tz, offset)) {
			Shift(values, offset);
			return;
		}
		Convert(tz, LogicalType::TIMESTAMP_TZ, values);
	}

private:
	static void Shift(vector<int64_t> &values, int64_t offset) {
		for (auto &value : values) {
			if (timestamp_t(value).IsFinite()) {
				value += offset;
			}
		}
	}

	//! Evaluates ICU's timezone(tz, value): TIMESTAMP -> TIMESTAMPTZ (local to UTC) or the reverse
	void Convert(const string &tz, const LogicalType &input_type, vector<int64_t> &values) {
		if (values.empty()) {
			return;
		}
		ExtensionHelper::AutoLoadExtension(context, "icu");
		vector<unique_ptr<Expression>> children;
		children.push_back(make_uniq<BoundConstantExpression>(Value(tz)));
		children.push_back(make_uniq<BoundReferenceExpression>(input_type, 0ULL));
		unique_ptr<Expression> expr;
		try {
			expr = FunctionBinder(context).BindScalarFunction(Identifier::DefaultSchema(), "timezone",
			                                                  std::move(children));
			ExpressionExecutor executor(context, *expr);
			DataChunk input;
			input.Initialize(context, {input_type});
			for (idx_t offset = 0; offset < values.size(); offset += STANDARD_VECTOR_SIZE) {
				auto count = MinValue<idx_t>(STANDARD_VECTOR_SIZE, values.size() - offset);
				input.Reset();
				{
					auto writer = FlatVector::Writer<timestamp_t>(input.data[0], count);
					for (idx_t i = 0; i < count; i++) {
						writer.WriteValue(timestamp_t(values[offset + i]));
					}
				}
				input.SetCardinality(count);
				Vector result(expr->GetReturnType(), count);
				executor.ExecuteExpression(input, result);
				idx_t i = 0;
				for (auto entry : result.Values<timestamp_t>()) {
					values[offset + i++] = entry.GetValue().value;
				}
			}
		} catch (std::exception &ex) {
			ErrorData error(ex);
			if (error.Type() == ExceptionType::INVALID_INPUT || error.Type() == ExceptionType::NOT_IMPLEMENTED) {
				throw InvalidInputException("Invalid timezone: %s", tz);
			}
			throw;
		}
	}

	ClientContext &context;
};

unique_ptr<TickTimeZones> CreateTickTimeZones(ClientContext &context) {
	return make_uniq<ICUTickTimeZones>(context);
}

} // namespace duckdb
