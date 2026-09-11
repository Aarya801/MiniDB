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
