# Design Decisions

A record of choices that were not obvious, and the reasoning behind them.
Entries are added as each milestone lands.

---

## Milestone 0 — Project foundation

### Why C++20

C++20 is the newest standard with solid support across GCC, Clang and MSVC. The
features MiniDB actually uses are modest, and each earns its place:

- **Concepts / `requires` expressions** — the test framework asks at compile
  time whether a type can be streamed to `std::ostream`, and falls back to a
  placeholder if not. In C++17 this needs a hand-written SFINAE trait.
- **`[[no_unique_address]]`** — lets the hash table hold a stateless hasher
  without the empty object costing a byte of every table.

An earlier draft of this section also claimed designated initialisers and
`std::span` would be useful once the storage format existed. Milestone 3 landed
without needing either, so the claim is removed rather than left standing.

`CXX_EXTENSIONS` is set to `OFF`. That compiles with `-std=c++20` rather than
`-std=gnu++20`, so a GNU-only extension fails on the machine where it is written
instead of on someone else's compiler.

### Why a static library plus a thin executable

The engine is built as `minidb_core` and the CLI as a separate `minidb` target.
The alternative — one executable containing everything — is simpler by one CMake
command and worse in every other respect:

- Tests can only exercise code they can link against. With one big executable,
  testing means either duplicating source files into the test build or driving
  the program through its own stdin.
- The separation forces the engine interface to be an actual interface. Any
  temptation to print to `std::cout` from inside the database shows up
  immediately as a layering violation.

### Errors as values, not exceptions

`Result` and `StatusCode` carry the outcome of every database operation. A
missing key, an oversized value and a malformed command are ordinary, expected
outcomes; a caller must handle them, so they are return values, and
`[[nodiscard]]` stops them being ignored silently.

Exceptions are kept for two cases:

1. **Programming errors** — calling `Result::value()` on a failed result, or
   `Result::failure(StatusCode::Ok)`. These indicate a bug in MiniDB, not a
   condition a caller can recover from, so they throw `std::logic_error`.
2. **Genuinely exceptional failures** — allocation failure and the like, which
   propagate to the top-level handler in `main`.

`std::expected` would be the modern alternative, but it is C++23 and would rule
out compilers MiniDB otherwise supports.

### Why an in-repo test framework

GoogleTest and Catch2 are the obvious choices, and either would be defensible.
MiniDB uses a roughly 200-line header instead, for three reasons:

- **No dependency at all.** The project configures and builds with nothing but a
  compiler and CMake — no network access at configure time, no vendored sources,
  no submodule to forget to initialise.
- **CTest already does the hard part.** Discovery, parallel execution, timeouts,
  filtering and failure reporting come from CTest. The header only has to supply
  registration and assertion macros.
- **It stays readable.** The whole framework fits on two screens and uses the
  same C++ the rest of the project does.

The trade-off is real and worth stating: there are no fixtures, no parameterised
tests, no mocking and no death tests. If the suite ever needs those, swapping in
GoogleTest is a contained change — the assertion macro names were chosen to match
its spelling, so the test bodies would largely survive.

### Warning policy

Warnings are configured through an `INTERFACE` target, `minidb_warnings`, that
every MiniDB target links. This keeps the flags in one place and stops them
leaking to anything that might later consume the library.

Beyond `-Wall -Wextra -Wpedantic`, two additions are load-bearing for a database:

- **`-Wconversion` / `-Wsign-conversion`** — MiniDB reads lengths and offsets out
  of files. A silent narrowing conversion between a 64-bit length on disk and a
  32-bit value in memory is exactly the bug class that turns a corrupt file into
  a buffer overflow.
- **`-Wshadow`** — a local variable quietly hiding an outer one is almost never
  intentional.

`MINIDB_WARNINGS_AS_ERRORS` defaults to `OFF` locally and is set to `ON` in CI.
A warning must never reach `main`, but it should not block someone halfway
through an edit on their own machine.

### Limits arrive with the code that enforces them

The project requires bounds on key, value and record size, because MiniDB will
read lengths out of files it did not write and a corrupt snapshot claiming a
4 GiB key must be rejected rather than handed to an allocator.

Those constants are deliberately *not* in Milestone 0. An earlier draft defined
a `limits` namespace up front, and it was removed for two reasons: nothing
referenced it, and `kMaxRecordSize` was defined as key + value + 64, where the
64 stood in for the header of a file format that does not exist yet. That is a
magic number justified by nothing.

Each limit is introduced in the milestone that checks it. Milestone 1 added
`kMaxKeySize` and `kMaxValueSize` alongside the `SET` path that enforces them.
Milestone 3 added `kMaxRecordCount` and `kMaxSnapshotSize` with the snapshot
reader that validates against them. The write-ahead log adds its own record
bound in Milestone 4. In every case the `CorruptData` status code was already
in place, so the checks slotted in without redesigning the error model.

### One test executable per test file

Each file in `tests/` becomes its own binary and its own CTest entry. A single
combined binary would link marginally faster; the per-file split means a segfault
or a hang in one area of the engine reports as one failing test rather than
losing the results of everything else, and CTest can run the files in parallel.

### Directories created when they have content

`benchmarks/` and `examples/` are part of the planned layout but do not exist
yet. Git does not track empty directories, and a directory holding only a
`.gitkeep` communicates nothing. Each is created in the milestone that gives it
content — benchmarks in Milestone 9, examples alongside the finished CLI.

---

## Milestone 1 — Basic database and CLI

### Why `std::unordered_map`, when the whole point is to write a hash table

Milestone 2 replaces the map with a hand-written hash table. Using the standard
one first is deliberate, not a shortcut:

- **It separates two kinds of bug.** When the custom table lands, any failure in
  the suite is a bug in the table, because everything around it already passed
  the same tests against a known-good container. Writing the table and the
  database together would leave every failure ambiguous.
- **It fixes the interface first.** `Database` exposes `set`/`get`/`remove`/
  `exists`/`keys`/`clear` and returns `Result`. None of that mentions buckets,
  load factors or collisions, so swapping the container underneath is a change
  to one private member and one `.cpp` file — the CLI and the tests do not move.
- **It gives Milestone 9 an honest baseline.** The benchmark question is "how
  does my table compare to `std::unordered_map`?" Having run the real workload
  through the standard container first makes that comparison meaningful rather
  than theoretical.

The cost is that Milestone 1 demonstrates no original data-structure work. That
is the correct trade: a working database with a stable interface is the platform
everything else is built on.

### The parser returns `std::variant<Command, Result>`

A parse either produces a command or fails; it never does both, and there is no
meaningful "partial command". `std::variant` states that in the type system, so
a caller physically cannot read a `Command` out of a failed parse.

The alternatives were each worse. A `Command` carrying an `is_valid` flag lets a
caller forget to check it. An out-parameter (`bool parse(line, Command&)`) makes
the success value look optional at every call site. `std::expected` is the
natural fit but is C++23, which would rule out compilers MiniDB supports.

The failure side is the same `Result` the database returns, so the CLI renders
one error model rather than two.

### The parser is a free function, not a class

`parse_command` holds no state between calls. Wrapping it in a `CommandParser`
class would add a constructor, an object to pass around and a header full of
nothing, in exchange for no invariant worth protecting. A namespaced free
function is the honest shape for a pure transformation.

### `SET` takes the rest of the line; everything else takes tokens

`SET name Aarya Lalan` has to store `"Aarya Lalan"`, so `SET` treats everything
after the key as the value, trimming the ends and preserving the interior. Every
other command takes whitespace-separated tokens.

Two consequences, both accepted:

- **Keys cannot contain spaces.** Quoting would fix it, and quoting brings escape
  sequences, unterminated quotes and their error messages. That complexity buys
  little for a key-value store whose keys are identifiers.
- **The CLI cannot store an empty value.** `SET k` is reported as a missing
  value rather than as a request to store `""`. The `Database::set` API accepts
  an empty value, and a test covers it; only the text syntax cannot express it.

### Commands that take no arguments reject extra ones

`KEYS extra` is an error, not a `KEYS`. Silently ignoring trailing input hides
typos, and a database that quietly does something adjacent to what was asked is
worse than one that refuses.

### `Database::keys()` does not sort

Sorting would make it O(n log n) for every caller, including ones that only
count keys or look for one. The CLI sorts what it prints, because a stable
alphabetical listing is a presentation decision. The engine stays O(n).

### Transparent hashing

`Database` uses `std::unordered_map<Key, Value, StringHash, std::equal_to<>>`
where `StringHash` declares `is_transparent`. That opts into C++20 heterogeneous
lookup, letting `find()` take a `std::string_view` directly.

Without it, every `get`, `exists` and `remove` would construct a temporary
`std::string` purely to be thrown away after the lookup — an allocation on the
hottest path in the program. Eight lines to delete an allocation per read is a
good trade, and it is one of the concrete reasons this project targets C++20.

### `remove` on a missing key is `NotFound`, not success

Deleting something that was not there is a different outcome from deleting
something that was, and the caller is entitled to know which happened. Returning
`Ok` for both would make `DELETE` untestable at the boundary that matters.

### A lifetime bug the warning set caught

Milestone 1 was the first code to write `EXPECT_EQ(database.get(k).value(), ...)`,
and `-Wdangling-reference` rejected it immediately.

The Milestone 0 `EXPECT_EQ` began with `const auto& actual_value = (actual);`.
That statement is its own full-expression, so the temporary `Result` returned by
`get()` was destroyed at its semicolon, leaving the reference dangling before
the comparison ran. The macro now passes both operands to a function template,
`equal_or_describe`, because arguments keep temporaries alive for the duration
of the call — and each operand is still evaluated exactly once.

This is the clearest argument so far for the strict warning set: the bug was
invisible in review, harmless in Milestone 0 where every operand was a named
variable, and would have produced unpredictable failures the moment the tests
grew up.

---

## Milestone 2 — Custom hash table

### Why the standard container went in first, and why it comes out now

Milestone 1 used `std::unordered_map` deliberately: it fixed the `Database`
interface against a container already known to be correct, so that the parser,
the CLI and the database tests were all proven before any hand-written data
structure existed.

That paid off exactly as intended. When `HashTable` replaced the map, the
change touched one private member in `database.hpp` and five lines of
`database.cpp`, and all 61 existing tests passed unchanged. Any failure could
only have come from the new table.

The map comes out now because this project exists to demonstrate understanding
of the mechanism, and a hash table is where hashing, collision resolution,
linked structures, amortised growth and manual ownership all meet. For a real
product, keeping `std::unordered_map` would be the correct engineering call —
it is written by specialists and far more heavily optimised than this.

### The hashing pipeline

```text
key ──► Hash{}(key) ──► std::size_t hash ──► hash % bucket_count ──► bucket index
                              │
                              └── cached in the node at insertion
```

Every step is in `HashTable`: `hash_of` applies the hasher, `bucket_index`
takes the remainder, and `Node::hash` stores the value so it never has to be
recomputed.

### Separate chaining, not open addressing

Each bucket owns a singly linked chain of the entries that hashed to it.
Lookup walks the chain comparing keys; a collision costs comparisons, never
correctness.

Open addressing — probing for another free slot — has better cache behaviour
and avoids a node allocation per entry. It was rejected because deletion is
genuinely subtle: removing an entry can break the probe sequence that reaches
entries inserted after it, so real implementations need tombstone markers and
a policy for cleaning them up. `DELETE` is a command MiniDB must get right,
and separate chaining makes it a two-line unlink that is obviously correct.

The cost is honest: one heap allocation per entry, and chains scattered through
memory rather than packed in an array.

### Prime bucket counts

Bucket counts come from a table of primes that roughly double: 17, 37, 79,
163, 331, ... and beyond the table a prime is searched for directly.

The modulus decides which bits of the hash survive. A power-of-two modulus
keeps only the low bits, and libstdc++ defines `std::hash<int>` as the identity
function — so with 16 buckets the keys 0, 16, 32, 48 would every one collide,
from entirely ordinary data. A prime modulus folds all the bits in, so a weak
hash degrades gradually instead of collapsing.

`test_hash_table.cpp` demonstrates this with 500 integer keys that are all
multiples of 16, and asserts no chain exceeds nine nodes.

### Load factor 0.75, growth by doubling

Growth is triggered when inserting one more entry *would* push
`size / bucket_count` past 0.75.

The threshold trades memory against collisions. At 1.0 the average chain is one
node but long chains are already common; at 0.5 lookups are fast but half the
array sits idle. 0.75 keeps the expected chain length below one while leaving a
quarter of the array spare — the same value the JDK settled on for `HashMap`.

Doubling is what keeps insertion amortised O(1). Each rehash is O(n), but it
happens only after the table has doubled, so the cost spread over the
insertions that caused it is constant per insertion. Growing by a fixed
increment would make n insertions cost O(n²) in total.

### Complexity

| Operation | Average | Worst case |
| --- | --- | --- |
| `insert_or_assign` | O(1) | O(n) |
| `find` / `contains` | O(1) | O(n) |
| `erase` | O(1) | O(n) |
| `for_each` | O(n + bucket_count) | O(n + bucket_count) |
| `clear` | O(n) | O(n) |
| `rehash` | O(n) | O(n) |
| Space | O(n + bucket_count) | O(n + bucket_count) |

The averages are conditional, not guaranteed. They require the hash to spread
keys evenly and the load factor to stay bounded. MiniDB enforces the second
itself and delegates the first to `std::hash`. The worst case — every key in
one bucket, turning the table into a linked list — is reproduced deliberately
in the tests rather than assumed impossible.

### Reference stability across rehash

Rehashing relinks existing nodes into the new bucket array instead of
recreating them, so entries never move in memory. A pointer returned by
`find()` therefore survives a rehash, and is invalidated only by erasing that
entry or clearing the table. This is stronger than what a `std::vector`-based
chain would give, where every growth would move every entry.

### Iterative teardown

Freeing a chain by destroying its head `unique_ptr` recurses once per node.
With 20,000 colliding entries that exhausts the stack — precisely the case
separate chaining exists to survive. `clear()` unlinks each node before
releasing it, keeping the recursion depth at one, and a test builds a
20,000-node chain to prove it.

This is the sharp edge of `unique_ptr`-based linked structures, and the reason
the destructor is written by hand rather than defaulted.

### `for_each` instead of an iterator type

A conforming forward iterator would need to skip empty buckets, compare
correctly across buckets, and satisfy the iterator requirements — a few hundred
lines of boilerplate. The only traversal MiniDB performs is `KEYS`. `for_each`
supplies exactly that and nothing more.

The trade-off: the table cannot be used with range-based `for` or standard
algorithms. If a later milestone needs those, an iterator can be added without
disturbing anything already written.

### Known trade-offs

- **One allocation per entry.** Node-based chaining allocates for every insert.
  A `std::vector` per bucket would allocate less often but would invalidate
  references on growth.
- **Insertion into a long chain is quadratic overall.** Each insert walks the
  chain to check for a duplicate. With a sound hash the chain is one or two
  nodes; under a pathological hash it is the documented O(n) worst case, and
  the 20,000-entry test pays it in full.
- **No `reserve`.** A caller who knows the final size cannot preallocate and
  skip the intermediate rehashes. `rehash(n)` is public and does the job
  manually.
- **`std::hash` is not collision-resistant.** It is not meant to be. MiniDB is
  a local, single-process database with no untrusted input, so hash-flooding is
  out of scope; a network-facing store would need a seeded or keyed hash.

---

## Milestone 3 — Persistent storage

### Snapshots before a log

There are two ways to make a database durable: write the whole thing out
periodically (a snapshot), or append every change to a log as it happens (a
write-ahead log). MiniDB does snapshots first because they are the simpler
half, and because they make the log's purpose obvious.

A snapshot is a complete, self-describing file. Loading it is one pass with no
replay and no reconciliation, and the code is short enough to read in one
sitting. Its weakness is equally clear: nothing on disk records a change until
the next save, so everything since the last one is lost if the process dies.
Milestone 4 adds the log precisely to close that window, and arriving at it
after feeling the gap is better than being handed both at once.

### Where the record type lives

`StorageManager` deals in `Record` — a plain key and value — not in
`HashTable` or `Database`. That keeps the dependency one-directional:
serialisation knows nothing about how entries are held in memory, and the hash
table knows nothing about bytes on disk. `Database` is the only place that
knows both, which is what makes it a coordinator rather than a wrapper.

The cost is one copy of every key and value into a vector on save — O(n)
memory on top of the database itself. That is documented rather than optimised
away: a callback-based writer would avoid the vector at the price of a more
tangled interface, and n here is bounded by a 256 MiB file.

### Little-endian by hand, not by memcpy

Integers are encoded one byte at a time rather than by copying the bytes of an
in-memory value. Writing `out.write(reinterpret_cast<const char*>(&count), 8)`
would be shorter, and would produce a file whose meaning depends on the CPU
that wrote it — a snapshot written on a big-endian machine would silently
misread on a little-endian one.

Doing the shifts explicitly costs a few lines, makes the format independent of
the architecture, and means there is no `reinterpret_cast` anywhere in the
reader or the writer. It also makes the documented layout literally true: what
`docs/STORAGE_FORMAT.md` says sits at offset 12 is what the code puts there.

### Why there is a checksum

The original plan had none. The magic number catches a file that is not a
snapshot, the version catches an incompatible one, and the length checks catch
a structurally broken one — which covers everything except the case that
actually matters for a database: a single flipped bit inside a key or a value.
That file passes every structural check and loads as plausible, wrong data.

Since "never silently accept corrupted data" is the entire point of validating
the file, a CRC-32 over the payload earns its thirty lines. It is computed
without a lookup table because the arithmetic is nothing next to the I/O, and
the writer patches it into the header by seeking back after the payload is
written — the alternatives being to encode every record twice or to hold the
whole encoded file in memory.

A CRC is an integrity check, not a security measure. It detects accidental
damage; it does not detect deliberate tampering, because anyone who edits the
file can recompute it.

### Validation order, and why lengths are checked before use

Checks run cheapest-first, and every length is validated before it reaches an
allocator. The two that matter most:

- **The record count** is compared as `count > payload_bytes / 9`, not
  `count * 9 > payload_bytes`. Dividing cannot overflow; multiplying a corrupt
  count of 2^64-1 would wrap to a small number and sail through.
- **Record lengths** are 32-bit and their sum is computed as 64-bit, so it
  cannot wrap, and it is compared against the bytes actually remaining in the
  file before either string is resized.

Getting this backwards is how a corrupt file becomes a crash or a
multi-gigabyte allocation. The tests include a record declaring two
`0xFFFFFFFF` lengths for exactly that reason.

### Duplicate keys are corruption, not a merge

A snapshot containing the same key twice cannot have come from `save()`,
because the database holds each key once. Such a file has been altered or
damaged. Resolving it by keeping the last record would be silent data loss, so
it is refused instead. Detection uses MiniDB's own `HashTable` as a seen-set,
which keeps loading O(n) on average rather than the O(n log n) a sort costs.

### Records are published only on success

`load()` parses into a local vector and moves it into the caller's only once
every check has passed. The first version pushed straight into the caller's
vector, and the tests caught it immediately: a file rejected halfway through
left the caller holding a partial snapshot. The status code was correct, but a
caller who checked it loosely would have had half a database.

Building the result separately makes the guarantee structural, rather than
something each of a dozen early returns has to remember.

### Atomic replacement, and what it does not buy

`save()` writes to `<database>.tmp` and renames it over the database. The
rename is one filesystem operation, so a reader sees either the whole old file
or the whole new one. The scratch file sits beside the database rather than in
the system temporary directory, because replacement is only atomic within one
filesystem and the temp directory is frequently a different volume. An RAII
guard removes the scratch file on every failure path, including an exception.

What this does **not** provide is durability against power loss. MiniDB
flushes to the operating system but does not `fsync` the file or its
directory, so after `save()` returns the data may still sit in the page cache.
Calling this "crash-safe" would be a lie; it is *atomic*, which is a different
and weaker promise. Durability needs an explicit device flush, and that
discussion belongs with the write-ahead log.

### A corrupt database stops the CLI

When `load()` fails, the CLI prints the reason and exits non-zero rather than
starting empty. Continuing would be actively destructive: the session would
end with a save, and that save would overwrite the damaged file with an empty
snapshot, destroying whatever might have been recovered from it.

Leaving the file untouched and telling the user to move it aside is less
convenient and much easier to defend.

### Saving on exit, not on every write

Every mutation could trigger a save. That would make each `SET` O(n) in the
size of the whole database, which is indefensible for a change to one key.

At Milestone 3 the CLI saved once, when the session ended normally. The consequence was
stated plainly in the README and in the format document: a killed process
loses its session. This is the honest shape of snapshot-only persistence, and
pretending otherwise would misrepresent what Milestone 4 is for.

### The default database location comes from the environment

The path is built from `%LOCALAPPDATA%` on Windows and `$XDG_DATA_HOME` or
`$HOME` elsewhere, falling back to the current directory when the environment
says nothing. No absolute path is compiled in, and the default is deliberately
outside the source tree so that running MiniDB never writes into the
repository. The CLI also accepts a path argument, which is what makes manual
testing possible without touching the user's real database.

### A shared temp-directory helper for tests

`tests/temp_directory.hpp` gives both the storage and database tests a unique
scratch directory that deletes itself. It is RAII rather than a cleanup call at
the end of each test, because a failing assertion leaves the case early and a
destructor still runs where a trailing cleanup line would be skipped.

Every path comes from `std::filesystem::temp_directory_path()`, so no test
writes into the repository, touches the user's real database, or depends on a
hard-coded path.

### Known trade-offs

- **Whole-file saves.** Changing one key rewrites everything. Incremental
  update is what the log is for.
- **No `fsync`.** Atomic, not durable. Stated everywhere it could mislead.
- **No file locking.** Two processes on one database will overwrite each other.
- **Keys are limited to 1 KiB; values to 1 MiB.** Limits live in `types.hpp`.
- **Duplicate detection costs memory.** The seen-set holds every key a second
  time during load. An O(1)-memory alternative would mean sorting, which costs
  time instead.

## Milestone 4: write-ahead logging

The existing Database/StorageManager/HashTable separation is retained.
WriteAheadLog owns append, replay, tail repair, and reset. Binary little-endian
and CRC helpers are shared privately through src/binary_format.*; snapshot
version 1 is unchanged. See [WAL.md](WAL.md) for the byte layout and policies.

Every SET/DELETE is logged and stream-flushed before memory changes. CLEAR is
one additional operation because the existing public API must also recover
correctly. Database::clear now returns Result so callers can handle disk
errors; callers that ignore its return continue to compile. A persistent
Database lazily recovers before its first mutation or save, preventing a
fresh object from overwriting an existing snapshot with an empty table.
Explicit load remains necessary before read-only access to existing data.

Failed append, failed reset, failed load, or an exception applying a logged SET
forces recovery before later writes/saves. This prevents a save from discarding
an uncertain durable operation. There is no rollback: an operation reported
as failed may appear after recovery if its complete log record reached the OS.

Snapshots are installed before resetting the WAL. Replaying a leftover complete
log after snapshot replacement has the same final state for absolute SET,
DELETE, and CLEAR; no sequence checkpoint in the snapshot is needed for this
restricted operation set. The recovered temporary table is published only on
success. Increment operations or transactions would require a different design.

Incomplete final records are trimmed only after the valid prefix is checked;
complete corruption is rejected. Plausible damaged lengths extending past EOF
and short junk tails remain ambiguous and are treated as incomplete writes.
Append and snapshot save enforce the same file-size caps as their readers,
so successful writes cannot create an oversized file recovery would reject.

Durability is a checked stream flush and close to the OS. There is no device or
directory sync, power-loss guarantee, file locking, transaction guarantee, or
future milestone implementation. Opening per append and retaining decoded WAL
records during replay favor readable lifecycle management over performance.
