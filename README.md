# MiniDB

An educational persistent key-value database engine built from scratch in modern C++20.

MiniDB is a learning project. It develops the mechanisms a real storage engine
depends on — a hash table, a binary on-disk format, a write-ahead log, crash
recovery and an LRU cache, with transactions and thread-safe access planned — at a size that can
be read and understood in an afternoon. It is not a production database, and the
[Limitations](#limitations) section says plainly what it does not do.

> **Current state:** MiniDB stores data on disk and reloads it on startup.
> SET, DELETE, and CLEAR are logged and flushed to the OS before changing memory.
> Startup loads the snapshot and replays only WAL operations newer than its checkpoint. Power-loss durability is not provided.

## Status

Built in milestones, each one a working program.

- [x] **Milestone 0** — build system, warning policy, test harness, CI
- [x] **Milestone 1** — in-memory database, command parser, interactive CLI
- [x] **Milestone 2** — custom hash table with separate chaining
- [x] **Milestone 3** — persistent binary snapshots with atomic replacement
- [x] **Milestone 4** — write-ahead log and startup replay
- [x] **Milestone 5** — checkpoint-aware crash recovery
- [x] **Milestone 6** — LRU read cache
- [ ] Milestone 7 — transactions
- [ ] Milestone 8 — concurrency
- [ ] Milestone 9 — benchmarks

## What works today

| Command | Behaviour |
| --- | --- |
| `SET <key> <value>` | Stores a value, replacing any existing one. The value may contain spaces. |
| `GET <key>` | Prints the value, or `NOT_FOUND`. |
| `DELETE <key>` | Removes a key. Prints `NOT_FOUND` if it was not there. |
| `EXISTS <key>` | Prints `true` or `false`. |
| `KEYS` | Lists every key, sorted. Prints `(empty)` when there are none. |
| `CLEAR` | Removes every key. |
| `HELP` | Lists the commands. |
| `EXIT` | Saves and ends the session. End-of-input works too. |

Command names are case-insensitive; keys and values are not. The database is
recovered at startup; mutations are logged immediately and a snapshot is saved on exit.

## Requirements

- A C++20 compiler: GCC 10+, Clang 12+, or MSVC 19.29+ (Visual Studio 2019 16.10+)
- CMake 3.20 or newer

MiniDB has no third-party dependencies. It configures and builds offline.

## Quick start

```bash
cmake -S . -B build
cmake --build build
ctest --test-dir build --output-on-failure
```

Then start a session:

```bash
./build/minidb
```

MiniDB keeps its database in your user data directory
(`%LOCALAPPDATA%\MiniDB\minidb.snapshot` on Windows,
`$XDG_DATA_HOME/minidb/minidb.snapshot` elsewhere). Pass a path to use a
different file:

```bash
./build/minidb ./scratch.snapshot
```

### Build options

| Option | Default | Effect |
| --- | --- | --- |
| `MINIDB_BUILD_TESTS` | `ON` | Build the test suite |
| `MINIDB_WARNINGS_AS_ERRORS` | `OFF` | Fail the build on any compiler warning (CI sets this to `ON`) |

For a release build:

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build
```

## Example session

```text
$ ./build/minidb
MiniDB v0.1.0
Database: /home/aarya/.local/share/minidb/minidb.snapshot
New database: nothing saved here yet.
Type HELP for the command list.

MiniDB> SET name Aarya
OK

MiniDB> GET name
Aarya

MiniDB> SET name Aarya Lalan
OK

MiniDB> GET name
Aarya Lalan

MiniDB> SET age 18
OK

MiniDB> KEYS
age
name

MiniDB> EXISTS age
true

MiniDB> DELETE age
OK

MiniDB> GET age
NOT_FOUND

MiniDB> GET
INVALID_ARGUMENT: GET requires a key

MiniDB> FROBNICATE x
INVALID_ARGUMENT: unknown command 'FROBNICATE'; type HELP for the list

MiniDB> EXIT
Goodbye!
```

## Persistence

Data survives restarts. The whole database is written to a binary snapshot
when the session ends, and read back when it starts:

```text
$ ./build/minidb ./demo.snapshot
MiniDB> SET name Aarya
OK
MiniDB> SET language C++
OK
MiniDB> EXIT
Goodbye!

$ ./build/minidb ./demo.snapshot
MiniDB v0.1.0
Database: ./demo.snapshot
Loaded 2 entries.
Type HELP for the command list.

MiniDB> GET name
Aarya
MiniDB> GET language
C++
```

**How it works.** Saving writes every record to a temporary file beside the
database, then renames it over the original in one filesystem operation, so a
reader never sees a half-written file. The format is documented byte by byte
in [docs/STORAGE_FORMAT.md](docs/STORAGE_FORMAT.md).

**Recovery.** Each mutation is appended to `<snapshot>.wal` and flushed to the
OS before changing memory. Startup loads the snapshot and replays only WAL operations newer than its checkpoint.
An incomplete final record is trimmed; complete corrupt records are refused.
Saving installs the complete snapshot before resetting the WAL. See
[WAL format and recovery policy](docs/WAL.md), including tail-repair ambiguity.

**Durability limits.** Successful flushes support recovery after process
termination while the OS remains healthy. MiniDB does not call `fsync` or
`FlushFileBuffers`, and does not guarantee recovery after power loss or OS
failure. This educational single-process database has no distributed recovery or
multi-process coordination. No full ACID or production-grade guarantees are claimed.

For example: snapshot `a=1,b=2` → log `DELETE a; SET c 3` → restart →
load snapshot → replay newer records → recover exactly `b=2,c=3`.
Snapshots now include a checksum-protected checkpoint, so a restart between
snapshot replacement and WAL reset skips operations already saved. Legacy
version-1 snapshots remain readable; the next save upgrades them to version 2.

## Architecture

```mermaid
flowchart TD
    CLI[CLI] --> Parser[Command parser]
    Parser --> Database[Database API]
    Database --> Cache[LRU read cache]
    Database --> HashTable[HashTable - separate chaining]

    Parser -.-> Result[Result / StatusCode]
    Database -.-> Result

    Database --> Storage[StorageManager]
    Storage --> Snapshot[(Binary snapshot)]
    Database --> WAL[WriteAheadLog]
    WAL --> Log[(Binary WAL)]

    classDef planned stroke-dasharray: 4 4
```

Solid arrows exist today; dashed boxes are later milestones.

Each component has one job:

- **`cli/main.cpp`** — reads lines, prints replies, and nothing else. It owns
  the wording of `OK`, `true`/`false` and the sorted `KEYS` output.
- **`command_parser`** — turns a line of text into a `Command`, or into a
  `Result` explaining why it is not one. Stateless free function.
- **`Database`** — stores the data. Knows nothing about terminals or syntax.
- **`HashTable`** — MiniDB's own hash table, described below. Knows nothing
  about keys being strings or values being database entries.
- **`StorageManager`** — turns records into bytes and back, and owns every
  rule about what a valid snapshot is. Knows nothing about hash tables.
- **`WriteAheadLog`** — appends, validates, replays, and resets mutation records.

Both the parser and the database report problems through the same `Result` /
`StatusCode` pair from Milestone 0, so the CLI has one error model to render.

## LRU read cache

GET checks a 128-entry LRU cache first. A miss reads the authoritative in-memory
hash table and caches a successful result. SET/DELETE invalidate affected
entries; CLEAR and successful recovery clear the cache. The snapshot and WAL
formats are unchanged. Failed writes/recovery preserve the previous valid state.

The cache uses `std::unordered_map` plus a doubly linked `std::list`: average
O(1) lookup/PUT, O(1) list promotion and tail unlink, average O(1) map removal
for eviction. Hash collisions/rehash and string hashing/copying add costs;
these bounds are in entry count, not bytes. Clear is O(C), size/capacity O(1).

Library callers can use `Database(0)` or `Database(path, 0)` to disable caching,
or supply another entry capacity. `cache_size()` and `cache_capacity()` expose
read-only diagnostics. Existing constructors and const GET remain supported.

This is an educational cache over data already in RAM, not a disk-page cache
or a measured performance improvement. It duplicates values, has no byte-budget
or thread-safety guarantee, and leaves snapshot/WAL I/O complexity unchanged.
See [Learning notes](docs/LEARNING_NOTES.md) for an access-order example.

## The hash table

Since Milestone 2, storage is MiniDB's own `HashTable<Key, Value, Hash>` rather
than `std::unordered_map`. A key reaches its bucket like this:

```text
key ──► Hash{}(key) ──► std::size_t hash ──► hash % bucket_count ──► bucket index
```

Keys that land on the same index are kept in a singly linked chain hanging off
that bucket — **separate chaining**. A collision costs extra key comparisons
and never loses data.

| Property | Choice | Reason |
| --- | --- | --- |
| Collision strategy | Separate chaining | Deletion is a simple unlink; open addressing needs tombstones |
| Bucket counts | Primes: 17, 37, 79, 163, ... | `std::hash<int>` is the identity in libstdc++, so power-of-two counts would collide badly on ordinary integer keys |
| Max load factor | 0.75 | Keeps the expected chain under one node without wasting a large array |
| Growth | Doubling, then round up to a prime | Makes insertion amortised O(1); fixed-size growth would be O(n²) overall |
| Node storage | `std::unique_ptr` chains | Explicit ownership, no leaks, no raw owning pointers |
| Hash caching | Stored per node | Rehashing recomputes only a remainder, never re-hashes a key |

Two properties worth knowing:

- **Pointers survive a rehash.** Growing relinks existing nodes rather than
  recreating them, so a pointer from `find()` stays valid. Only erasing that
  entry or clearing the table invalidates it.
- **Teardown is iterative.** Destroying a chain through its head `unique_ptr`
  would recurse once per node and exhaust the stack on a long chain, so
  `clear()` unlinks each node before releasing it. There is a test that builds
  a 20,000-node chain to prove it.

## Complexity

As provided by `HashTable`:

| Operation | Complexity |
| --- | --- |
| `SET` | Average O(1), worst case O(n) |
| `GET` | Average O(1), worst case O(n) |
| `DELETE` | Average O(1), worst case O(n) |
| `EXISTS` | Average O(1), worst case O(n) |
| `KEYS` | O(n) |
| `CLEAR` | O(n) |
| `size` | O(1) |
| Loading a snapshot | O(n) time, O(n) memory |
| Saving a snapshot | O(n) time |

Persistence is not O(1) in any form: saving and loading each touch every
record. Plus, on the table itself: `rehash` is O(n), and space is O(n + bucket_count).

These averages are **expected** values, not guarantees. They hold on two
conditions: the hash spreads keys evenly, and the load factor stays bounded.
MiniDB enforces the second by rehashing at 0.75 and delegates the first to
`std::hash`. The worst case is every key colliding into one bucket, which turns
the table into a linked list — the test suite reproduces this deliberately
rather than assuming it cannot happen.

`KEYS` as printed by the CLI is O(n log n), because the CLI sorts for readable
output; `Database::keys()` itself is O(n).

## Testing

Every test file compiles to its own executable and registers as its own CTest
test, so one crashing test cannot take down the rest of the suite.

```bash
ctest --test-dir build --output-on-failure
```

To run a single test verbosely:

```bash
ctest --test-dir build -R test_database -V
```

| Suite | Covers |
| --- | --- |
| `test_recovery` / `test_recovery_process` | Checkpoints, legacy files, malformed recovery inputs, truncated tails, abrupt subprocess exit and CLI restart |
| `test_lru_cache` | Recency, eviction, zero/one capacity, copy/move safety, mixed-operation model, Database invalidation and recovery |
| `test_wal` | Binary encoding, replay, torn tails, corruption, length limits, failed I/O, snapshot/reset ordering, restart recovery |
| `test_storage` | Round trips, corrupt magic, bad version, truncation at every offset, invalid lengths, overflow attempts, duplicate keys, checksum failures, scratch-file cleanup |
| `test_hash_table` | Insert, lookup, update, erase, clear, resizing, forced collisions, chain surgery, rehash preservation, value semantics |
| `test_database` | Every operation, empty state, overwrites, size limits, binary values, 1000-key stress |
| `test_command_parser` | Every command, spaces in values, case handling, missing and extra arguments, unknown commands |
| `test_result` | The `Result` / `StatusCode` error model |

Tests use a small in-repo framework (`tests/test_framework.hpp`) rather than a
third-party library. The reasoning is in
[docs/DESIGN_DECISIONS.md](docs/DESIGN_DECISIONS.md).

## Limitations

As of Milestone 6, MiniDB does **not**:

- Guarantee durability against power loss. Saves are not `fsync`ed.
- Update the snapshot incrementally. Every save rewrites the whole file, so
  saving is O(n) however small the change.
- Coordinate between processes. There is no locking, and two MiniDB processes
  on one database will overwrite each other.
- Support transactions, caching, or concurrent access.
- Allow spaces in keys, since arguments are whitespace-separated.
- Allow an empty value from the CLI, though `Database::set` accepts one.

And it is not intended to ever implement:

- SQL, or any query language beyond simple key-value commands
- A network protocol or client/server mode
- Replication, sharding, or any distributed behaviour
- A query planner or optimiser
- Full ACID isolation between concurrent transactions
- Multi-process coordination

## Documentation

- [WAL format and recovery](docs/WAL.md) — mutation records, durability, and limitations
- [Storage format](docs/STORAGE_FORMAT.md) — the snapshot layout, byte by byte
- [Design decisions](docs/DESIGN_DECISIONS.md) — why things are built the way they are
- [Learning notes](docs/LEARNING_NOTES.md) — the concepts behind the code, explained from scratch

## License

[MIT](LICENSE)
