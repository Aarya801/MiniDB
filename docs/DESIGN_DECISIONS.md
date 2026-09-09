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
- **Designated initialisers and `std::span`** — expected to be useful once the
  binary storage format lands in Milestone 3.

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
`kMaxKeySize` and `kMaxValueSize` alongside the `SET` path that enforces them;
the record and log bounds arrive with the binary format in Milestone 3. The
`CorruptData` status code already exists, so the error vocabulary is in place
and those checks will slot in without redesign.

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
