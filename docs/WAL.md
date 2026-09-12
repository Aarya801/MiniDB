# Write-Ahead Log (Milestone 4)

MiniDB keeps `<snapshot>` and `<snapshot>.wal`. The snapshot format remains
version 1. The WAL records SET, DELETE, and the existing CLEAR operation.
This is an educational, single-owner database; it does not provide full ACID.

## Binary format, version 1

There is no file header: an empty WAL is zero bytes. Records are concatenated.
All integers are unsigned little-endian, encoded explicitly rather than by
writing C++ structs. Keys and values are opaque bytes, including embedded NULs.

| Offset | Bytes | Field |
| --- | --- | --- |
| 0 | 4 | Magic: ASCII `MWAL` |
| 4 | 2 | Version: 1 |
| 6 | 2 | Operation: SET=1, DELETE=2, CLEAR=3 |
| 8 | 8 | Sequence: starts at 1, increases by exactly 1 |
| 16 | 4 | Key length |
| 20 | 4 | Value length |
| 24 | 4 | CRC-32 |
| 28 | key length | Key bytes |
| 28 + key length | value length | Value bytes |

Total record size is `28 + key_length + value_length`; no terminators or
padding. CRC-32 uses reflected IEEE polynomial `0xEDB88320`, initial state
`0xFFFFFFFF`, and final XOR `0xFFFFFFFF`. It covers header bytes 0–23 followed
by key and value, excluding the checksum field. CRC detects accidental damage;
it is not authentication and collisions are possible.

SET requires a key of 1–1024 bytes and a value of 0–1048576 bytes. DELETE
requires a key of 1–1024 bytes and zero value bytes. CLEAR requires both
lengths to be zero. WAL files are limited to 256 MiB on both append and replay.
Append refuses to exceed that limit; save a snapshot to reset the WAL.
Sequences restart at 1 after reset and cannot wrap.

## Write path and durability

1. Validate the request. Before the first persistent mutation or save, recover
   existing files if `load()` has not already succeeded.
2. Encode and append the entire record to the WAL.
3. Flush the C++ stream to the OS, explicitly close it, and check stream state.
4. Only on success, apply the mutation to the in-memory hash table.

A missing-key DELETE returns NotFound without logging. CLEAR uses one record
so recovery cannot restore entries cleared by the existing API. The in-memory
Database constructor continues to perform no disk operations.

Successful writes are intended to survive termination of this process while
the OS and filesystem remain healthy. There is **no fsync, FlushFileBuffers,
or directory sync**, so OS crashes, power loss, device failures, and filesystem
write reordering are outside the guarantee. Flushing a stream is not a device
barrier. WAL writes are not assumed atomic.

A write/flush/close error leaves the WAL outcome uncertain: a complete record
may have reached the OS even though the call failed. Memory is not changed by
an append error; later recovery may apply that record. Further Database writes
or saves first recover again. A memory-allocation exception after logging also
forces recovery before further writes or saves. Exceptions can propagate to
the caller; the CLI catches them and exits. There is no rollback or exactly-once
request protocol. Read-only calls show the current in-memory state; call load()
to refresh after an error. A direct WriteAheadLog user must replay an unknown
or failed tail before appending, or reset only after installing its snapshot.

## Recovery

`Database::load()` reads the snapshot into a temporary table, then replays the
WAL directly into that table without logging again. It publishes the recovered
table only on success. Missing snapshot means start from an empty table;
missing WAL means no additional mutations. Both missing is a normal first run.
A present empty WAL is valid. A corrupt snapshot is refused even if the WAL
appears valid; there is no automatic search for older snapshot files.

Replay checks file size, magic, version, operation, sequence, operation-specific
length limits, available payload bytes, and CRC. Length sums are widened to
64 bits before addition, remaining bytes are compared before allocation, and
append uses subtraction to check the total file limit without overflow.
Unexpected read errors after obtaining the file size return IoError and never
trigger destructive tail repair.

### Incomplete final record

Fewer than 28 trailing bytes, or an otherwise valid complete header whose
bounded payload extends beyond EOF, is treated as an interrupted append.
After validating all preceding records, replay closes the reader and truncates
the WAL to the last complete record boundary. Only then may appends continue.
An incomplete first record produces an empty WAL. `discarded_tail_bytes()`
reports the removed bytes to direct WAL callers. Failure to truncate fails
recovery and leaves the Database's current table unchanged.

This policy cannot distinguish a torn append from arbitrary short trailing
junk, or from a corrupted but plausible final length that extends beyond EOF.
Those bytes are discarded, so this is deliberate salvage with a documented
ambiguity, not detection of all corruption. Complete bad headers, impossible
lengths, wrong sequences, unknown operations, and complete checksum failures
are rejected, including in the final record. Unsupported versions return
UnsupportedVersion; other format damage returns CorruptData. Rejected files
are preserved and no partially parsed record vector is returned.

## Snapshot interaction

Save first serializes and validates the complete current state, writes the
snapshot to its sibling `.tmp` file, flushes and explicitly closes it, and
renames it over the snapshot. Only after successful replacement does it
truncate the WAL to zero bytes and reset sequence numbering.

A failed snapshot leaves the WAL untouched. If WAL reset fails, save reports
the failure even though the snapshot has been installed, and forces recovery
before another mutation or save. If the process stops between snapshot
replacement and reset, recovery may replay the old WAL over the new snapshot.
This is safe for this operation set: the entire ordered sequence of absolute
SET, DELETE, and CLEAR operations is idempotent. Intermediate replay state is
never published. After a successful save the WAL is empty, so no old operations
are replayed on reopening. This reasoning would not apply to an increment
operation; adding one would require a checkpoint/generation scheme.

The two files have no shared generation identifier. Do not mix snapshots and
WALs from different databases or backup times, delete one to repair the other,
or modify them while MiniDB is using them. Copy the pair while MiniDB is stopped.
Power loss can persist the truncation before the replacement snapshot; no
power-loss consistency is claimed.

## Costs and scope

Append costs O(key bytes + value bytes), plus an open, write, flush, and close.
Replay holds decoded records and a recovered table in memory; its cost depends
on total WAL bytes and operations. The 256 MiB disk cap does not imply a 256 MiB
RAM cap: record objects, strings, and hash-table nodes add overhead. Snapshots
remain whole-file saves. There is no file locking, concurrent writer support,
transaction protocol, background checkpointing, log rotation, or production
storage guarantee. Milestone 5 and later are not implemented here.
