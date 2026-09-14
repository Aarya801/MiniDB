# Benchmarks (Milestone 9)

MiniDB includes a small standalone benchmark for observing the cost of its
public operations and core hash table. It is an educational measurement tool,
not a claim that MiniDB competes with a production database.

## Methodology

The benchmark uses `std::chrono::steady_clock` and reports the median of five
measured trials. Each trial performs exactly 1,000, 10,000, or 100,000 timed
operations. Key/value generation and workload setup happen outside the timed
region. Average latency is the median elapsed time divided by the operation
count; it is not a percentile or a sample of individual operation latency.

Database workloads use an in-memory `Database`, so WAL and snapshot I/O are
excluded:

- **SET:** insert distinct generated string keys with caching disabled.
- **GET (no cache):** look up every preloaded key once with caching disabled.
- **DELETE:** remove every preloaded key.
- **Mixed SET/GET:** alternate a SET with a GET of the key just inserted, for
  an approximately 50/50 mix. Caching is disabled.
- **GET (cache hit):** repeatedly read a warmed 128-key working set from a
  cache with capacity 128.
- **Hash table comparison:** insert or look up the same generated strings in
  MiniDB's `HashTable` and `std::unordered_map`. Neither table is pre-reserved
  for the insertion measurement. Lookup setup is untimed.

The process is single-threaded. Keys are sequential strings such as `key-42`,
values are sequential strings such as `value-42`, and successful results are
consumed so the timed operations remain observable to the optimizer.

Build and run the Release benchmark with:

```bash
cmake -S . -B build-bench -DCMAKE_BUILD_TYPE=Release
cmake --build build-bench --target minidb_benchmark
./build-bench/minidb_benchmark
```

Set `MINIDB_BUILD_BENCHMARKS=OFF` when the target is not wanted. The executable
is deliberately not registered as a CTest test, so timing work does not affect
the correctness suite.

## Measured environment

Results below were recorded on 2026-09-14 with:

- Windows 25H2, build 26200, x64
- 12th Gen Intel Core i5-12450HX, 12 logical processors
- 15.7 GiB physical memory
- MinGW-w64 GCC 16.1.0
- CMake 4.4.3 with `CMAKE_BUILD_TYPE=Release`
- MiniDB commit `7baa2c5` plus the uncommitted Milestone 9 benchmark changes

Background applications, CPU frequency, temperature, power policy, and system
load were not controlled.

## Measured results

These are the actual median values printed by `minidb_benchmark` on the
environment above.

| Workload | Operations | Elapsed ms | Operations/second | Average ns/op |
| --- | ---: | ---: | ---: | ---: |
| Database SET | 1,000 | 0.209 | 4,786,979 | 208.9 |
| Database GET (no cache) | 1,000 | 0.142 | 7,022,472 | 142.4 |
| Database DELETE | 1,000 | 0.169 | 5,903,188 | 169.4 |
| Database mixed SET/GET | 1,000 | 0.177 | 5,659,310 | 176.7 |
| Database GET (cache hit) | 1,000 | 0.156 | 6,410,256 | 156.0 |
| HashTable insert | 1,000 | 0.084 | 11,976,048 | 83.5 |
| `std::unordered_map` insert | 1,000 | 0.078 | 12,804,097 | 78.1 |
| HashTable lookup | 1,000 | 0.017 | 59,880,240 | 16.7 |
| `std::unordered_map` lookup | 1,000 | 0.018 | 55,865,922 | 17.9 |
| Database SET | 10,000 | 2.236 | 4,471,472 | 223.6 |
| Database GET (no cache) | 10,000 | 1.500 | 6,667,111 | 150.0 |
| Database DELETE | 10,000 | 1.818 | 5,500,248 | 181.8 |
| Database mixed SET/GET | 10,000 | 1.987 | 5,032,459 | 198.7 |
| Database GET (cache hit) | 10,000 | 1.496 | 6,682,259 | 149.7 |
| HashTable insert | 10,000 | 1.156 | 8,653,513 | 115.6 |
| `std::unordered_map` insert | 10,000 | 0.942 | 10,620,221 | 94.2 |
| HashTable lookup | 10,000 | 0.168 | 59,630,292 | 16.8 |
| `std::unordered_map` lookup | 10,000 | 0.223 | 44,883,303 | 22.3 |
| Database SET | 100,000 | 31.415 | 3,183,193 | 314.1 |
| Database GET (no cache) | 100,000 | 22.486 | 4,447,172 | 224.9 |
| Database DELETE | 100,000 | 25.140 | 3,977,741 | 251.4 |
| Database mixed SET/GET | 100,000 | 21.264 | 4,702,895 | 212.6 |
| Database GET (cache hit) | 100,000 | 15.683 | 6,376,250 | 156.8 |
| HashTable insert | 100,000 | 16.406 | 6,095,257 | 164.1 |
| `std::unordered_map` insert | 100,000 | 26.280 | 3,805,132 | 262.8 |
| HashTable lookup | 100,000 | 4.382 | 22,822,713 | 43.8 |
| `std::unordered_map` lookup | 100,000 | 7.017 | 14,250,292 | 70.2 |

## Interpretation

The Database rows include validation, synchronization, `Result` construction,
and string copying in addition to hash-table work. The raw table rows omit
those layers, so they are useful for inspecting the container but are not a
substitute for the public API measurements.

The warmed cache workload was faster than the cache-disabled full-data lookup
at 10,000 and 100,000 operations in this run. The workloads have different
access patterns: the cache test repeatedly uses 128 hot keys, while the
cache-disabled test scans every key once. The result therefore does not isolate
the cache as the only cause. MiniDB's cache sits above an in-memory table and
also pays for a mutex, an unordered-map lookup, and LRU list promotion.

The custom table and `std::unordered_map` exchange the lead at different sizes
in these measurements. Their growth policies, bucket counts, allocation
strategies, and implementation details differ. These results describe this
compiler, standard library, data shape, and run; they do not establish that
either container is generally faster.

## Limitations

- Five-trial medians reduce single-run noise but are not a statistical study.
- The benchmark does not pin a CPU, raise process priority, disable frequency
  scaling, or isolate the machine from background work.
- Sequential generated strings are only one key distribution. There are no
  random keys, forced collisions, variable-size values, or skewed write mixes.
- The cache-hit and cache-disabled GET rows use different working sets.
- No WAL, snapshot, recovery, disk, power-loss, or restart cost is measured.
- No concurrent workload or lock-contention scaling is measured.
- Allocation and string-copy costs are included where the public API incurs
  them, but memory use is not reported.
- Small sub-millisecond trials are particularly sensitive to timer resolution,
  scheduling, and CPU state.

Results will vary across machines, operating systems, compiler versions,
standard-library implementations, build types, power modes, and system load.
Re-run the executable locally rather than treating this table as a portable
performance guarantee.
