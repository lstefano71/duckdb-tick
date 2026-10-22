# tick — QuestDB TICK interval syntax for DuckDB

`tick` is a DuckDB extension that brings [QuestDB's TICK syntax](https://questdb.com/docs/query/operators/tick/)
(Temporal Interval Calendar Kit) to DuckDB: one compact string describing many time windows, used directly
in a `WHERE` clause.

```sql
-- NYSE trading hours on every workday of January 2025
SELECT * FROM trades
WHERE ts IN '[2025-01]T09:30@America/New_York#workday;6h30m';

-- The last five business days, at market open, for one minute
SELECT * FROM prices
WHERE ts IN '[$today - 5bd..$today - 1bd]T09:30@America/New_York;1m';
```

> **Status:** early development. The extension builds against DuckDB v2.0; the TICK grammar is being
> implemented feature by feature. The sections below describe the intended behaviour.

## How it works

DuckDB v2.0 parses SQL with an extensible PEG grammar. `tick` registers a grammar extension that adds one
alternative to the `IN` predicate: when the right-hand side is a single string literal, `x IN '...'` is
parsed as `tick_in(x, '...')` (and `x NOT IN '...'` as `NOT tick_in(x, '...')`).

When the pattern is a constant, `tick_in` is expanded at bind time into a disjunction of half-open ranges,
`(x >= lo AND x < hi) OR ...`, so DuckDB's ordinary filter pushdown and zone-map pruning apply.

## Syntax

Components appear in this order:

```text
date [T time] @timezone #dayFilter ;duration
```

| Feature          | Examples                                                              |
| ---------------- | --------------------------------------------------------------------- |
| Dates            | `2024-01-15`, `2024-01`, `2024`, `2024-W01-1`                          |
| Bracket expansion| `2024-01-[10,15,20]`, `2024-01-[10..15]`, `2024-[01,06]-[10,15]`        |
| Date lists       | `[2024-01-15,2024-03-20]`, `[2024-12-31,2025-01-[01..05]]`              |
| Times            | `T09:30`, `T[09:00,12:00]`, `T[09,14]:30`                              |
| Variables        | `$now`, `$today`, `$yesterday`, `$tomorrow`, with `+`/`-` arithmetic   |
| Ranges           | `$now - 2h..$now`, `$today - 5bd..$today - 1bd`                        |
| Time zones       | `@America/New_York`, `@+02:00`, `@UTC`                                 |
| Day filters      | `#workday`, `#weekend`, `#Mon,Wed,Fri`                                 |
| Durations        | `;1h`, `;6h30m`, `;500T250u`                                           |

Units: `y` `M` `w` `d` `h` `m` `s` `T` (ms) `u` (µs) `n` (ns); `bd` (business days) in arithmetic only.
See the [QuestDB reference](https://questdb.com/docs/query/operators/tick/) for full semantics.

Not supported: exchange calendars (`#XNYS` and other ISO 10383 codes), a QuestDB Enterprise feature.

## DuckDB specifics

- **Activation.** Loading the extension enables the grammar for the current and future connections.
  It can be switched off per connection with `SET active_grammar_extensions = []`.
- **Time zones follow DuckDB.** Unlike QuestDB, which is UTC throughout, a pattern without `@timezone`
  is read in the session's `TimeZone` setting, and plain `TIMESTAMP` / `TIMESTAMP_NS` values are treated
  as wall-clock times in that zone. `TIMESTAMPTZ` values are absolute instants. `SET TimeZone = 'UTC'`
  gives QuestDB's behaviour.
- **`$now` / `$today`** are taken from the transaction start time.
- **Strings.** `'ell' IN 'hello'` (DuckDB's substring containment) keeps working for `VARCHAR` operands.

The building blocks are also callable directly:

```sql
SELECT * FROM tick_intervals('2024-01-[15,16,17]T09:00;1h');   -- the expanded intervals
SELECT tick_in(TIMESTAMP '2024-01-15 09:30', '2024-01-15T09:00;1h');
```

## Building

The repository follows the [DuckDB extension template](docs/TEMPLATE_README.md). Clone with submodules:

```sh
git clone --recurse-submodules <repo-url>
make            # release build
make test       # run the SQL logic tests
```

Build artefacts:

```
./build/release/duckdb                                   # DuckDB shell with tick linked in
./build/release/test/unittest                            # test runner
./build/release/extension/tick/tick.duckdb_extension     # loadable extension
```

Tests live in `test/sql` as [SQLLogicTests](https://duckdb.org/dev/sqllogictest/intro.html).
See [docs/UPDATING.md](docs/UPDATING.md) for moving to a new DuckDB release.

## License

MIT — see [LICENSE](LICENSE).
