# Ideas for later

Possible additions that are not planned yet. Nothing here is implemented.

None of these change the `x IN '<pattern>'` syntax. They are separate, explicitly named functions that
reuse the extension's time zone handling, calendar arithmetic and duration parser. With them, `tick`
would become a small kit of time syntaxes, with TICK as the main one, and the README should say so.

## Other time syntaxes as functions

Ordered by value for effort.

### 1. Splunk relative time: `splunk_time(modifier [, anchor]) → TIMESTAMPTZ`

`splunk_time('-1d@d')` gives yesterday at midnight. High value for log analysis, small grammar, and most of
the machinery already exists. An Elasticsearch twin, `es_time('now-1M/M')`, can share the same core.

Draft semantics: [SPLUNK_TIME_SEMANTICS.md](SPLUNK_TIME_SEMANTICS.md).

### 2. ISO 8601 durations: `iso8601_duration(text) → INTERVAL`

`P1Y2M10DT2H30M` → `INTERVAL`. Years and months go into the interval's months, weeks and days into its
days, and the time part into its microseconds.

**First check whether DuckDB's `VARCHAR → INTERVAL` cast already accepts it**
(`SELECT 'P1Y2M10DT2H30M'::INTERVAL`). If it doesn't, an upstream contribution to DuckDB would reach more
users than a function here.

### 3. Pandas frequencies: `pandas_date_range` / `pandas_bucket`

A bare `pandas_freq(text) → INTERVAL` is not worth doing: anchored frequencies (`BME`, `W-MON`, `QS-JAN`,
`B`) are rules, not lengths, and an `INTERVAL` cannot hold them. Useful shapes:
- `pandas_date_range(start, end, freq)`: a table function, the equivalent of `pd.date_range`. DuckDB's
  `generate_series` has no business-day or anchored steps.
- `pandas_bucket(freq, ts)`: `time_bucket` with anchored frequencies.

Caveats: the most work and the most edge cases. The alias set changes between pandas versions (2.2
renamed `T` → `min` and `M` → `ME`), so we would pick a version and probably accept the older aliases too.
The `bd` business-day logic in `tick_pattern.cpp` is a starting point.

### 4. PromQL durations: `promql_duration(text) → INTERVAL`

`1h30m`, `500ms`. A few lines on top of the existing duration parser. Only worth adding alongside the
others.

## TICK itself

### Exchange calendars (`#XNYS` and other ISO 10383 codes)

The one TICK feature not implemented. It is feasible without an external service if the calendars ship as
data. The hard part is keeping that data current, not the code.

**Semantics are mostly known.** QuestDB's open-source code includes the `TickCalendarService` hook and
`TickCalendarTest`, though the calendars themselves are Enterprise-only. As recorded in
[TICK_SEMANTICS.md](TICK_SEMANTICS.md): the filter intersects the expanded intervals with the exchange's
trading sessions in UTC, after the time-zone conversion, and a duration then extends each session's end.
In the engine this is one more sorted interval list to intersect with.

**The data is mostly rules.** Most exchange holidays follow fixed patterns:
- fixed dates, or "the nth weekday of a month";
- holidays tied to Easter;
- moving a holiday to Monday when it falls on a weekend;
- weekly session times, plus the dates those times changed.

The Python library [`exchange_calendars`](https://github.com/gerrymanoim/exchange_calendars) (Apache 2.0,
about 50 exchanges) already encodes all of this. Two hard categories remain:
- **One-off closures** (9/11, Hurricane Sandy, the day of mourning for Carter on 2025-01-09) cannot be
  computed and must be listed date by date.
- **Lunar holidays** (Chinese New Year, Diwali, Eid; Shanghai, Bombay, Jakarta among others) are
  precomputed lists even in `exchange_calendars`, and only reach a few years ahead.

Add early closes (half days) and lunch breaks (Hong Kong, Tokyo), and the data is fiddly but small:
kilobytes per exchange.

**Plan: embedded data, overridable by the user.**
1. *Embedded tables.* A build-time script exports holidays, early closes and session-time changes from
   `exchange_calendars` (e.g. 1990 to about two years ahead) into a compiled-in table. Works offline and is
   reproducible, but goes stale: new holidays (Juneteenth from 2022, the 2023 coronation) need a new
   release of the extension.
2. *User-registered calendars*, e.g. `tick_register_calendar('XNYS', 'xnys_sessions.parquet')`, with one
   open/close row per session. Covers exchanges we don't ship, lets firms use their licensed vendor data,
   and makes data quality the user's responsibility.

Ship 1 as the default and let 2 override or extend it.

**Rule:** past the end of a calendar's data, raise an error instead of projecting the rules forward. A
silent guess gives wrong answers that look plausible.

**Cost:** someone has to regenerate and release the data once or twice a year. Without that commitment,
2 alone is still worthwhile: the `#XNYS` syntax works and the user supplies the data.
