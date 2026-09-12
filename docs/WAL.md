# Write-Ahead Log and Crash Recovery (Milestones 4–5)

MiniDB keeps `<snapshot>` and `<snapshot>.wal`. Database saves now use a
version-2 snapshot with a checkpoint; version-1 snapshots remain readable.
The WAL layout remains version 1 and records SET, DELETE, and CLEAR.
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
| 8 | 8 | Sequence: monotonic; resumes at checkpoint + 1 after reset |
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
Database sequences continue from the snapshot checkpoint after reset and cannot
wrap. Direct WAL callers pass the checkpoint to replay/reset; omitting it retains
the original standalone behavior (checkpoint zero).

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

`Database::open()` (also available as `load()`) reads the snapshot into a temporary table, then replays the
WAL operations newer than its checkpoint directly into that table without logging again. It publishes the recovered
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
truncate the WAL to zero bytes. The next sequence is checkpoint + 1.

The snapshot checkpoint is the highest sequence represented by its state.
Version 2 includes that number in the snapshot CRC, alongside its header fields
and payload. A crash before snapshot replacement leaves the prior snapshot and
WAL available. A crash after replacement but before reset leaves a checkpoint
that tells recovery which old records to skip. A crash after reset leaves the
snapshot and an empty WAL. No checkpointed operation is applied to the table.

All complete WAL records, including skipped ones, still undergo structural and
CRC validation. The first sequence must be positive and no greater than
checkpoint + 1 (checked without overflowing). Records must then be contiguous;
a jump directly to checkpoint + 1 is also allowed when it skips only operations
already covered by the snapshot. Missing operations newer than the checkpoint,
duplicates, and backwards sequences are refused. Empty/missing WALs resume
from the checkpoint. Skipped records are not included in
`replayed_operation_count()`.

A failed snapshot leaves the WAL untouched. If WAL reset fails, save reports
failure even though the snapshot is installed, and forces recovery before
another mutation or save. The checkpoint makes that recovery skip already
saved mutations. Sequence exhaustion reports an error; saving does not reset
the lifetime sequence space.

### Opening and legacy files

The CLI calls `Database::open()` before accepting commands. Library users can
call open/load to obtain a Result before reading; the existing lazy recovery
before the first mutation/save remains. The constructor does not load records.

A version-1 snapshot has no checkpoint, so recovery treats it as checkpoint
zero and follows Milestone 4 replay semantics. If an old process stopped between
snapshot replacement and WAL reset, it is impossible to identify included
operations from those legacy bytes. That one legacy recovery may reapply them;
absolute SET/DELETE/CLEAR preserve the final state. The next successful save
writes version 2 and establishes the checkpoint. Older MiniDB builds reject
version 2 rather than misreading it. Standalone `StorageManager::save(records)`
retains the version-1 writer; passing a checkpoint writes version 2.

Example: save `a=1, b=2` at checkpoint 2, then append `DELETE a` (3) and
`SET c 3` (4). Restart loads `{a=1,b=2}`, replays only 3 and 4, and publishes
exactly `{b=2,c=3}`. Save records checkpoint 4 and empties the WAL. Reopening
then replays zero operations.

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
storage guarantee. This is an educational single-process database with no
distributed recovery, multi-process coordination, or full ACID claim.
OS flush is not guaranteed physical power-loss durability. Milestone 6 and
later are not implemented here.
