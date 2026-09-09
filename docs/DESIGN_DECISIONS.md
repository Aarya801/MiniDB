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

Each limit is introduced in the milestone that checks it -- key and value bounds
with the `SET` path, record bounds with the binary format. The `KeyTooLarge`,
`ValueTooLarge` and `CorruptData` status codes already exist, so the error
vocabulary is in place and the checks slot in without redesign.

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
