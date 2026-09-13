# Learning Notes

Engineering notes on the ideas MiniDB implements, written to be readable
without prior knowledge. Each section describes what the code in this
repository actually does, not a generic textbook version.

---

## Milestone 2 — Hash tables

### The problem a hash table solves

A database has to answer "what is stored under this key?" quickly. The obvious
approaches do not scale:

- **A list of pairs.** Finding a key means checking each entry in turn. With a
  million keys that is up to a million comparisons — O(n).
- **A sorted array.** Binary search cuts that to about 20 comparisons —
  O(log n) — but every insertion has to shift elements to keep the order.

A hash table answers the question in roughly **constant time**, no matter how
many keys are stored. That is the whole point of it.

### What a hash function is

A hash function turns a value of any size into a fixed-size number.

```text
"name"   -> 14876239847162398471
"age"    ->  9283746518273645192
"name"   -> 14876239847162398471    (same input, always the same output)
```

Two properties matter:

1. **Deterministic.** The same key must always produce the same number, or you
   would never find what you stored.
2. **Well spread.** Different keys should produce numbers that scatter. A hash
   function that returns `0` for everything is still deterministic — and
   useless, because every key ends up in the same place.

MiniDB does not write its own hash function. It uses `std::hash`, which the
standard library provides for strings, integers and other built-in types.
Designing a good hash function is a specialised job, and the standard one is
already well tested.

### What a bucket is

The hash is an enormous number. It has to be turned into a position in an
array. That array is the table, and each slot in it is a **bucket**.

```text
key ──► hash(key) ──► big number ──► number % bucket_count ──► bucket index
```

The `%` (remainder) operator folds any number into the range
`0 .. bucket_count - 1`. With 17 buckets, a hash of 1000 gives
`1000 % 17 = 14`, so that key belongs in bucket 14.

This exact pipeline is in `HashTable::bucket_index`.

### What a collision is

Two different keys can land in the same bucket. There are far more possible
keys than buckets, so this is not a rare accident — it is guaranteed to happen
eventually. This is called a **collision**.

```text
"name" -> hash 1000 -> 1000 % 17 -> bucket 14
"city" -> hash 2717 -> 2717 % 17 -> bucket 14      <- same bucket
```

A hash table that ignored collisions would overwrite one key with another and
silently lose data.

### Separate chaining

MiniDB handles collisions with **separate chaining**: each bucket holds a
linked list ("chain") of every entry that landed there.

```text
bucket 0:  (empty)
bucket 1:  ["age", 18]
bucket 2:  (empty)
   ...
bucket 14: ["city", "Pune"] -> ["name", "Aarya"]
```

- **Insert**: walk the chain. If the key is already there, replace its value.
  Otherwise add a new node at the front, which is O(1).
- **Lookup**: go to the bucket, walk the chain, compare keys, return the match.
- **Erase**: walk the chain and unlink the node.

Nothing is ever lost — a collision just costs a few extra comparisons.

The main alternative is **open addressing**, where a colliding key is placed in
some *other* free bucket. It is faster for the CPU cache, but deletion becomes
awkward: removing an entry can break the search path to entries placed after
it, so implementations need "tombstone" markers. Separate chaining was chosen
because deletion stays simple and obviously correct, and `DELETE` is a command
MiniDB has to get right.

### Load factor

```text
load factor = number of entries / number of buckets
```

It measures how full the table is, and it predicts how long chains get. With
100 entries in 100 buckets, the average chain is one node. With 100 entries in
10 buckets, the average chain is ten nodes and every lookup is ten times
slower.

MiniDB grows the table once the load factor would exceed **0.75**
(`HashTable::kMaxLoadFactor`). That number is a trade:

- Higher (say 1.0) uses less memory but allows longer chains.
- Lower (say 0.5) keeps lookups fast but leaves half the array unused.

0.75 keeps the expected chain length below one while wasting little space.

### Rehashing

When the table gets too full, it **rehashes**: allocate a bigger bucket array
and redistribute every entry into it.

Redistribution is necessary, not optional. The bucket index depends on
`bucket_count`, so changing the count changes where every key belongs:

```text
hash 1000 with 17 buckets -> 1000 % 17 = 14
hash 1000 with 37 buckets -> 1000 % 37 = 1     <- different bucket
```

Two details in MiniDB's implementation:

- Each node **caches its hash** from when it was inserted, so rehashing only
  recomputes a remainder rather than re-hashing every key.
- Nodes are **relinked, not recreated**. The entries do not move in memory, so
  a pointer obtained from `find()` stays valid across a rehash.

Rehashing is O(n) — it touches every entry. But because the table *doubles*
each time, it happens rarely enough that the cost spread across all the
insertions that caused it is O(1) each. This is called **amortised** constant
time. Growing by a fixed amount instead (say 10 buckets at a time) would make
inserting n items cost O(n²) overall.

### Why the bucket counts are prime

MiniDB uses primes — 17, 37, 79, 163, 331, ... — rather than powers of two.

`hash % bucket_count` decides which bits of the hash survive. With a power of
two, only the lowest bits matter. libstdc++ defines `std::hash<int>` as the
**identity function**, so the hash of `16` is `16`. With 16 buckets, the keys
0, 16, 32, 48 would all map to bucket 0 — every one a collision, from perfectly
ordinary data.

A prime modulus mixes all the bits of the hash into the result, so a weak hash
degrades gracefully instead of collapsing. `test_hash_table.cpp` checks exactly
this with 500 integer keys that are all multiples of 16.

### Average versus worst case

| Operation | Average | Worst case |
| --- | --- | --- |
| insert | O(1) | O(n) |
| lookup | O(1) | O(n) |
| erase | O(1) | O(n) |

The **average** assumes keys spread evenly across buckets, so chains stay
short and a lookup checks one or two nodes.

The **worst case** is every key colliding into a single bucket. The table
becomes one long linked list, and a lookup walks all n entries. It is not
theoretical — `test_hash_table.cpp` produces it deliberately by supplying a
hasher that returns `0` for every key.

Saying a hash table is "O(1)" without qualification is wrong. It is O(1)
*on average*, and only while the hash is decent and the load factor is
controlled. MiniDB controls the load factor itself and delegates hash quality
to `std::hash`.

### Why not just keep std::unordered_map

For a real product, keeping it would be the right call — it is written by
experts and heavily optimised.

The reason for replacing it here is that this project exists to demonstrate
understanding of the mechanism, and a hash table is where hashing, collisions,
linked structures, amortised growth and memory ownership all meet in about 300
lines. Using the standard container would have hidden every one of them.

Milestone 1 deliberately used `std::unordered_map` first, so that the database,
the parser and the CLI were all proven correct against a known-good container.
When the custom table replaced it, the existing 61 tests kept passing
unchanged — which meant any new failure could only be the new table.

---

## Milestone 2 — C++ techniques used

### Ownership with `std::unique_ptr`

Each chain node owns the next one:

```cpp
struct Node {
    Key key;
    Value value;
    std::size_t hash;
    std::unique_ptr<Node> next;
};
```

A `unique_ptr` frees what it points at when it is destroyed, and cannot be
copied — so two nodes can never accidentally own the same successor. Deleting
the whole table is automatic: no `delete`, and no way to leak.

### The bug that ownership creates

Destroying a chain the obvious way is recursive. Freeing the head destroys its
`next`, which destroys *its* `next`, and so on — one stack frame per node. With
20,000 colliding entries that exhausts the stack and crashes.

`HashTable::clear` unlinks each node *before* releasing it, so the depth is
always one:

```cpp
std::unique_ptr<Node> node = std::move(bucket);
while (node != nullptr) {
    std::unique_ptr<Node> next = std::move(node->next);
    node = std::move(next);   // frees exactly one node
}
```

There is a test for this, because it is the kind of failure that only appears
under the exact conditions the container is built to handle.

### The pointer-to-pointer erase trick

Removing from a singly linked list normally needs a special case for the first
node, because there is no previous node to relink. `HashTable::erase` avoids it
by tracking *the thing that owns the current node*:

```cpp
std::unique_ptr<Node>* link = &buckets_[index];
while (*link != nullptr) {
    if (matches) {
        std::unique_ptr<Node> removed = std::move(*link);
        *link = std::move(removed->next);
        return true;
    }
    link = &(*link)->next;
}
```

For the first node `link` points at the bucket slot; afterwards it points at
the previous node's `next`. One code path handles both.

### Rule of five

A class that manages a resource by hand usually needs all five of: destructor,
copy constructor, copy assignment, move constructor, move assignment. Writing
the destructor stops the compiler generating the move operations, so once one
is written, the rest have to be considered explicitly.

`HashTable` implements all five. Copying duplicates every chain, so two tables
never share nodes; moving transfers the buckets and leaves the source empty but
still usable.

### Templates and hashing

`HashTable<Key, Value, Hash>` takes the hash function as a template parameter
defaulting to `std::hash<Key>`. That is what makes the collision tests possible
without weakening the real code: the tests pass in a hasher that returns `0`
for every key, and exercise the same chaining logic the database uses.

---

## Milestone 3 — Persistence

### Why a database needs to write to disk

Everything up to now lived in RAM, which the operating system reclaims the
moment the process ends. A database that forgets everything when you close it
is a cache, not a database. Persistence means the data outlives the program.

The hard part is not writing bytes to a file. It is that a program can stop at
*any* moment — killed, crashed, out of disk, power cut — and whatever is on
disk at that instant has to still make sense.

### Serialisation

Serialisation is turning in-memory objects into a sequence of bytes;
deserialisation is the reverse. In memory a key is a `std::string`: a pointer,
a length, and a capacity. None of that can go into a file, because the pointer
is an address that means nothing in another process.

So a record is written as *content*, not as layout:

```text
key_length  value_length  key bytes  value bytes
```

Reading it back means reading the lengths, then reading exactly that many
bytes. The lengths come first for a reason: without them the reader would not
know where one field ends and the next begins.

### Why lengths instead of separators

A text format might write `name=Aarya` and split on `=`. That breaks the moment
a value contains `=`, and fixing it needs an escaping scheme, which then needs
its own rule for escaping the escape character.

An explicit length sidesteps all of it. Any byte can appear in a key or value —
spaces, `=`, newlines, even a zero byte — because the reader never searches for
a delimiter. It is told how far to read.

### Binary versus text

MiniDB uses a binary format: numbers are stored as raw bytes rather than as
digits. `1048576` takes 4 bytes as a `uint32` and 7 as text, and parsing text
back into a number means validating the digits. Binary is smaller and there is
nothing to parse.

The cost is that the file cannot be read in a text editor. That is why
`docs/STORAGE_FORMAT.md` exists, and why it includes a hex dump of a real
snapshot.

### Endianness

Computers disagree about the order of bytes inside a number. The value
`0x12345678` is stored as `78 56 34 12` on a little-endian machine (x86 and
most ARM) and as `12 34 56 78` on a big-endian one.

That matters for file formats. Write the raw bytes of an integer on one machine
and read them on the other, and the number changes.

MiniDB picks **little-endian** and encodes byte by byte:

```cpp
for (int shift = 0; shift < 32; shift += 8) {
    out.push_back(static_cast<char>((value >> shift) & 0xFF));
}
```

The shifts produce the same bytes on any CPU, so a snapshot is portable. This
is also why the code never copies the bytes of an integer directly — that
shortcut would make the format depend on the hardware.

### Untrusted input

A database file is **untrusted input**, even when your own program wrote it. It
may have been truncated by a full disk, edited by someone curious, damaged by
failing hardware, or replaced entirely.

The dangerous pattern looks completely reasonable:

```cpp
uint32_t length = read_length_from_file();
std::string value;
value.resize(length);          // length came from the file
read_bytes(value.data(), length);
```

If the file is corrupt and claims a length of 4,000,000,000, that `resize`
tries to allocate 4 GB. Best case the program dies; worse cases involve reading
past the end of a buffer.

MiniDB validates every length *before* using it: against a configured maximum,
and against the number of bytes actually left in the file. Nothing taken from
the file reaches an allocator unchecked.

### Integer overflow

Fixed-width integers wrap silently when they exceed their range. If a corrupt
file claims 2^64-1 records and the code checks:

```cpp
if (record_count * 9 > payload_bytes) { reject(); }
```

the multiplication wraps to something small, the check passes, and the loop
runs essentially forever. Rearranging the same comparison fixes it:

```cpp
if (record_count > payload_bytes / 9) { reject(); }
```

Division cannot overflow. The rule worth remembering: when validating a value
you do not trust, arrange the arithmetic so it cannot wrap — usually by
dividing the trusted side rather than multiplying the untrusted one.

### Checksums

A checksum is a small number computed from a block of data and stored alongside
it. Recompute it on read; if it differs, the data changed.

MiniDB uses **CRC-32**, which reduces any number of bytes to 4. It catches the
failure no other check can see: a single flipped bit inside a key or a value.
Such a file has a valid magic number, a valid version, a valid record count and
valid lengths — it parses perfectly and hands back the wrong answer. Only the
checksum notices.

A CRC detects *accidental* damage. It is not security: anyone deliberately
editing the file can recompute the checksum too.

### Atomic file replacement

Writing directly over a database file is dangerous. Halfway through, the file
is half old data and half new, and a crash at that moment leaves it that way
permanently.

The standard fix is to never modify the real file:

```text
1. write everything to database.tmp
2. close it
3. rename database.tmp -> database
```

Renaming is a *single* filesystem operation. At every instant the database path
points either at the complete old file or at the complete new one, never at
something in between. A crash during step 1 leaves the real database untouched
and a stray `.tmp` file, which gets cleaned up.

One subtlety: this only works within a single filesystem. Renaming across
volumes is a copy, which is not atomic — which is why MiniDB puts the temporary
file next to the database rather than in the system temp directory.

### Atomic is not the same as durable

These sound alike and are not:

- **Atomic** — an operation either happens completely or not at all. Nobody
  observes a half-done state.
- **Durable** — once the operation reports success, the data survives a power
  cut.

MiniDB's rename is atomic. It is **not** durable, because writing a file only
hands the bytes to the operating system, which may hold them in memory for a
while before the disk sees them. Forcing them out requires `fsync` (or
`FlushFileBuffers` on Windows), which MiniDB does not call.

So MiniDB guarantees you never see a corrupt half-written snapshot. It does not
guarantee that a save survives pulling the plug one second later. Knowing which
of the two you have is the difference between an honest claim and marketing.

### Snapshots versus write-ahead logs

Two strategies for getting data onto disk:

| | Snapshot | Write-ahead log |
| --- | --- | --- |
| What is written | The whole database | Each change, as it happens |
| Cost per change | O(n) — rewrites everything | O(1) — appends one record |
| Cost to load | O(n) — one pass | O(changes) — replay the log |
| Lost in a crash | Everything since the last save | Nothing that was flushed |

MiniDB currently does snapshots only, saving when the session ends. A clean
exit persists everything; a killed process loses the session. That is the gap
Milestone 4 fills: the log records each change immediately, so a crash costs at
most the last unflushed write.

Real databases use both. The log captures changes cheaply as they arrive, and
periodic snapshots keep the log from growing forever.

### Why a corrupt file must not become an empty database

The tempting shortcut on a failed load is to shrug and start empty. It is
actively destructive. The session would end with a save, that save would
overwrite the damaged file with an empty snapshot, and the original bytes —
which a person might have recovered something from — would be gone.

MiniDB refuses to start instead, reports what is wrong, and leaves the file
alone. Failing loudly beats destroying data quietly.

### RAII for files

The scratch file has to disappear on every failure path: a write error, a
rename failure, an exception from anywhere in between. Writing
`remove(temp_path)` before each of a dozen `return` statements works right up
until someone adds the thirteenth.

Instead the cleanup lives in a destructor:

```cpp
class ScratchFile {
    ~ScratchFile() { if (remove_on_destruction_) fs::remove(path_); }
    void keep() noexcept { remove_on_destruction_ = false; }
};
```

Every exit from the function runs the destructor, including one caused by an
exception. The success path calls `keep()`. This is the same idea as
`unique_ptr` freeing memory, applied to a file: the resource is released by
leaving the scope, not by remembering to say so.

## LRU: finding a value and remembering access order

An LRU cache evicts the least recently used entry when it runs out of room.
A hash map answers “where is this key?” but does not efficiently answer “which
key was used longest ago?” A doubly linked list supplies that ordering.

MiniDB stores key/value nodes in `std::list`, with MRU at the front and LRU at
the back. `std::unordered_map` maps keys to list iterators. The iterator means
GET can locate a node by hash lookup and splice it to the front without walking
the list. Both links are available, so unlinking a middle node is constant time.

For capacity two (left is MRU):

| Operation | Order afterward | Result |
| --- | --- | --- |
| PUT a=1 | a | Insert |
| PUT b=2 | b, a | Insert |
| GET a | a, b | Return 1 and promote a |
| PUT c=3 | c, a | Evict b |
| PUT a=4 | a, c | Update and promote a |

Contains does not promote. A missing GET does not change order. Capacity zero
is a disabled cache; capacity one keeps at most the last inserted/accessed key.
Eviction removes exactly one entry when a successful new insertion exceeds the
capacity. It never deletes the authoritative database entry.

### Where this fits in Database

GET → cache hit → copy the cached value into Result.

GET → cache miss → look up the in-memory HashTable → cache the found value →
return it. A missing key remains NotFound and is not cached.

SET/DELETE invalidate the affected cache entry only after successful WAL append.
The next GET reads the authoritative value again. CLEAR and successful recovery
empty the cache. A failed recovery leaves the previous table/cache consistent.
The cache starts empty on restart and is rebuilt from reads; it is never stored
in the snapshot or WAL. Existing `get() const` works because recency is mutable
implementation state, not a modification of the logical database contents.

### Complexity needs units

For C cached entries, hash lookup/PUT are average O(1), but can be O(C) under
collisions or during rehash. Promotion and selecting/unlinking the LRU tail are
O(1); eviction's map removal is average O(1). A key of K bytes still takes O(K)
to hash, and copying a value of V bytes takes O(V). Size and capacity are O(1);
clear walks C entries. The list and map use O(C) metadata plus stored bytes.

A cache hit does not make disk access O(1). MiniDB already loads its authoritative
state into memory; a miss reads that table, not a disk page. Snapshot operations
and WAL recovery keep their existing whole-input costs. This cache can even add
overhead, so its value here is learning the data structure and keeping cached
copies correct. Capacity limits entries, not RAM bytes. There is no concurrent
access support and no claimed benchmark improvement.
