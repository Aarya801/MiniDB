# Concurrency and Thread Safety (Milestone 8)

MiniDB makes ordinary operations on one `Database` safe to call from multiple
threads. The design favors a small, reviewable lock protocol over maximum
parallelism.

## Protected state

`Database::state_mutex_` is a `std::shared_mutex`. It protects:

- the authoritative HashTable and every traversal of it
- snapshot/WAL objects and WAL sequence state
- load/save/recovery publication
- `loaded_` and the replayed-operation count
- transaction commit publication

Shared locks are used by GET after key validation, EXISTS, KEYS, size, empty,
and read-only persistence metadata access. SET, DELETE, CLEAR, load/open, save,
lazy recovery, and transaction commit use a `std::unique_lock`. WAL append,
snapshot replacement, WAL reset, and recovered-table publication therefore
cannot overlap another operation on that Database.

`Database::writer_mutex_` serializes attempts to acquire the exclusive state
lock. Writers were already mutually exclusive, so this does not reduce their
parallelism; it avoids relying on several threads entering a platform
reader/writer-lock writer queue simultaneously and gives writer acquisition one
simple entry point.

`Database::cache_mutex_` is a `std::mutex` protecting every LRU lookup,
promotion, insertion, invalidation, clear, and diagnostic. A GET holds a shared
state lock while it checks or populates the cache, so a writer cannot change
the authoritative value between the table lookup and cache insertion.

Low-level `HashTable`, `LruCache`, `StorageManager`, and `WriteAheadLog`
instances do not synchronize themselves. Database supplies their
synchronization. Callers using those types directly must prevent conflicting
access, like callers of standard-library containers.

## Lock order and deadlocks

The fixed order is:

```text
Transaction mutex -> Database writer mutex -> Database state lock -> Database cache mutex
```

Database never calls back into Transaction. No operation takes the cache mutex
and then requests the state lock. Copy and move operations snapshot the source
before locking the destination, so they never hold locks from two Database
objects at once. These rules remove lock cycles from the current architecture.

Object destruction and assignment require exclusive ownership from the caller.
The path accessors return references whose lifetime still depends on the
Database object; using one while assigning, moving, or destroying that object
is invalid.

## Transactions

Every Transaction method locks that object's `std::mutex`, protecting its
active flag and local overlay. Calls from multiple threads are serialized in
the order they acquire that mutex. COMMIT holds it while Database takes its
exclusive state lock, writes the one transaction WAL record, and publishes the
staged table. ROLLBACK only clears the protected overlay.

Independent Transaction objects bound to one Database are allowed. Their local
overlays are independent and commits are serialized, so disjoint committed
changes are not lost. This is not transaction isolation: there is no snapshot
captured at BEGIN, no conflict detection, and no repeatable-read guarantee.
Direct Database writes may occur while a transaction is active, and the
transaction's next fallback read can observe them. Last committed SET/DELETE
on the same key determines the visible value.

## Contention and scope

Read-only HashTable work can overlap, but cache operations briefly serialize.
Every writer excludes readers and other writers. File I/O, recovery, whole-file
snapshot save, and O(n) transaction table staging happen while the exclusive
lock is held. A slow disk or large database can therefore pause every other
thread. No fairness or starvation bound is promised by `std::shared_mutex`.

The locks are per object. Two Database instances using the same snapshot/WAL
paths do not share a lock and can corrupt each other's file protocol. There is
no file locking, multi-process coordination, distributed transaction support,
MVCC, lock-free access, asynchronous I/O, or claim of high scalability.
Physical power-loss durability and uncertain WAL write outcomes retain the
limitations documented in [WAL.md](WAL.md).

Concurrency tests use C++20 latches to start workers at a defined phase and
join every worker before assertions. They avoid sleeps and timing thresholds.
