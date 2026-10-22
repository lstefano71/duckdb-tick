# `splunk_time`: draft semantics

> **Status: draft, not implemented.** This is a design proposal written from memory of Splunk's time
> modifiers, not from Splunk's source or test suite. Every rule marked **[VERIFY]** must be checked against
> the Splunk documentation ("Time modifiers" in the Search Reference) before implementation.
> See [IDEAS.md](IDEAS.md) for how this fits with other possible additions.

## Goal

A scalar function that turns a Splunk relative time modifier into a DuckDB timestamp:

```sql
-- Yesterday, midnight to midnight
SELECT * FROM logs
WHERE ts >= splunk_time('-1d@d') AND ts < splunk_time('@d');

-- Since the start of the current week (Monday)
SELECT * FROM logs WHERE ts >= splunk_time('@w1');
```

It is a separate function, not part of the `x IN '<pattern>'` syntax. TICK already gives `@` a meaning
(time zone), so the two languages cannot share one string.

## Signatures

| signature | result |
|---|---|
| `splunk_time(modifier VARCHAR) → TIMESTAMPTZ` | relative to "now" |
| `splunk_time(modifier VARCHAR, anchor TIMESTAMPTZ) → TIMESTAMPTZ` | relative to `anchor` |

- **"now"** is the transaction start time, or the `tick_now` setting when it is set. This is the same
  instant TICK's `$now` uses, so the two always agree within a query.
- **Time zone.** Snapping and calendar arithmetic use the session `TimeZone` setting (Splunk uses the
  user's configured time zone). A plain `TIMESTAMP` anchor is cast to `TIMESTAMPTZ` by DuckDB's usual
  rule, i.e. read in the session time zone.
- **NULL** in either argument gives NULL. A malformed modifier is an error, not NULL; `TRY(...)` covers
  the lenient case.
- **Stability** is `CONSISTENT_WITHIN_QUERY`, as for `tick_in`: the result depends on the transaction
  start time and on `TimeZone`.

### Pruning

The function is only worth having if `ts >= splunk_time('-1d@d')` still skips row groups. Plan: when the
arguments are constant, evaluate at bind time and replace the call with a `TIMESTAMPTZ` constant, as TICK
expands its pattern once at bind time. The comparison is then an ordinary constant filter that every
scanner can push down. **To check during implementation:** whether DuckDB already folds a
`CONSISTENT_WITHIN_QUERY` call with constant arguments (as it handles `now()`), in which case the
bind-time rewrite is unnecessary. Confirm with `EXPLAIN` against a Parquet file either way.

## Grammar

```text
modifier  := 'now'
           | epoch
           | absolute
           | step* snap? step*      (at least one step or snap)
step      := ('+' | '-') integer? unit
snap      := '@' snap_unit
epoch     := digits                 (Unix seconds)
absolute  := MM/DD/YYYY:HH:MM:SS    [VERIFY: Splunk's default timeformat]
```

- Whitespace is not allowed inside a modifier [VERIFY]. Leading/trailing whitespace is trimmed.
- A missing integer means 1: `-d` is `-1d` [VERIFY].
- Steps and snaps apply left to right: `-1d@d+9h` means "go back one day, snap to midnight, add nine hours".
  Several snaps in one modifier (`-1mon@mon@w1`) are allowed and also apply left to right [VERIFY].
- An unsigned integer with no unit is an epoch in seconds. `0` is 1970-01-01 00:00:00 UTC.
- Units are case-sensitive [VERIFY]: `m` is minutes and `mon` is months; there is no `M`.

### Units

| unit | accepted spellings | arithmetic |
|---|---|---|
| second | `s` `sec` `secs` `second` `seconds` | absolute |
| minute | `m` `min` `mins` `minute` `minutes` | absolute |
| hour | `h` `hr` `hrs` `hour` `hours` | absolute |
| day | `d` `day` `days` | calendar |
| week | `w` `week` `weeks` | calendar (7 days) |
| month | `mon` `month` `months` | calendar |
| quarter | `q` `qtr` `qtrs` `quarter` `quarters` | calendar (3 months) |
| year | `y` `yr` `yrs` `year` `years` | calendar |

Sub-second units (`ms`, `us`, `cs`, `ds`) exist in some Splunk contexts [VERIFY]. If they do, add
them as absolute units.

Snap units: any unit above, without an integer, plus `w0`–`w6` (day of week, `w0` = Sunday,
`w1` = Monday … `w6` = Saturday). `@w` is the same as `@w0` [VERIFY]. Integers on snap units
(`@15m`, `@2h`) are rejected [VERIFY].

## Semantics

**Absolute vs calendar arithmetic.** Seconds, minutes and hours are added to the instant. Days, weeks,
months, quarters and years are added to the local wall-clock date in the session time zone, keeping the
local time of day. This matches DuckDB's `TIMESTAMPTZ + INTERVAL`, where the micros part is absolute and
the days and months parts follow the calendar. Across a DST change, `-1d` and `-24h` therefore differ by an
hour [VERIFY that Splunk behaves the same].

**Month overflow.** Adding months to a day that does not exist in the target month clamps to its last day
(Mar 31 `-1mon` → Feb 28/29), as DuckDB and TICK's `M` do [VERIFY].

**Snapping** rounds *down* to the latest boundary that is not after the current value:
- `@s`, `@m`, `@h`: truncate the local wall-clock time, keeping the UTC offset of the value being snapped.
  Inside the repeated hour of a DST fall-back, `@h` stays in the same occurrence of that hour.
- `@d`: local midnight of the current date.
- `@w0`–`@w6`: local midnight of the most recent such weekday, today included.
- `@mon`, `@q`, `@y`: local midnight on the first day of the month, of the quarter (Jan, Apr, Jul, Oct),
  or of the year.

**Resolving local times.** When a snap or calendar step lands on a local time that does not exist (in a DST
gap; e.g. midnight in zones that change at 00:00), it moves forward by the gap, as DuckDB's casts and TICK
do. When it lands on an ambiguous local time, the earlier instant (the first occurrence) is used [design
decision].

**Ranges.** Splunk's `earliest` is inclusive and `latest` is exclusive. Users write
`ts >= splunk_time(e) AND ts < splunk_time(l)`. No range function is proposed for now.

## Examples

`TimeZone = 'UTC'`, now = `2026-01-22 10:30:00+00` (a Thursday; the same "now" as
[TICK_SEMANTICS.md](TICK_SEMANTICS.md)).

| modifier | result |
|---|---|
| `now` | `2026-01-22 10:30:00` |
| `-1d` | `2026-01-21 10:30:00` |
| `-24h` | `2026-01-21 10:30:00` |
| `@d` | `2026-01-22 00:00:00` |
| `-1d@d` | `2026-01-21 00:00:00` |
| `-1d@d+9h` | `2026-01-21 09:00:00` |
| `@d-2h` | `2026-01-21 22:00:00` |
| `-90m@h` | `2026-01-22 09:00:00` |
| `+3h@h` | `2026-01-22 13:00:00` |
| `@w0` / `@w` | `2026-01-18 00:00:00` (Sunday) |
| `@w1` | `2026-01-19 00:00:00` (Monday) |
| `@w4` | `2026-01-22 00:00:00` (today is Thursday) |
| `@w5` | `2026-01-16 00:00:00` (previous Friday) |
| `@mon` | `2026-01-01 00:00:00` |
| `-mon@mon` | `2025-12-01 00:00:00` |
| `-1q@q` | `2025-10-01 00:00:00` |
| `-1y@y` | `2025-01-01 00:00:00` |
| `0` | `1970-01-01 00:00:00` |
| `1700000000` | `2023-11-14 22:13:20` |

DST, `TimeZone = 'America/New_York'`, now = `2026-03-08 12:00:00-04` (the day clocks moved forward at 02:00):

| modifier | result |
|---|---|
| `-1d` | `2026-03-07 12:00:00-05` (23 hours earlier) |
| `-24h` | `2026-03-07 11:00:00-05` |
| `@d` | `2026-03-08 00:00:00-05` |
| `@d+3h` | `2026-03-08 04:00:00-04` (hours are absolute: three real hours after midnight, across the skipped hour) |

Errors: `''`, `@`, `-1x`, `@15m`, `@w7`, `1d` (a step needs a sign), `-1d @d` [VERIFY], `rt-1h`
(real-time windows are out of scope).

## Elasticsearch sibling: `es_time`

The same core (calendar steps plus snapping) also covers Elasticsearch date math, as a second function:

```sql
SELECT * FROM logs WHERE ts >= es_time('now-1M/M') AND ts < es_time('now/M');
```

Differences to account for:
- Syntax: `now` or `<date>||` as the anchor, then `+N<unit>` / `-N<unit>` / `/<unit>`.
- Units: `y` `M` `w` `d` `h` `H` `m` `s`. `M` is month and `m` is minute, the reverse of what a Splunk user
  might expect.
- `/w` rounds to Monday (ISO week), not Sunday.
- ES rounds *up* to the last millisecond of the unit when the value is used as a `gt` or `lte` bound.
  Either add a `round_up BOOLEAN` argument or document that half-open ranges with rounding down are the
  DuckDB way. The latter is simpler.

## Implementation notes

- New source file `src/tick_splunk_time.cpp`. Reuse `TickEnvironment` (now, time zone, time zone database),
  the `tick_now` setting and the local/UTC conversions in `tick_timezone.cpp`. The calendar arithmetic
  in `tick_pattern.cpp` may be reusable as is.
- Tests in `test/sql/splunk_time.test`, using `SET tick_now` and `SET TimeZone` for reproducibility, with
  every row of the tables above plus the error cases.
- README: a short section after the TICK syntax, making clear these functions are not part of TICK.
