# Snapshot Format

The on-disk format MiniDB uses for persistence, as of Milestone 3.

A snapshot is the entire database in one file. Saving rewrites the whole file;
loading reads the whole file. There is no incremental update and no log — the
write-ahead log arrives in Milestone 4.

- **Format version:** 1
- **Default location:** `%LOCALAPPDATA%\MiniDB\minidb.snapshot` on Windows,
  `$XDG_DATA_HOME/minidb/minidb.snapshot` or
  `$HOME/.local/share/minidb/minidb.snapshot` elsewhere
- **Written by:** `StorageManager::save`, in `src/storage.cpp`
- **Read by:** `StorageManager::load`

## Conventions

**All integers are unsigned, fixed-width, and little-endian.** They are encoded
one byte at a time, least significant byte first, and decoded the same way.
Nothing is written by copying the bytes of an in-memory integer, so a snapshot
written on a big-endian machine is readable on a little-endian one and vice
versa. There is no `reinterpret_cast` in the reader or the writer.

**Keys and values are byte strings, not text.** They are stored with an
explicit length and no terminator, so any byte — including `0x00` — is valid
data. Nothing is assumed about their encoding.

## Layout

```text
┌─────────────────────────────────────────────┐
│ HEADER                             24 bytes │
├─────────────────────────────────────────────┤
│ RECORD 0                     variable bytes │
│ RECORD 1                                    │
│ ...                                         │
│ RECORD record_count-1                       │
└─────────────────────────────────────────────┘
```

The file is exactly the header plus `record_count` records. Any byte after the
last record makes the file invalid.

### Header

| Offset | Size | Field | Value |
| --- | --- | --- | --- |
| 0 | 8 | `magic` | ASCII `MINIDBSS`, no terminator |
| 8 | 4 | `version` | `uint32` — must be `1` |
| 12 | 8 | `record_count` | `uint64` — number of records that follow |
| 20 | 4 | `payload_crc32` | `uint32` — CRC-32 of every byte after the header |

An empty database is a valid snapshot: a 24-byte file with `record_count = 0`
and the CRC of zero bytes.

### Record

Records follow the header back to back, with no padding or alignment.

| Offset | Size | Field | Value |
| --- | --- | --- | --- |
| +0 | 4 | `key_length` | `uint32` — 1 to 1048576 |
| +4 | 4 | `value_length` | `uint32` — 0 to 1048576 |
| +8 | `key_length` | `key` | raw bytes |
| +8 + `key_length` | `value_length` | `value` | raw bytes |

A record therefore occupies `8 + key_length + value_length` bytes, and at
minimum 9 bytes, since a key must be at least one byte long. An empty *value*
is allowed; an empty *key* is not, because the database itself rejects one.

Records appear in whatever order the hash table produced them. The order
carries no meaning and may differ between two saves of identical data.

### Checksum

`payload_crc32` is the standard CRC-32 (IEEE 802.3, reflected polynomial
`0xEDB88320`, initial value `0xFFFFFFFF`, final XOR `0xFFFFFFFF`) computed over
every byte after the header — that is, over the record region exactly as it
appears in the file, length fields included.

The writer does not know it until the payload has been written, so the header
goes out with a placeholder of `0` and the real value is patched in afterwards
by seeking back to offset 20. That avoids either encoding every record twice or
holding the whole encoded payload in memory.

### Worked example

Two records, `language` → `C++` and `name` → `Aarya`:

```text
offset  bytes                                             meaning
0       4d 49 4e 49 44 42 53 53                           "MINIDBSS"
8       01 00 00 00                                       version 1
12      02 00 00 00 00 00 00 00                           record_count 2
20      03 dc d8 9a                                       payload_crc32
24      08 00 00 00                                       key_length 8
28      03 00 00 00                                       value_length 3
32      6c 61 6e 67 75 61 67 65                           "language"
40      43 2b 2b                                          "C++"
43      04 00 00 00                                       key_length 4
47      05 00 00 00                                       value_length 5
51      6e 61 6d 65                                       "name"
55      41 61 72 79 61                                    "Aarya"
```

Total: 60 bytes.

## Size limits

Every limit lives in `include/minidb/types.hpp` and is enforced on both write
and read.

| Limit | Value | Purpose |
| --- | --- | --- |
| `kMaxKeySize` | 1 MiB (1048576) | Largest key |
| `kMaxValueSize` | 1 MiB (1048576) | Largest value |
| `kMaxRecordCount` | 10,000,000 | Largest `record_count` a reader will believe |
| `kMaxSnapshotSize` | 256 MiB | Largest file a reader will open |

The writer checks records against the key and value limits before creating any
file. A snapshot MiniDB refuses to read is one it must also refuse to write —
declining to save is recoverable, while writing a file that cannot be loaded is
not.

## Validation

The file is untrusted input. It may have been truncated, edited by hand,
damaged by failing hardware, or produced by a different program entirely.
Checks run in this order, and the first failure stops the load:

| # | Check | Failure |
| --- | --- | --- |
| 1 | Path exists | `NotFound` — treated as "new database", not an error |
| 2 | Path is a regular file | `IoError` |
| 3 | Size ≤ `kMaxSnapshotSize` | `CorruptData` |
| 4 | Size ≥ 24 bytes | `CorruptData` |
| 5 | File opens for reading | `IoError` |
| 6 | Full 24-byte header present | `CorruptData` |
| 7 | `magic` == `MINIDBSS` | `CorruptData` |
| 8 | `version` == 1 | `UnsupportedVersion` |
| 9 | `record_count` ≤ `kMaxRecordCount` | `CorruptData` |
| 10 | `record_count` ≤ payload bytes ÷ 9 | `CorruptData` |
| 11 | Per record: 8 length bytes present | `CorruptData` |
| 12 | Per record: `1 ≤ key_length ≤ kMaxKeySize` | `CorruptData` |
| 13 | Per record: `value_length ≤ kMaxValueSize` | `CorruptData` |
| 14 | Per record: declared bytes ≤ bytes left in file | `CorruptData` |
| 15 | Per record: key and value bytes actually read | `CorruptData` |
| 16 | No key appears twice | `CorruptData` |
| 17 | No bytes left over after the last record | `CorruptData` |
| 18 | `payload_crc32` matches the payload | `CorruptData` |

The ordering is deliberate. Cheap checks that can reject a file outright come
before expensive ones, and **every length is validated before it is used** —
neither `reserve()` nor `resize()` is ever handed a number taken straight from
the file.

Specific hazards and how they are handled:

- **Absurd `record_count`.** Compared as `record_count > payload_bytes / 9`
  rather than `record_count * 9 > payload_bytes`. Dividing cannot overflow, so
  a count of 2⁶⁴−1 is rejected instead of wrapping to a small number.
- **Length arithmetic.** `key_length` and `value_length` are 32-bit; their sum
  is computed as a 64-bit value, so it cannot wrap. It is then compared against
  the bytes actually remaining.
- **Unbounded allocation.** `reserve()` is called only after `record_count` has
  been bounded twice, and `resize()` only after the declared length has been
  checked against both the configured limit and the real remaining file size.
- **Duplicate keys.** Rejected rather than resolved. `save()` cannot produce
  them, so a file containing them has been altered, and quietly keeping
  whichever record came last would be silent data loss.
- **Trailing bytes.** Rejected even when every record parsed, because the file
  is then not what its header describes.
- **A flipped bit inside a key or value.** Structurally invisible — the magic,
  version, count and lengths are all still correct. Only the checksum catches
  it, which is why the format has one.

On any failure, the caller's record vector is left **empty**. Records are
parsed into a local vector and handed over only once every check has passed, so
no failure path can leak a partially read snapshot.

Most importantly: a damaged snapshot is **never** reported as an empty
database. `Database::load` returns the failure and leaves the in-memory
database untouched, and the CLI refuses to start rather than continuing with no
data — because continuing would overwrite the damaged file with an empty
snapshot on exit and destroy any chance of recovering it.

## Atomic replacement

`save()` never writes to the database file directly:

```text
write all records to <database>.tmp
        ↓
flush and close the temporary file
        ↓
rename <database>.tmp over <database>      ← single filesystem operation
```

The scratch file sits **beside** the database rather than in the system
temporary directory, because replacing a file is only atomic within a single
filesystem and the system temp directory is often a different volume.

Cleanup is handled by an RAII guard (`ScratchFile` in `src/storage.cpp`) that
removes the temporary file unless the rename succeeded. Every failure path, and
any exception thrown along the way, therefore leaves no scratch file behind.

### Platform notes

`std::filesystem::rename` is specified to replace an existing destination, and
both supported platforms implement it as one operation:

- **POSIX** — `rename(2)`, atomic by specification.
- **Windows** — `MoveFileEx` with `MOVEFILE_REPLACE_EXISTING`. Atomic with
  respect to other processes on the same volume. It can fail if another process
  holds the destination open, which is reported as `IoError` rather than
  retried; MiniDB assumes one process owns a database at a time.

## What this actually guarantees

Stating this precisely matters more than making it sound strong.

**Guaranteed:**

- A reader never sees a partially written snapshot. The rename makes the new
  file visible in one step, so a concurrent reader observes either the complete
  previous snapshot or the complete new one.
- If MiniDB crashes, is killed, or fails mid-write, the existing snapshot is
  left exactly as it was. The incomplete data is in the scratch file, which is
  discarded.
- A snapshot that is damaged in any way described above is detected and
  refused, not silently misread.

**Not guaranteed:**

- **Durability against power loss or OS crash.** `save()` flushes to the
  operating system but does **not** call `fsync`/`FlushFileBuffers` on the file
  or its directory. After `save()` returns, the data may still be in the OS
  page cache. If the machine loses power at that moment, the filesystem may
  come back with either version of the file, or in principle with a
  zero-length one. Genuine durability needs an explicit flush to the device,
  which MiniDB does not perform.
- **Anything between saves.** The CLI saves when the session ends normally. Kill
  the process mid-session and every change made since startup is gone — nothing
  on disk records them. This is the central limitation of snapshot-only
  persistence and precisely what the write-ahead log in Milestone 4 exists to
  fix.
- **Multi-process safety.** There is no locking. Two MiniDB processes on one
  database file will overwrite each other's snapshots.

In short: MiniDB currently provides **atomic snapshot replacement**, not crash
durability. WAL and crash-recovery guarantees do not exist until Milestone 4.

## Complexity

For `n` records:

| Operation | Time | Memory |
| --- | --- | --- |
| `save` | O(n) | O(1) beyond the records handed in |
| `load` | O(n) | O(n) for the records produced |

Neither is O(1), and neither can be: both touch every record. Duplicate
detection during load uses MiniDB's own `HashTable`, keeping it O(n) on average
rather than the O(n log n) a sort would cost.

## Versioning

`version` exists so that an incompatible future layout is **refused** rather
than misread. A reader that meets a version it does not know returns
`UnsupportedVersion`, which is deliberately distinct from `CorruptData`: the
file may be perfectly well formed, just written by a newer MiniDB, and the user
deserves to be told the difference.

There is no migration path yet. Version 1 is the first format, so there is
nothing to migrate from, and inventing an upgrade framework before a second
version exists would be speculation.
