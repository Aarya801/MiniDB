# Transactions (Milestone 7)

MiniDB supports one educational transaction coordinator over one `Database`.
The lifecycle is:

```text
inactive --BEGIN--> active --COMMIT--> inactive
                           --ROLLBACK--> inactive
```

BEGIN while active, or COMMIT/ROLLBACK while inactive, returns
`INVALID_TRANSACTION_STATE`. A failed SET/DELETE leaves the transaction active.
A COMMIT attempt always ends it because an append error can have an uncertain
durable outcome. The caller must reopen and recover before deciding whether a
complete record reached the WAL.

## Local state and visibility

`Transaction` stores an `unordered_map<Key, optional<Value>>`. A value is a
local SET; `nullopt` is a DELETE tombstone. GET checks this overlay first, then
uses `Database::get`, giving read-your-writes behavior without modifying the
authoritative table. Repeated writes to a key replace its overlay entry, so
commit records only its final effect. KEYS, EXISTS, and CLEAR operate on the
same local view in the CLI.

ROLLBACK clears the overlay. Since no local mutation was written to the main
table, snapshot, WAL, or cache, there is no durable state to undo.

## Commit, WAL, and recovery

COMMIT sorts the final per-key mutations for deterministic encoding. Database
first copies its hash table and applies the changes to that copy. It then
encodes the changes in one checksummed TRANSACTION WAL record, flushes and
closes the stream, and publishes the prepared table only after success.

On restart, WAL replay validates the entire outer record and every inner
mutation before returning any of them. A crash that leaves an incomplete final
record causes the whole record to be trimmed. A complete invalid record stops
recovery. This makes a valid transaction logically all-or-nothing in memory and
on replay. It does not make an OS stream flush a physical disk barrier. Power
loss, filesystem reordering, device failure, and an I/O error with an uncertain
write outcome remain outside the guarantee. MiniDB does not claim full ACID.

An empty transaction commits without a WAL record. A nonempty transaction WAL
payload is limited to 1 MiB; individual key/value and total WAL limits still
apply. Saving a snapshot includes committed state and resets the WAL using the
existing checkpoint order.

## Cache behavior

Local writes do not update the main LRU cache. Reads of unchanged database
values can use and populate it. Successful commit publishes the replacement
table and clears the cache, preventing stale entries. Rollback does not touch
the cache because its contents still match the database.

## Complexity and limits

For `t` distinct changed keys, overlay GET/SET/DELETE are average O(1), subject
to hashing and string size. ROLLBACK destroys O(t) stored state. COMMIT sorts in
O(t log t), copies O(n) database entries for strong in-memory publication, and
encodes/writes O(transaction bytes). The overlay uses O(t) memory and the staged
commit table uses O(n) additional memory.

There is no MVCC, locking, nested transaction, advanced isolation level,
savepoint, multi-process coordination, concurrent transaction guarantee,
distributed recovery, or exactly-once client protocol. Direct library callers
must coordinate access to the bound `Database`; concurrency is deferred to
Milestone 8.
