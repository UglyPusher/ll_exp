# OrderBook Design

## Component Boundary

`OrderBook` owns the storage and ordering of already-resting orders for one
instrument. A caller supplies matching policy and externally serializes all
access.

```text
OrderBook
  -> OrderPool
  -> OrderIdIndex
  -> Bid SideBook
  -> Ask SideBook
```

Internal slots, hash buckets, FIFO links, price levels, and segments are
implementation details. Public operations exchange value objects and never
expose `OrderIndex`.

## Data Structures

### OrderPool

`OrderPool` owns a fixed array of `Order` slots. Construction links free slots
into a freelist, and runtime insertion acquires one slot without allocation.
Erasure returns the slot to that freelist.

Each active `Order` stores intrusive `prev` and `next` slot indices. This makes
unlinking the head, middle, or tail of a price FIFO constant-time once the slot
is known. Indices are used instead of pointers so moving the owning arrays is
not required and the sentinel representation remains compact.

### OrderIdIndex

`OrderIdIndex` maps an external `OrderId` to an internal pool slot. It uses open
addressing over one fixed, power-of-two bucket array. The fixed table avoids
runtime rehashing and per-entry allocation.

Lookup stops at the first empty bucket. Erasure uses in-place backward-shift
deletion: subsequent entries are scanned and moved only when their probe path
would otherwise be broken. This preserves lookup termination without a second
bucket state or reinserting the cluster through the public insertion path.

The table is deliberately sized below 50% load for valid `OrderBook`
configurations. Probe length still depends on the hash distribution, so index
operations are not guaranteed constant-time.

### SideBook

Bid and Ask storage use separate `SideBook` instances. A side maps configured
prices directly into an array of `PriceSegment` objects. This trades memory
proportional to the configured price range for direct addressing and predictable
runtime mutation.

Each `PriceSegment` contains:

- 64 `PriceLevel` objects;
- one 64-bit `active_mask`, with one bit per non-empty level.

Each side also owns a bitmap with one bit per segment. A set bit means that the
segment contains at least one active level. The bitmap lets a best-segment
recompute scan 64 segments per machine word instead of reading every segment.

`best_segment` caches the segment containing the current best price. Within
that segment, standard C++20 bit operations select the lowest Ask offset or the
highest Bid offset. A cache avoids scanning the segment bitmap on ordinary
`best()` calls and on mutations that do not empty the current best segment.

### PriceLevel

A `PriceLevel` stores FIFO head and tail indices, active order count, and exact
aggregate quantity. New orders append at the tail. `set_remaining` updates the
aggregate but does not requeue the order; any priority policy for quantity
increases belongs to the caller.

`Quantity` for one order is `uint32_t`. `AggregateQuantity` is `uint64_t`,
allowing a level or side total to exceed `UINT32_MAX` without modulo overflow.

## Mutation Coordination

`OrderBook` coordinates the pool, index, and side structures:

- successful insertion acquires a slot, inserts one ID mapping, then appends to
  one side FIFO;
- `set_remaining` performs one index lookup and updates the order, level, and
  side aggregates;
- successful erasure obtains and removes the slot mapping in one index pass,
  unlinks the FIFO node, updates masks and aggregates, then releases the slot.

Insertion rolls back the acquired pool slot if index insertion fails. No FIFO
or aggregate mutation occurs before both the slot and ID mapping are secured.

## Invariants

The following conditions hold after construction and every completed public
operation, provided calls are externally serialized:

- every active `OrderId` is unique;
- every active order has `remaining > 0`;
- every active order price is inside the configured inclusive range;
- Bid best is the maximum active Bid price;
- Ask best is the minimum active Ask price;
- orders at one side and price are linked in insertion FIFO order;
- `set_remaining` does not change FIFO position;
- removal is represented by `erase`, not by setting quantity to zero;
- each active pool slot has exactly one index entry and one FIFO position;
- every index entry names the same active order stored in its pool slot;
- FIFO links, level counts, side counts, and aggregate quantities agree;
- a bit in `active_mask` is set exactly when its `PriceLevel` is non-empty;
- a side bitmap bit is set exactly when its `PriceSegment` is non-empty;
- cached best validity agrees with side emptiness and identifies the true best
  occupied segment;
- failure statuses leave the logical state unchanged;
- internal `OrderIndex` values never leave the public `OrderBook` API.

There are no synchronization guarantees for concurrent calls. Single-writer,
externally serialized access is part of the contract. Read/write races are
undefined behavior.

## Complexity

Let:

- `P` be order capacity;
- `B` be index bucket count;
- `S` be segment count per side;
- `W = ceil(S / 64)` be segment bitmap words per side;
- `p` be the number of index buckets probed;
- `c` be the scanned backward-shift cluster length.

| Operation | Complexity | Qualification |
|---|---|---|
| Construction | `O(P + B + S + W)` | Allocates and initializes all arrays for both sides. |
| `insert` | `O(p)` | Pool acquire and FIFO append are `O(1)`; index insertion depends on probes. |
| `best` | `O(1)` | Uses cached segment and one 64-bit level mask. |
| `set_remaining` | `O(p)` | Index lookup plus constant-time aggregate updates. |
| FIFO unlink | `O(1)` | Intrusive links are available after index lookup. |
| `erase` | `O(p + c + W)` worst case | Bitmap scan occurs only when the best segment becomes empty. |
| Backward-shift repair | `O(c)` | One pass over the affected probe cluster. |
| Best-segment recompute | `O(W)` worst case | Scans segment occupancy words in side-specific direction. |
| `validate_invariants` | `O(Pp + B + 64S + W)` | Full traversal, including an index lookup for each active pool slot. |

Hash-table operations are expected to be short at the configured load policy,
but they are not guaranteed `O(1)`.

## Memory Model

For a valid inclusive price range:

```text
pool_bytes = P * sizeof(Order)
B = smallest power of two >= (P == 0 ? 1 : 2 * P + 1)
index_bytes = B * sizeof(Bucket)
S = (max_price_tick >> 6) - (min_price_tick >> 6) + 1
W = ceil(S / 64)
one_side_bytes = S * sizeof(PriceSegment) + W * sizeof(uint64_t)
allocated_array_bytes = pool_bytes + index_bytes + 2 * one_side_bytes
```

The `OrderBook` object and allocator metadata are additional overhead.
Construction can throw `std::bad_alloc`. `UINT32_MAX` is a scalar domain limit,
not a promise that the corresponding storage can be constructed.

As a platform-specific x64 MSVC 19.44 example, not an ABI guarantee:

| Type | Size |
|---|---:|
| `Order` | 40 bytes |
| `OrderIdIndex::Bucket` | 16 bytes |
| `PriceLevel` | 24 bytes |
| `PriceSegment` | 1,544 bytes |
| `OrderPool` | 24 bytes |
| `OrderIdIndex` | 24 bytes |
| `SideBook` | 72 bytes |
| `OrderBook` | 208 bytes |

Example allocated-array footprints on that build:

| Configuration `{min, max, capacity}` | Segments/side | Approximate total |
|---|---:|---:|
| `{1, 4096, 400000}` | 65 | 32,978,176 bytes |
| `{1, 1000000, 400000}` | 15,626 | 81,034,432 bytes |

The direct price array is appropriate when bounded prices and predictable
access matter. A very wide sparse range consumes memory even if few prices are
active; that is an explicit design tradeoff.

## Construction and First Touch

Construction allocates and sequentially initializes the pool, index buckets,
price segments, and segment bitmaps. Applications that care about NUMA
placement should construct the book on its final owner thread after selecting
CPU affinity and memory policy. Memory locking, affinity, and NUMA policy remain
application responsibilities.
