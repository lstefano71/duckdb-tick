# QuestDB TICK interval syntax: semantics taken from the source

Source: `questdb/questdb` at commit `8da4e347f96563dc4e9d13916e00177e2af708f9`.

Files:
- `core/src/main/java/io/questdb/griffin/model/IntervalUtils.java` (cited as **IU:line**)
- `DateExpressionEvaluator.java` (**DEE**)
- `DateVariableExpr.java` / `CompiledTickExpression.java`: the compiled ("dynamic") path for expressions that contain `$vars`. Tests assert that it gives the same output as the static path.
- `core/src/main/java/io/questdb/cairo/MicrosTimestampDriver.java` (**MTD**): `parseInterval`, `parseFloor`, and ISO week parsing.
- `std/datetime/millitime/Dates.java` (`parseOffset`) and `std/datetime/CommonUtils.java`.

Tests:
- `core/src/test/java/io/questdb/test/griffin/model/TickExprTest.java` (**T:**`testName`). It is parameterised for MICRO and NANO timestamps; only the MICRO output is shown here.
- `TickCalendarTest.java` (**TC:**) and `IntervalUtilsTest.java` (**IUT:**).

Conventions:
- Every interval is `[lo, hi]` with **both bounds inclusive**, in UTC microseconds, as QuestDB reports them.
- "now" is the value passed to `parseTickExpr(..., nowTimestamp)`. Most tests use **now = `2026-01-22T10:30:00.000000Z`** (a Thursday). Other values are given per row.
- `x.999999` stands for `x.999999Z`. The `Z` is dropped in the tables.
- **[CODE]** marks a rule I derived from reading the source because no test covers it. **[UNDETERMINED]** marks something I could not settle from the source.

## 0. Processing pipeline (IU:782-1213 `parseTickExpr`)

1. Skip leading ASCII whitespace. Trailing whitespace is *not* trimmed at the top level: `'2024-01-15 '` gives `Invalid date` [CODE].
2. Find `#` outside brackets and before the first `;`. The text from `#` up to `;` (or the end) is the day filter, or an exchange calendar if a `TickCalendarService` recognises the name. That text is **removed** from the string before the rest is parsed (IU:806-841).
3. Dispatch on the first character:
   - `[` and `isDateList` is true (the matching `]` is followed by end of string, `T`, `;`, `@` or whitespace; IU:4137) → **date list** (`expandDateList`, IU:2874).
   - `$` → **bare variable**. The expression runs until the first `@`, `;`, `,` or `T`+digit. It is wrapped as `[expr]suffix` and handled as a date list (IU:900-970). A `,` at that point raises an error.
   - A digit, where the date part before the first `T`+digit/`T[` or `;` has fewer than 2 `-` (a year, month or ISO week without a day) → wrapped as `[date]suffix` and handled as a date list (IU:976-1022).
   - Anything else → find `;` and `@` outside brackets, then recursive bracket expansion (IU:1024-1213).
4. For each element, steps run in this order. The order matters:
   1. Parse the date and time.
   2. Apply the duration (local wall-clock arithmetic).
   3. Apply the day filter (on the **local** date).
   4. Convert from the time zone to UTC.
   5. Apply the exchange calendar (in UTC).
   6. Sort and merge.

## 1. Bare dates, months and years; expanding months and years into days

Rule (MTD:1080-1255 `parseInterval`): an interval covers the whole precision unit, from its start to start + unit − 1µs.

- A **year** covers Jan 1 00:00 to Dec 31 23:59:59.999999.
- A **month** covers day 1 to the last day of the month (leap years handled).
- Field widths are fixed: year 4 digits, all other fields 2 digits. `2016-1` and `2016-02-3` are errors.
- Brackets with nothing after them change nothing: `[2024-01]` is still one month-long interval. In the bracket form, though, a 4-digit year in a field-expansion bracket such as `[2024,2025]` at end of string is parsed as a date list of years.

**Month or year with a time or duration suffix** (IU:3132-3193, IU:2659-2673, IU:3673-3762): if the element has no day (fewer than 2 `-`) and the suffix starts with `T`+digit, `T[`, or `;` (a duration), the element is expanded to **every day**:
- `YYYY-MM` is rewritten to `YYYY-MM-[01..lastDay]`.
- `YYYY` is rewritten to `YYYY-[01..12]`, and each month is then expanded to its days.

The suffix is then applied to each day. A day filter alone, without a time or duration, does **not** trigger this rewrite. Instead, `applyDayFilter` splits the multi-day interval into the matching days (see section 9).

| pattern | intervals | source |
|---|---|---|
| `2016` | `[2016-01-01T00:00:00.000000, 2016-12-31T23:59:59.999999]` | T:testParseShortInterval1 |
| `2016-03` | `[2016-03-01T00:00:00.000000, 2016-03-31T23:59:59.999999]` | T:testParseShortInterval2 |
| `2016-03-21` | `[2016-03-21T00:00:00.000000, 2016-03-21T23:59:59.999999]` | T:testParseShortInterval3 |
| `[2025-12-31]` | `[2025-12-31T00:00, 2025-12-31T23:59:59.999999]` | T:testDateListSingleDate |
| `2018-[5]` | `[2018-05-01T00:00, 2018-05-31T23:59:59.999999]` | T:testBracketExpansionZeroPaddingMonth |
| `[2024-02]T09:30` | 29 intervals: `[2024-02-DDT09:30:00, 2024-02-DDT09:30:59.999999]` for DD=01..29 | T:testDateListImpreciseDateWithTimeOverride |
| `2024-02T09:30` (bare) | same as `[2024-02]T09:30` | T:testDateListImpreciseBareExpression, testImpreciseBareTime |
| `2024-02;6h30m` | same as `[2024-02-[01..29]];6h30m` (29 intervals `[DDT00:00, DDT06:29:59.999999]`) | T:testImpreciseBareDuration, testDateListImpreciseMonthWithDurationOnly |
| `[2024]T09:30` | 366 intervals | T:testDateListImpreciseYearLevel |
| `[2023]T09:30` | 365 intervals | T:testDateListImpreciseYearLevelNonLeap |
| `[2100-02]T09:30` | 28 intervals | T:testDateListImpreciseCenturyNonLeapFebruary |
| `2024-[01..02]T09:30` | 60 intervals, Jan 1 09:30 to Feb 29 09:30 | T:testDateListImpreciseBracketExpansionTimestamps |
| `[2024-01-15, 2024-02]T09:30` | 1 + 29 = 30 intervals | T:testDateListImpreciseDateWithMixedPrecision |
| `[2024]#workday;6h30m` | 262 intervals (every 2024 workday, 00:00 to 06:29:59.999999) | T:testDateListImpreciseYearLevelWithDayFilter |
| `2026-01;-3d` | `[2025-12-29T00:00, 2026-01-30T23:59:59.999999]` (per day `[d-3d, d-1µs]`, then merged) | T:testBracketExpansionNegativeDurationSimpleMonth |
| `[2024-13]T09:30`, `[2024-00]T09:30`, `2024-13T09:30` | error `Invalid date` | T:testDateListImpreciseInvalidMonth13/Zero, testDateListImpreciseBareInvalidMonth |

## 2. Partial-precision times (no duration)

Rule: **yes**, `[lo, lo + precision − 1µs]`, inclusive (MTD:1100-1207).

- Hour precision → `+59:59.999999`. Minute → `+59.999999`. Second → `+.999999`.
- A fraction of n digits (1 ≤ n ≤ 6) → `+10^(6−n) − 1` µs. For example, `.1` covers `.100000` to `.199999`.
- With more than 6 fractional digits (up to 9 are accepted), the micro driver truncates the extra digits and returns a single point.
- A time is **only valid after a date**. A pattern like `'T09'` on its own gives `Invalid date` [CODE: `parseInterval` needs ≥4 leading digits].
- `T` must be upper case: `[2026-01-01t09:30]` gives `Invalid date` (T:testDateListErrorLowercaseT).

**Point intervals** (`lo == hi`) [MTD `parseInterval` throws, so the code falls back to `parseFloor`, IU:4347-4362]:
- a space instead of `T`;
- a trailing `Z` or a numeric offset on a timestamp of second or finer precision;
- a raw epoch integer.

| pattern | intervals | source |
|---|---|---|
| `2016-03-21T10` | `[10:00:00.000000, 10:59:59.999999]` | T:testParseShortInterval4 |
| `2016-03-21T10:30` | `[10:30:00.000000, 10:30:59.999999]` | T:testParseShortInterval5 |
| `2016-03-21T10:30:40` | `[10:30:40.000000, 10:30:40.999999]` | T:testParseShortInterval6 |
| `2016-03-21T10:30:40.100` | `[10:30:40.100000, 10:30:40.100999]` | T:testParseShortInterval8 |
| `2016-03-21T10:30:40.28` | `[10:30:40.280000, 10:30:40.289999]` | T:testParseShortInterval8 |
| `2016-03-21T10:30:40.12345678` (micro) | `[10:30:40.123456, 10:30:40.123456]` | T:testParseShortInterval9 |
| `2016-03-21T10:30:40.100Z` | `[10:30:40.100000, 10:30:40.100000]` (point) | T:testParseShortInterval7 |
| `2018-01-[10,15] 10:30` | points `[2018-01-10T10:30:00, same]`, `[2018-01-15T10:30:00, same]` | T:testBracketExpansionWithSpaceSeparator |
| `1234567890000000` | `[1234567890000000, 1234567890000000]` (raw epoch µs) | T:testBracketExpansionRawTimestamp |
| `2016-02-15T1`, `T31`, `X1`, `2016-03-21T10:3`, `T10:69`, `T10:31:61`, `T10:31:1`, `2016-02-30` | error | T:testParseShort*Err* |

## 3. Durations `;…`

Rule (IU:4450-4481 `parseRange`, IU:4729-4738):
- `anchor` = **lo** of the parsed element. The precision span is **replaced**; the duration does not extend from the end of the span.
- `endExclusive = anchor + duration`.
- If `endExclusive ≥ anchor`: `[anchor, endExclusive − 1µs]`.
- Otherwise (a negative duration): `[endExclusive, anchor − 1µs]`.

Format:
- One or more parts `[±]N<unit>`, for example `1h30m` or `-3d2h`. Each part's sign applies to that part only: `-3d2h` is −3d + 2h.
- `_` is allowed inside a number.
- Units are those of `TimestampDriver.add`: `y M w d h m s T u n` (IU:1427-1477). Month and year steps are calendar-aware and clamp to the end of the month.
- `bd` is not a duration unit [CODE].
- No whitespace is allowed: `$today; h` gives `Expected number before unit ' '`.
- `0d` gives an *inverted* interval (`hi = lo − 1µs`). It is kept, not dropped.
- The duration is applied **before** time-zone conversion, so it is local wall-clock arithmetic. Each bound is then converted to UTC on its own.
- **Exception:** with an exchange calendar (`#XNYS;1h`), the duration is added to the **hi** of each trading-session interval (IU:1699-1713; TC:testExchangeFilterWithDuration).

| pattern (now = 2026-01-22T10:30Z) | intervals | source |
|---|---|---|
| `2018-01-[10,15]T10:30;1h` | `[01-10T10:30:00, 01-10T11:29:59.999999]`, `[01-15T10:30:00, 01-15T11:29:59.999999]` | T:testBracketExpansionWithDuration |
| `2024-01-15T09:30;1h` | `[09:30:00, 10:29:59.999999]` (same rule) | derived from above + T:testDateVariableTodayWithDuration |
| `2024-01-15T10:00;1h30m` | `[10:00:00, 11:29:59.999999]` | T:testBracketExpansionWithMultiUnitDuration |
| `2024-01-15T10:00:00;2h15m30s` | `[10:00:00, 12:15:29.999999]` | T:…DurationComplex |
| `2024-01-15T00:00;1d12h` | `[01-15T00:00, 01-16T11:59:59.999999]` | T:…DurationDaysHours |
| `2024-01-15T10:00:00.000000;1_500T` | `[10:00:00, 10:00:01.499999]` | T:…UnderscoreNumber |
| `[2025-01-01,2025-01-05];1h` (= `2024-01-15;1h` form) | `[01-01T00:00, 01-01T00:59:59.999999]`, `[01-05T00:00, 01-05T00:59:59.999999]` | T:testDateListWithDurationOnly |
| `[2024-01-01#Mon];1d` | `[2024-01-01T00:00, 2024-01-01T23:59:59.999999]` | T:testDayFilterWithOneDayDuration |
| `[2024-01-01#Mon];3d` | `[2024-01-01T00:00, 2024-01-03T23:59:59.999999]` | T:testDayFilterWithMultiDayDuration |
| `[2024-01]T09:30#workday;6h30m` | 23 intervals `[DDT09:30, DDT15:59:59.999999]` for the Jan 2024 workdays | T:testDateListImpreciseDateWithTimeAndDayFilter |
| `[2024-01]#workday;6h30m` | 23 intervals `[DDT00:00, DDT06:29:59.999999]`; by the same rule `[2024-01]#workday;8h` gives `[DDT00:00, DDT07:59:59.999999]` for each workday | T:testDateListImpreciseDateWithDurationAndDayFilter |
| `$now - 1h;30m` | `[09:30:00, 09:59:59.999999]` | T:testDateVariableBareWithDuration |
| `2018-01-[10,15]T00:00;-1h` | `[01-09T23:00, 01-09T23:59:59.999999]`, `[01-14T23:00, 01-14T23:59:59.999999]` | T:testBracketExpansionNegativeDuration |
| `[2025-01-01]T09:30;-5m` | `[09:25:00, 09:29:59.999999]` | T:testDateListNegativeDuration |
| `2025-01-15T10:00:00;-2h` | `[08:00:00, 09:59:59.999999]` | T:testParseNegativeDuration |
| `2025-01-15;-3d2h` | `[2025-01-12T02:00, 2025-01-14T23:59:59.999999]` | T:testParseNegativeDurationMixedSigns |
| `2025-01-15;-3d-2h` | `[2025-01-11T22:00, 2025-01-14T23:59:59.999999]` | T:…MultiNegative |
| `2025-01-15;+3d` | `[2025-01-15T00:00, 2025-01-17T23:59:59.999999]` | T:testParsePositiveDurationExplicitSign |
| `2025-01-15T10:00:00;0d` | `[10:00:00, 09:59:59.999999]` (inverted) | T:testParseZeroDuration |
| `2026-01-27T15:00;-1M` | `[2025-12-27T15:00, 2026-01-27T14:59:59.999999]` | T:testParseNegativeDurationCalendarMonth |
| `2026-03-31;-1M` | `[2026-02-28T00:00, 2026-03-30T23:59:59.999999]` | T:…DayClamping |
| `2024-02-29;-1y` | `[2023-02-28T00:00, 2024-02-28T23:59:59.999999]` | T:…CalendarYear |
| `2024-01-15T08:00@+03:00;1h` | `[05:00:00, 05:59:59.999999]` | T:testTimezoneWithDurationSuffix |
| `2024-01-15T[09:00,10:30];2h` | `[09:00, 12:29:59.999999]` (overlaps merged) | T:testTimeListBracketOverlappingMerged |

The comments in some of these tests say the duration is "added to the end". The expected values show replacement from lo. Follow the expected values.

## 4. Ranges `a..b`

- **Only date-variable ranges exist.** A range is recognised only when the element starts with `$` (IU:2979-3008, IU:900). Both sides must be `$var [± N unit]` expressions, evaluated by DEE.
- Static dates or full timestamps are **not** accepted on either side:
  - `2024-01-01..2024-01-05` gives `Invalid date: …` [CODE: the text reaches `parseInterval` and `parseFloor`].
  - `[..$today]` gives `Invalid date` (T:testDateVariableRangeEmptyStartExpression).
  - A static end such as `$today..2024-02-01` gives `Unknown date variable: 2024` [CODE].
- Inside brackets, the numeric `[a..b]` form is field expansion (section 7), which is a different feature.
- The first `.` found before `@` or `#` is taken as the range operator (IU:4001). So `$today.$tomorrow` gives `Unknown date variable: tomorrow`, and `...` also gives `Unknown date variable`.
- Whitespace around `..` and inside the expressions is trimmed.
- If `start > end` (raw timestamps, before truncation) → `Invalid date range: start is after end`.

The behaviour then depends on precision (IU:3404-3450):

- **Both endpoints have a sub-day time** (neither falls exactly on midnight): one interval **`[start, end]`, inclusive, with hi = end exactly** (no `−1µs`, and no precision padding).
  - Only the `@tz` and `#filter` suffixes are applied. The day filter checks lo's local day.
  - **[CODE]** The `T…` and `;duration` suffixes are **ignored** in this branch: no code reads them. The compiled test `$now - 2h..$now;1h` only asserts that static and compiled output agree.
- **Otherwise** (at least one endpoint at midnight, e.g. `$today`, `$tomorrow`, `$now..$tomorrow`): both endpoints are truncated to the start of their day. The code then **iterates day by day** and emits each day as its own element, with the full shared suffix (`T`, time list, `@tz`, `;duration`) applied to each.
  - Adjacent full days are merged at the end, so a plain calendar-day range looks like one interval that runs to the end of the last day.
  - If the **end** expression ends in `bd` (case-insensitive), Saturdays and Sundays are skipped during iteration (IU:4114, IU:3457-3483).
  - A range whose end does not end in `bd` (e.g. `$today - 5bd..$today`) iterates over every calendar day.
- **Bare ranges behave exactly like bracketed ones.** The bare form is wrapped in `[...]`. Suffixes work in bare form as well (T:testDateVariableRangeWithHoursBareWithGlobalTimezone), even though the docs say brackets are required.
- The docs' claim that "a range creates a single continuous interval" holds only in the sub-day branch.
- Quirk [CODE]: `$now..$now + 1d` evaluated at exactly midnight takes the day-iteration branch.

| pattern (now = 2026-01-22T10:30Z, Thu) | intervals | source |
|---|---|---|
| `$today..$today+5d` (bare) | `[01-22T00:00, 01-27T23:59:59.999999]` | T:testDateVariableBareRange |
| `[$today-3d..$today]` (now 2026-01-02T10:30) | `[2025-12-30T00:00, 2026-01-02T23:59:59.999999]` (ends at end of today) | T:testDateVariableRangeSpanningYearBoundary |
| `[$today-5d..$today-2d]` | `[01-17T00:00, 01-20T23:59:59.999999]` | T:testDateVariableRangeBothNegativeArithmetic |
| `[$today..$today+5bd]` | `[01-22, 01-23T23:59:59.999999]`, `[01-26, 01-29T23:59:59.999999]` | T:testDateVariableRangeBusinessDays |
| `[$today+1bd..$today+5bd]` | `[01-23, 01-23T23:59:59.999999]`, `[01-26, 01-29T23:59:59.999999]` | T:…ArithmeticBothSides |
| `[$today..$today+3bd]` (now Sat 2026-01-24) | `[01-26, 01-28T23:59:59.999999]` | T:…BusinessDaysFromWeekend |
| `[$today..$today+0bd]` (now Sat 2026-01-24) | `[]` | T:…ZeroBusinessDaysFromWeekend |
| `[$now..$tomorrow]` | `[01-22T00:00, 01-23T23:59:59.999999]` (`$now` truncated) | T:testDateVariableRangeWithNowStripsTime |
| `[$now - 2h..$now]` | `[08:30:00.000000, 10:30:00.000000]` | T:testDateVariableRangeWithHours |
| `$now-10_000T..$now-9_900T` | `[10:29:50.000000, 10:29:50.100000]` | T:testDateVariableUnderscoreInNumberRange |
| `[$now - 2h..$now]@America/New_York` | `[13:30:00, 15:30:00]` | T:…WithHoursAndGlobalTimezone |
| `[$now - 2h..$now@+05:00]@America/New_York` | `[03:30:00, 05:30:00]` (the element tz wins) | T:…ElementTzPrecedesGlobal |
| `[$today..$today+3d]T09:00;1h` | 4 intervals `[DDT09:00, DDT09:59:59.999999]`, DD=22..25 | T:testDateVariableRangeWithTimeSuffix |
| `[$today..$today+2d]T[09:00,14:00]` | 6 minute intervals (3 days × 2 times) | T:…WithTimeList |
| `[$today..$today+1d]T09:[00,30]` | 4 minute intervals: 22nd and 23rd at 09:00 and 09:30 | T:…WithNumericExpansionBrackets |
| `[$today..$today+2d]@America/New_York;1h` | `[01-22T05:00, 05:59:59.999999]`, `[01-23T05:00, …]`, `[01-24T05:00, …]` (per day) | T:…WithTimezoneAndDuration |
| `[$today..$today+2d]@+05:00` | `[01-21T19:00, 01-24T18:59:59.999999]` | T:…WithPerElementTimezone |
| `[$today..$today+10d]#workday` | `[01-22, 01-23T23:59:59.999999]`, `[01-26, 01-30T23:59:59.999999]` | T:…WithDayFilter |
| `[$today-2d..$today, $today+10d..$today+12d]` | `[01-20, 01-22T23:59:59.999999]`, `[02-01, 02-03T23:59:59.999999]` | T:…MultipleRangesInList |
| `[$today..$today+300d]` | `[2026-01-22T00:00, 2026-11-18T23:59:59.999999]` | T:…LargeRangeTriggersIncrementalMerge |
| `[$today-5bd..$today-1bd]T09:30` (now Mon 2026-01-26) | **[CODE]** 5 intervals `[DDT09:30, DDT09:30:59.999999]` for Jan 19..23 (one per business day, not one continuous interval); same output in bare form | derived |
| `[$today+5d..$today]` | error `Invalid date range: start is after end` | T:testDateVariableRangeInvalidOrder |
| `[$today..]` | error `Empty end expression in date range` | T:…EmptyEndExpression |
| `[$today#workday..$tomorrow]` | error `Invalid day name: workday..$tomorrow` | T:…DayFilterInStartExpression |

## 5. Date variables

Definitions (DEE:268-317):
- `$now` = now (full precision).
- `$today` = `startOfDay(now)` in **UTC**.
- `$yesterday` = `$today − 1d`.
- `$tomorrow` = `$today + 1d`.

Rules:
- Variable names are **case-insensitive** (`$TODAY`, `$ToDay`, `$TOMORROW`).
- Grammar: `$var [ws] [(+|-) [ws] N [ws] unit]`. There is **exactly one term**.
  - [CODE] `$today - 1d + 2h` gives `Unexpected characters after unit` (DEE:171-175).
  - Only the space character counts as whitespace here.
  - `N` allows `_` between digits; `1__000`, a leading `_` and a trailing `_` are rejected with `Invalid number in date expression`.
  - Units: `y M w d D bd h m s T u n`. `d` and `D` are equivalent. `bd` is case-insensitive. All other units are case-sensitive (`M` is months, `m` is minutes).
  - `n` (nanoseconds) on the micro driver: **[UNDETERMINED]**, because `Micros.addPeriod` was not inspected.
- Month and year arithmetic clamps to the end of the month (DEE → `Micros.addPeriod`).
- How a variable becomes an interval (IU:3021-3047):
  - If the resolved value is exactly midnight, it is formatted as `YYYY-MM-DD`. That makes it a **full day** that takes the `T`, `;` and `@` suffixes.
  - Otherwise (e.g. `$now`, `$today + 3h`) it is formatted as a full µs timestamp with `Z`. That makes it a **point** `[t, t]`.
  - For such a point, a suffix `T…` is **ignored**, but `;duration` and `@tz` still apply.
- `@tz` treats the variable's UTC wall-clock value as **local time** in that zone: `[$now]@America/New_York` at 10:30Z gives 15:30Z.

| pattern | now | intervals | source |
|---|---|---|---|
| `[$today]` | 2026-01-22T10:30 | `[01-22T00:00, 01-22T23:59:59.999999]` | T:testDateVariableToday |
| `[$yesterday]` / `[$tomorrow]` | same | Jan 21 / Jan 23 full day | T:testDateVariableYesterday/Tomorrow |
| `[$now]`, `$now` | same | `[10:30:00.000000, 10:30:00.000000]` | T:testDateVariableNow, …NowBare |
| `$now - 2h` | same | `[08:30:00, 08:30:00]` | T:testDateVariableBareWithArithmetic |
| `[$now + 500T]` | same | `[10:30:00.500000, 10:30:00.500000]` | T:…ArithmeticMilliseconds |
| `[$now + 100u]` | same | `[10:30:00.000100, same]` | T:…ArithmeticMicroseconds |
| `[$today + 2w]` | same | Feb 5 full day | T:…ArithmeticWeeks |
| `[$today + 1M]` | 2026-01-31T10:30 | `[2026-02-28T00:00, 02-28T23:59:59.999999]` | T:…MonthsEndOfMonth |
| `[$today + 1y]` | 2024-02-29T10:30 | `[2025-02-28T00:00, …]` | T:…YearsLeapYear |
| `[ $today   +   3 d ]` | 2026-01-22 | Jan 25 full day | T:testDateVariableWithWhitespace |
| `[$today + 3D]` / `[$ToDay + 1BD]` | 2026-01-22 | Jan 25 / Jan 23 | T:testCompiledTickExprDynamicOffsetUppercaseD, testDateVariableCaseInsensitive |
| `[$now - 1h]T09:30` | 2026-01-22T10:30 | `[09:30:00, 09:30:00]` (the `T` suffix is ignored) | T:…ArithmeticHoursWithTimeSuffix |
| `$todayT09:30` | same | `[09:30:00, 09:30:59.999999]` | T:testDateVariableBareWithTimeSuffix |
| `$today@America/New_York` | same | `[01-22T05:00, 01-23T04:59:59.999999]` | T:testDateVariableBareWithTimezone |
| `[$now, 2026-01-15]T09:00` | same | `[01-15T09:00, 09:00:59.999999]`, `[01-22T10:30:00, 10:30:00]` | T:testDateVariableNowMixedWithStaticDate |
| `[$today, $yesterday, 2025-06-10]T18:30` | 2025-06-18T08:15 | Jun 10, 17, 18 at `[18:30, 18:30:59.999999]` (sorted) | T:testDateVariableMixedList |
| `$now,$tomorrow` | | error `comma-separated date lists require brackets, e.g., [$now,$tomorrow]` | T:…BareCommaListRejected |
| `[$today + $today]` / `[$today +]` | | error `Expected number after operator` | T:… |
| `[$today * 5d]` | | error `Expected '+' or '-' operator` | |
| `[$today + 5]` | | error `Expected time unit after number` | |
| `[$today + 5x]` | | error `Invalid time unit: x` | |
| `[$today + 5bx]`, `[$today + 3dabc]` | | error `Unexpected characters after unit` | |
| `[$today + 99999999999d]` | | error `Invalid number in date expression` | |
| `$nowT` | | error `Unknown date variable` | T:testCompiledTickExprTrailingT |

## 6. Business days (`bd`)

Rule (DEE:191-205): step one calendar day at a time in the sign's direction, and count only the days that land on Mon–Fri.
- The start day is never adjusted, so `+0bd` returns the start unchanged, even on a weekend.
- From a Saturday, `+1bd` gives Monday and `−1bd` gives Friday.
- In a `..` range, a `bd` end makes the iteration skip weekend days (section 4).

| pattern | now | result | source |
|---|---|---|---|
| `[$today + 1bd]` | Thu 2026-01-22 | Fri 01-23 | T:testDateVariableArithmeticBusinessDays |
| `[$today + 2bd]` | Thu 2026-01-22 | Mon 01-26 | same |
| `[$today - 1bd]` | Thu 2026-01-22 | Wed 01-21 | same |
| `[$today + 0bd]` | Thu 2026-01-22 | Thu 01-22 | same |
| `[$today + 1bd]` | Sat 2026-01-24 | Mon 01-26 | T:testDateVariableBusinessDaysFromWeekend |
| `[$today - 1bd]` | Sat 2026-01-24 | Fri 01-23 | same |
| `$today + 3bd` | Fri 2026-03-20 | Wed 03-25 | T:testDateVariableExprEvaluateBusinessDaysAcrossWeekend |
| `[$today + 1_0bd]` | Thu 2026-01-22 | Thu 02-05 | T:testDateVariableUnderscoreInNumberBusinessDays |
| `[$today + 2bd]T09:30;1h` | Wed 2025-04-09 | `[2025-04-11T09:30, 10:29:59.999999]` | T:testDateVariableBusinessDaysWithTimeSuffix |
| `$today - 5bd` | Sat 2026-01-24 / Mon 2026-01-26 | **[CODE]** Mon 01-19 in both cases | derived from the algorithm |
| `[$today+0bd]` alone | Sat | **[CODE]** Saturday full day | derived |

## 7. Bracket expansion

Rules (IU:2618-2854 `expandBracketsRecursive`):

- **What a bracket can contain:** comma-separated non-negative integers and ascending ranges `a..b`, with any whitespace around values, `..` and commas. A trailing comma is allowed (`[10,   ]`).
- **Which fields a bracket can replace:** any numeric field — year, month, day, hour, minute, second, fractional seconds, ISO week, ISO weekday. A pattern can mix several bracket groups, e.g. `[1]-[1]-[1]T[1]:[1]:[1].[1][1][1]`.
- **Expansion order:** groups expand left to right as a **Cartesian product**. Each value is substituted into the string, the result is parsed with the normal date rules, then everything is sorted and merged.
- **Zero padding** (IU:2553-2605) depends on the text already expanded *before* the bracket:
  - empty (the bracket comes first, e.g. the year) → no padding;
  - after `.` → no padding (the fraction keeps its digit count, so `.[100,200]` has millisecond precision);
  - after `T` or a space → 2 digits;
  - directly after `W` → 2 digits;
  - after `Www-` → 1 digit;
  - otherwise (month or day) → 2 digits.

  So `2024-01-[5..7]` and `2024-01-[05..07]` are identical, and `2025-01-[02..8]` works (TC:testWorkdayExpressionMatchesXnys). Values too wide for the field produce `Invalid date`.
- **Time list vs field expansion:** a bracket whose content contains `:` is a **time list** (IU:4172; `expandTimeListBracket` IU:3786).
  - Each comma-separated element is a full time string and may carry its own `@tz`.
  - Elements must not contain `[`.
  - The text after `]` (e.g. `:[00,30]` or `;1h`) is appended to each element and can expand further.
  - Mixed precision is allowed (`T[09:00,14]`).
  - The time-list bracket owns time-zone handling: a per-element tz wins, otherwise the global tz is used.
  - `T[]` (no `:`) is treated as an empty field expansion.
- **Military time:** a field bracket directly after `T` with a value ≥ 100 → error.
- **Leading `[`:** this is a date list if the matching `]` is followed by end of string, `T`, `;`, `@` or whitespace. Otherwise (followed by `-`, `:`, `.`, a digit…) it is field expansion: `[2018,2019]-01-10`.
- **Date lists** (IU:2874):
  - Elements are separated by `,` at depth 0 and trimmed.
  - Each element can contain field brackets, `$vars`, `$` ranges, a per-element `@tz` and a per-element `#filter` (written `date@tz#filter`).
  - The shared suffix after `]` (`T…`, `@tz`, `;dur`) is applied to every element.
  - An element that already has a `T`+digit time keeps that time and skips the suffix's time part. It still takes the duration and tz.
  - A per-element tz or filter overrides the global one.
  - `[[2025-01-01]]` is an error.
- **Limits** (DefaultCairoConfiguration values, as the test comments state):
  - At most 8 bracket groups (`Too many bracket groups (max 8)`).
  - Incremental merge every 256 intervals.
  - At most 1024 intervals after merging (`Bracket expansion produces too many intervals (max 1024)`).

| pattern | intervals | source |
|---|---|---|
| `2018-[01,06]-[10,15]` | days Jan 10, Jan 15, Jun 10, Jun 15 (4 full days) | T:testBracketExpansionCartesianProduct |
| `2018-01-[5,10..12,20]` | `[01-05]`, `[01-10, 01-12T23:59:59.999999]`, `[01-20]` (full days, merged) | T:testBracketExpansionMixed |
| `2018-01-[10 .. 12 ]` | `[01-10T00:00, 01-12T23:59:59.999999]` | T:…RangeWithWhitespace |
| `2018-01-[ 10 , 15 ]` | Jan 10 and Jan 15 | T:…WithWhitespace |
| `2018-01-[5]` | `[2018-01-05T00:00, 23:59:59.999999]` (padded) | T:…ZeroPaddingDay |
| `[2018,2019]-01-10` | 2018-01-10 and 2019-01-10 | T:testBracketExpansionYear |
| `[1970..2170]-01-01` | 201 intervals | T:…LargeRangeWithIncrementalMerge |
| `2018-01-10T[10,14]:30` | `[10:30, 10:30:59.999999]`, `[14:30, 14:30:59.999999]` | T:testBracketExpansionHourMinute |
| `2018-01-10T10:30:00.[100,200]` | `[.100000, .100999]`, `[.200000, .200999]` | T:…Microseconds |
| `2024-01-15T[09:00,18:00]` | `[09:00, 09:00:59.999999]`, `[18:00, 18:00:59.999999]` | T:testTimeListBracketWithoutDuration |
| `2024-01-15T[09:00,09:01]` | `[09:00:00, 09:01:59.999999]` (adjacent, merged) | T:testTimeListBracketAdjacentIntervals |
| `2024-01-15T[09,14:29]` | `[09:00, 09:59:59.999999]`, `[14:29, 14:29:59.999999]` | T:…MixedFormats2 |
| `2024-01-15T[09:00:30,14:30:45]` | second-precision intervals | T:…WithSeconds |
| `2024-01-15T[09:00,14:30]:[00,30]` | 4 second intervals: 09:00:00, 09:00:30, 14:30:00, 14:30:30 | T:…WithSuffixExpansion |
| `2024-01-15T[09:00,14:30]:[00,30];1h` | `[09:00:00, 10:00:29.999999]`, `[14:30:00, 15:30:29.999999]` | T:…SuffixBracketAndDuration |
| `[2024-01-15,2024-01-16]T[09:00,14:30]:[00,30]` | 8 second intervals | T:testDateListWithNestedTimeExpansion |
| `[2025-01-15,2025-01-20]T[09,14]:30;1h` | 4 intervals `[DDT09:30, 10:29:59.999999]`, `[DDT14:30, 15:29:59.999999]` | T:testDateListWithTimeExpansion |
| `[2025-12-31,2026-01-[03..05]]` | `[12-31]`, `[01-03, 01-05T23:59:59.999999]` | T:testDateListWithNestedExpansion |
| `[ 2025-01-01 , 2025-01-02 ]` | `[01-01T00:00, 01-02T23:59:59.999999]` | T:testDateListWithWhitespace |
| `[2026-01-01T09:30, 2026-01-02]T10:00;1h` | `[01-01T09:30, 10:29:59.999999]`, `[01-02T10:00, 10:59:59.999999]` | T:…TimeSuffixWithDuration |
| `[2026-01-01T09:30, 2026-01-02]T[10:00,14:00]` | 01-01 09:30; 01-02 10:00; 01-02 14:00 (minute intervals) | T:…TimeListSuffix |
| `2018-01-[15..10]` | error `Range must be ascending: 15..10` | |
| `2018-01-[]` | error `Empty bracket expansion` | |
| `2018-01-[10..]` | error `Expected number after '..'` | |
| `2018-01-[abc]`, `2018-01-[[10]]`, `2018-01-[99999999999]` | error `Expected number in bracket expansion` | |
| `2018-01-[10.5]`, `2018-01-[10;15];1h`, `[[2025-01-01]]` | error `Expected ',' or end of bracket` | |
| `2018-01-[10,15` | error `Unclosed '[' in interval` | |
| `2024-01-15T[0900,1430]` | error `Military time format not supported in bracket expansion. Use colons: [09:00,14:30] instead of [0900,1430]` | |
| `2024-01-15T[09:[00,30],14:30]` | error `Nested brackets not supported in time list. Use separate expansions: T[09:00,09:30] instead of T[09:[00,30]]` | |
| `2024-01-15T[09:00,,14:30];6h` | error `Empty element in time list` | |
| `[1000..3000]-01-01` | error `…too many intervals (max 1024)` | |

## 8. Time zones `@tz`

Accepted forms (IU:1760-1822, `Dates.parseOffset`):
- **Numeric offsets:**
  - `±HH`, `±HH:MM`, `±HHMM`, or bare `HH…` (sign optional);
  - `Z`/`z`;
  - `UTC`/`GMT`, optionally followed by a signed offset (`UTC+05:00`).
  - Hours must be ≤ 23 and minutes ≤ 59.
  - [CODE] Characters after a complete `HH:MM` are not checked, so they are silently ignored.
- Otherwise the text is a **named zone** looked up with `DateLocale.matchZone` (e.g. `Europe/London`, `America/New_York`, `UTC`, `GMT`).
  - **[UNDETERMINED]** The full list of names and abbreviations `matchZone` accepts.
- An empty or unknown zone gives `invalid timezone: <text>`.

Where `@` is recognised:
- It must be the last `@` outside brackets and before the first `;` (IU:4026).
- The order of suffixes must be `T…` → `@tz` → `#filter` → `;dur`. Writing `@` after `#` makes the tz part of the filter name.
- A per-element tz is written `[date@tz, …]` or `T[09:00@tz, …]`, and it overrides the global tz.

How conversion works:
- The pattern is read as local wall-clock time, and the duration is added in local time. Each of lo and hi is then converted with `toUTC`. The day filter is evaluated *before* conversion, on the local date.
- **Numeric offset:** subtract the offset.
- **DST gap:** if lo falls in the gap, both lo and hi move forward by the gap offset. If only hi falls in the gap, only hi is adjusted.
- **DST overlap:** the daylight (pre-transition) offset is used.

| pattern | intervals | source |
|---|---|---|
| `2024-01-15T08:00@+03:00` | `[05:00, 05:00:59.999999]` | T:testTimezoneSimpleNumericOffset |
| `2024-01-15T08:00@-12:00` / `@+14:00` | `[20:00, …]` / `[2024-01-14T18:00, …]` | T:testTimezoneExtreme* |
| `2024-01-15T08:00@UTC` / `@GMT` / `@+00:00` | `[08:00, 08:00:59.999999]` | |
| `2024-07-15T08:00@Europe/London` | `[07:00, 07:00:59.999999]` | T:testTimezoneNamedTimezoneSummer |
| `2024-03-10T02:30@America/New_York` (gap) | `[2024-03-10T07:00:00, 07:00:59.999999]` | T:testTimezoneDstGapAdjustment |
| `2024-03-10T01:59@America/New_York;2m` | `[06:59:00.000000, 07:01:59.999998]` (artifact of the hi-only gap adjustment) | T:testTimezoneDstGapHiOnlyInGap |
| `2024-11-03T01:30@America/New_York` (overlap) | `[05:30:00, 05:30:59.999999]` (EDT) | T:testTimezoneDstOverlapFallBack |
| `2024-01-15T[09:00@+05:00,08:00@+02:00];1h` | `[04:00, 04:59:59.999999]`, `[06:00, 06:59:59.999999]` | T:testTimeListBracketPerElementTimezone |
| `2024-01-15T[09:00@UTC,14:30]@+02:00;1h` | `[09:00, 09:59:59.999999]`, `[12:30, 13:29:59.999999]` | T:…MixedTimezone |
| `[2024-01-15@UTC,2024-01-16]T08:00@+03:00` | `[01-15T08:00, …]`, `[01-16T05:00, …]` | T:testTimezoneDateListMixedTimezones |
| `[2024-01-15@Europe/London,2024-07-15@Europe/London]T08:00` | `[01-15T08:00, …]`, `[07-15T07:00, …]` | |
| `[2024-01-01@+14:00]#Mon` | `[2023-12-31T10:00, 2024-01-01T09:59:59.999999]` (local Monday) | T:testDayFilterDateListTimezoneChangesDayOfWeek |
| `2024-01-[01..02]@+12:00#Mon` | `[2023-12-31T12:00, 2024-01-01T11:59:59.999999]` | T:testDayFilterBracketExpansionWithTimezoneConsistent |
| `[$now - 1h..$now]@America/New_York#Mon` (now 2026-01-26T23:30Z) | `[2026-01-27T03:30, 04:30]` (local Monday) | T:testDayFilterLocalTimeSemanticsMonday |
| `2024-01-15T08:00@`, `@InvalidZone`, `@+02:x0` | error `invalid timezone` | |

## 9. Day filters `#…`

Names (IU:4233-4287, IU:4493-4587), all case-insensitive:
- Keywords: `workday` and `wd` (Mon–Fri), `weekend` (Sat, Sun).
- Day names, 3-letter or full: `Mon/Monday`, `Tue/Tuesday`, `Wed/Wednesday`, `Thu/Thursday`, `Fri/Friday`, `Sat/Saturday`, `Sun/Sunday`.
- Comma lists such as `#Mon,Wed,Fri`, mixing short and full names. Keywords cannot appear inside a list. Empty items are skipped. No whitespace is allowed.

Placement: anywhere outside brackets before the first `;`, but conventionally after `@tz`. A per-element filter goes inside a date list as `date#Mon`. Without a tick-calendar service (the OSS default, `DefaultTickCalendarService` returns null), `#XNYS` is parsed as a day name and gives `Invalid day name: XNYS`.

Semantics (`applyDayFilter`, IU:1611-1684), applied to the local time:
- If the element has day precision (≥2 `-`, including `YYYY-Www-D`), or it is a var range: keep the whole interval if **lo's** weekday matches. This holds even if the interval spans several days because of a duration.
- If the element is a year, month or ISO week (a multi-day interval): split it into one full-day interval per matching day.

**Merge quirk:** the non-bracket, non-list path (IU:1075-1121) does **not** union afterwards. So `2024-W01#workday` returns 5 *separate* day intervals, while `2024-01-[01..07]#workday` returns a single merged interval.

| pattern | intervals | source |
|---|---|---|
| `2024-01-[01..07]#workday` / `#wd` | `[2024-01-01T00:00, 2024-01-05T23:59:59.999999]` | T:testDayFilterWorkday(Short) |
| `2024-01-[01..07]#WEEKEND` | `[01-06T00:00, 01-07T23:59:59.999999]` | T:testDayFilterCaseInsensitive |
| `2024-01-[01..07]#Mon,Wed,Fri` | Jan 1, Jan 3, Jan 5 full days | T:testDayFilterMultipleDays |
| `2024-01-[01..07]#Monday,Wednesday` | Jan 1, Jan 3 | T:testDayFilterFullDayNames |
| `2024-W01#Tue` | `[2024-01-02T00:00, 23:59:59.999999]` | T:testDayFilterIsoWeek |
| `2024-W01#workday` | 5 separate intervals, Jan 1..5 (not merged) | T:testDayFilterIsoWeekMultipleDays |
| `2022#Tue` / `2022#Sat` | 52 / 53 intervals | T:testDayFilterYearOnly(Matches) |
| `2024-12#Mon` | 5 intervals (Dec 2..30) | T:testDayFilterMonthStartingSunday |
| `2024-01-01#Tue` | `[]` | T:testDayFilterSingleDateNoMatch |
| `[2024-01-01#Mon,2024-01-06]#Sat` | Jan 1 and Jan 6 (element filter wins) | T:testDateListMixedPerElementAndGlobalDayFilter |
| `[2026-01-05T09:30, 2026-01-06]T10:00#Mon` | `[01-05T09:30, 09:30:59.999999]` | T:…TimeAndGlobalDayFilter |
| `2024-01-[01..07]T09:00@+02:00#workday;1h` | 5 intervals `[DDT07:00, 07:59:59.999999]`, DD=01..05 | T:testDayFilterWithTimezone |
| `$today#Thu` / `$today#Mon` (now Thu 2026-01-22) | Jan 22 full day / `[]` | T:testDateVariableBareWithDayFilter(NoMatch) |
| `[$now - 2h..$now]@America/New_York#Thu` | `[13:30, 15:30]` | T:…GlobalTimezoneAndDayFilter |
| `#` empty | error `Empty day filter after '#'` | |
| `#invalid`, `#Mon,invalid,Fri` | error `Invalid day name: invalid` | |
| `#,,,` | error `Invalid day filter: ,,,` | |

## 10. ISO weeks

Grammar (MTD:1423-1551): `YYYY-Www[-D][(T| )HH[:MM[:SS[.f…]]]]`.
- `ww` is exactly 2 digits, from 01 to `getWeeks(year)` (52 or 53).
- `D` is 1..7 (Mon..Sun).
- `YYYY` is the **ISO week-year**. Week 1 is the week whose Monday is `Jan 1 + getIsoYearDayOffset` (ISO 8601: the week that contains the first Thursday).
- Precision works the same as in section 2. A space separator works here and still gives a precision interval, unlike plain dates.
- A time part requires `-D`.
  - [CODE] `2024-W01T09:00` and `2024-W01;1h` give `Invalid date: 2024-W01`. The bare-imprecise rewrite tries to parse the week as `YYYY-MM` and fails (IU:3169-3174).
  - `2024-W01#Tue` is fine.

| pattern | intervals | source |
|---|---|---|
| `2024-W01` | `[2024-01-01T00:00, 2024-01-07T23:59:59.999999]` | T:testIsoWeekBasic |
| `2020-W01` | `[2019-12-30T00:00, 2020-01-05T23:59:59.999999]` | T:testIsoWeekYearBoundary |
| `2020-W53` / `2015-W53` | `[2020-12-28, 2021-01-03T23:59:59.999999]` / `[2015-12-28, 2016-01-03T…]` | |
| `2024-W52` | `[2024-12-23, 2024-12-29T23:59:59.999999]` | |
| `2024-W01-5` | `[2024-01-05T00:00, 23:59:59.999999]` | T:testIsoWeekWithDay |
| `2024-W01-1T09` | `[09:00, 09:59:59.999999]` | T:testIsoWeekHourOnly |
| `2024-W01-1T09:30:15.1` | `[09:30:15.100000, 09:30:15.199999]` | T:testIsoWeekPartialMicroseconds |
| `2024-W01-1 09:00` | `[09:00, 09:00:59.999999]` | T:testIsoWeekWithSpaceSeparator |
| `2024-W01-1T09:00;1h` | `[09:00, 09:59:59.999999]` | T:testIsoWeekWithDurationSuffix |
| `2024-W01-1T09:00@+02:00` | `[07:00, 07:00:59.999999]` | |
| `2024-W[01..04]` | `[2024-01-01, 2024-01-28T23:59:59.999999]` | T:testIsoWeekBracketExpansionWeek |
| `2024-W01-[1..5]` | `[2024-01-01, 2024-01-05T23:59:59.999999]` | |
| `2024-W[01,02]-[1,5]` | Jan 1, Jan 5, Jan 8, Jan 12 | |
| `2024-W53`, `W00`, `W54`, `W1`, `W01-`, `W01-0`, `W01-8`, `W01-1T`, `2024-W[52..53]` | error `Invalid date` | |

## 11. Sorting and merging

- In static mode, the final step for date lists, bracket expansions and `$` expressions (`unionBracketExpandedIntervals`, IU:4746-4774) sorts by lo and merges intervals that **overlap or are adjacent** (`lo <= prevHi || lo == prevHi + 1`). A gap of 2µs keeps intervals apart (IUT:testUnionBracketExpandedIntervalsGapOfTwoStaysSeparate).
- The merge runs only when there are more than one interval.
- Exception: in the plain non-bracket path (e.g. `2024-W01#workday`), the intervals produced by the day filter are not merged (section 9).
- Duplicates collapse: `[2025-01-05,2025-01-05,2025-01-05]` gives one interval (T:testDateListOverlappingIntervals).
- Unsorted input is sorted: `[2025-01-20,2025-01-05,2025-01-15]` gives 05, 15, 20 (T:testDateListUnsortedDates).
- Inverted intervals (from `;0d`) are not removed.
- The separate `unionInPlace` operation, used when combining different `IN`/`AND` predicates, merges only when `lo <= prevHi` (it touches, but adjacency alone does not merge; IUT:testUnionInPlaceAdjacentRegionsStaySeparate). That operation is outside TICK parsing.

## 12. Error messages, verbatim

QuestDB prefixes each message with `[position] `. `…` means the offending text follows.

| message | trigger example |
|---|---|
| `Invalid date: …` | `2016-02-30`, `xyz-[10,15];1h`, `[..$today]`, `[2024-13]T09:30`, `   ` (static), `2024-01-01..2024-01-05` [CODE] |
| `Invalid interval format: …` | `2018-01-10T10:30;30m;2d` (exactly 2 `;`) |
| `Invalid interval format (too many semicolons): …` | 4 or more `;` (`2018-01-10;1h;1d;2;extra`) |
| `Period not a number` | `2018-01-10T10:30;30m;Xd;2` |
| `Count not a number` | `…;30m;2d;X` |
| `Count must be positive` | `…;1s;1h;0`, `…;-1s;-1h;-2` |
| `Unknown period: w at 1` | `2018-01-10T10:30;30m;2w;3` (period units y M d h m s only) |
| `Expected number before unit '<c>'` | `…;h`, `$today; h` |
| `Duration not a number: …` | `…;999999999999h` |
| `Invalid duration unit: <c>` | `…;1x` |
| `Missing unit at end of duration` | `…;1h30`, `$today;30` |
| `Unclosed '[' in interval` | `2018-01-[10,15` |
| `Unclosed '[' in date list` | `[2025-01-01`, `[$today` |
| `Empty date list` | `[]` |
| `Empty element in date list` | `[2025-01-01,,2025-01-02]`, `[,x]`, `[x,]`, `[   ,   ]` |
| `Empty bracket expansion` | `2018-01-[]`, `2024-01-15T[];1h` |
| `Expected number in bracket expansion` | `2018-01-[abc]`, `[[10]]`, int overflow |
| `Expected number after '..'` | `2018-01-[10..]`, overflow end |
| `Range must be ascending: A..B` | `2018-01-[15..10]` |
| `Expected ',' or end of bracket` | `2018-01-[10.5]`, `[10;15]`, `[[2025-01-01]]` |
| `Military time format not supported in bracket expansion. Use colons: [09:00,14:30] instead of [0900,1430]` | `T[0900,1430]` |
| `Too many bracket groups (max N)` | 9 groups (N = `sqlIntervalMaxBracketDepth`, 8 by default per the test comment) |
| `Bracket expansion produces too many intervals (max N)` | `[1000..3000]-01-01` (N=1024 by default per the test comment) |
| `Date range expansion produces too many intervals (max N)` | a `$` day range with more than N non-adjacent days after merging [CODE] |
| `Empty element in time list` | `T[09:00,,14:30]`, `[$today]T[ ,14:00]` |
| `Nested brackets not supported in time list. Use separate expansions: T[09:00,09:30] instead of T[09:[00,30]]` | `T[09:[00,30],14:30]` |
| `invalid timezone: …` | `@`, `@InvalidZone`, `@+02:x0`, `$today@Bogus/Zone` |
| `Empty day filter after '#'` | `…#` |
| `Invalid day name: …` | `#invalid`, `#XNYS` without calendars, `[$today#workday..$tomorrow]` |
| `Invalid day filter: …` | `#,,,` |
| `comma-separated date lists require brackets, e.g., [$now,$tomorrow]` | `$now,$tomorrow` |
| `Unknown date variable: …` | `$invalid`, `$garbage`, `[$today.$tomorrow]` (gives `…: tomorrow`), `$nowT` |
| `Expected '+' or '-' operator` | `[$today * 5d]` |
| `Expected number after operator` | `[$today +]`, `[$today + $today]`, `[$today+..$today+5d]` |
| `Invalid number in date expression` | `[$today + 99999999999d]`, `1__000T`, `_100T`, `100_T` |
| `Expected time unit after number` | `[$today + 5]` |
| `Unexpected characters after unit` | `[$today + 3dabc]`, `[$today + 2bdxyz]`, `[$today + 5bx]`, `$today - 1d + 2h` [CODE] |
| `Invalid time unit: <c>` | `[$today + 5x]` |
| `Empty end expression in date range` | `[$today..]` |
| `Invalid date range: start is after end` | `[$today+5d..$today]` |

These come **only from the compiled path**, which is used when the expression contains a `$var` (IU:154-2462):

| message | trigger example |
|---|---|
| `Empty tick expression` | all-whitespace input on the compiled path |
| `Invalid time override: T with no value` | `[$today]T` |
| `Invalid time override: …` | `[$today]Tabc` |
| `Expected 'T' time override, got: <c>` | `[$today]09:30` |
| `Invalid time in time list: …` | `[$today]T[abc,14:00]` |
| `invalid timezone in time list: …` | `[$today]T[09:00@Bogus,14:00]` |
| `Unclosed '[' in time list` | `[$today]T[09:00` |

## 13. Accepted syntax the docs don't mention

- **Legacy repeating intervals** `date;duration;period;count`. The period unit is one of `y M d h m s`, and the period may be negative. Adjacent repeats merge.
  - `2018-01-10T10:30;30m;2h;3` gives `[10:30, 10:59:59.999999]`, `[12:30, …]`, `[14:30, …]`.
  - `2015-03-12T11:00:00;5m;-2M;3` gives Nov 12, Jan 12 and Mar 12 at `[11:00, 11:04:59.999999]`.
  - `2016-03-21;3y;6M;5` gives `[2016-03-21, 2021-03-20T23:59:59.999999]`.
  - Tests: T:testRepeatingInterval*, testParseLongInterval*, testParseLongMinusInterval.
- **Point intervals** (section 2) from:
  - a raw epoch integer;
  - a space separator;
  - a `Z` or `±hh[:mm]` suffix on a second-or-finer timestamp, which parseFloor applies (`2015-02-28T08:22:44.556011-01` becomes `09:22:44.556011Z`; T:testDateFloor*).
- **Fractions** of 1–9 digits. Digits beyond microseconds are truncated on the micro driver.
- **`$now`-style points** inside lists, with a per-element tz: `[$now@Europe/London, 2026-01-15@America/New_York]`.
- **Durations:** negative, explicitly positive (`+3d`), zero, multi-part, mixed-sign, and with `_` in numbers. Only one duration per element is possible: extra `;` switch to the repeating syntax.
- **Per-element modifiers in date lists:** day filters, time zones and times.
- **Time zones:** `@Z`, `@UTC+05:00`, `@+0530`, `@+05`.
- **Upper-case variables and units:** `$TODAY`, `D`, `BD`.
- **Whitespace** inside variable expressions (`$today   +   3 d`) and in brackets.
- **Exchange calendars** `#XNYS` / `#XHKG` (TC:). These need a non-default `TickCalendarService`. The filter intersects with the UTC trading sessions after tz conversion, and a duration then extends each session's hi.
