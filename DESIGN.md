# DESIGN.md

## Overview

Each instrument has its own order book: an array indexed by price, bounded by
the instrument's daily price band. All books share one order pool and one
order-id index. Nothing on the `submit`/`cancel`/`replace` path allocates,
throws or locks; the engine is single-threaded by contract.

## Data structures

**Price ladder and array-indexed book.** A resting order's price must lie in
the band `[ceil_to_tick(ref·(1−b)), floor_to_tick(ref·(1+b))]`, which is
fixed at `configure()`. `PriceLadder` maps every tick-aligned price in that
band to a dense index, in up to four segments (one per tick regime the band
touches), with O(1) arithmetic both ways. Each side of a book is a flat
`std::vector<Level>` indexed by it, so `price_to_index`, `add_resting`,
`remove_resting` and `top_of_book` are O(1), and the levels a match walks
are contiguous. A ±10% band gives a few hundred to ~2000 levels per side.

**Best price: cached index + `LevelBitset`.** Each side caches its best
index. When that level empties, `find_highest_le`/`find_lowest_ge` find the
next occupied level by skipping whole 64-bit words with
`std::countl_zero`/`std::countr_zero`: O(1) amortized, O(depth/64) worst
case. Each loop tests the word for zero before the call, so GCC emits a bare
`bsr`/`tzcnt`.

**FIFO: intrusive list in the pool.** Each `OrderRecord` holds `prev`/`next`
slot indices. A `Level` is `head`, `tail` and a 64-bit `total_qty`
(16 bytes). It keeps no order count: a level is empty exactly when
`head == kInvalidSlot`. Append, unlink and head-first matching are O(1) with
no per-order allocation.

**Order pool: fixed, chunked, append-only.** `configure()` allocates all
`max_orders` slots in chunks of 2²⁰ (the last chunk holds the remainder).
Chunks never move, so slot indices stay valid. Order ids are unique for the
engine's lifetime, so slots are never reused. `try_allocate()` never
allocates; a full pool returns `kInvalidSlot` and the engine rejects with
`kCapacityExceeded`.

**Id index: `FlatHashMap`.** Open addressing with linear probing,
`splitmix64`-mixed keys and a 0.7 load-factor cap. It supports insert and
find only, because ids are permanent. It is reserved for `max_orders` in
`configure()`, so the hot path uses `insert_no_grow()`, which never rehashes.
The growing `insert()` is used only at configure time. A rehash allocates
before touching any member, so a failure leaves the map intact.

## Integer limits

- **Prices** are `int64_t` in 1/10000 units; no floating point in tick, band
  or trade-price math. The band divisor is `kBpsPerUnit`, not `kPriceScale`:
  both are 10000, but they mean different things.
- **Reference price** ≤ `INT64_MAX / kMaxBandBps` keeps `ref · band_bps` and
  the band's upper edge inside `int64_t`.
- **Ladder size** ≤ `kMaxPriceLevels` (2²⁴ per side, at most ~512 MB of level
  arrays per instrument). The level count is about `band width / tick`, and
  the top tick bracket has a fixed 0.10 tick, so a high price with a wide
  band can need billions of levels. `configure()` computes the count in
  64 bits with `price_level_count()` and rejects anything over the cap.
  The limit is on level count, not price: `price_level_count()` and
  `PriceLadder::build` walk the band with the same `tick_span_at()`, so
  any tick-table change moves the check and the allocation together.
  `build` re-checks and throws `std::length_error`, so ladder indices always
  fit in `uint32_t`.
- **Quantities**: one order's `Quantity` is `uint32_t`; any sum of
  quantities (a level's total, the FOK pre-check, `TopOfBook`) is
  `AggregateQuantity` (`uint64_t`). At most `kMaxOrderCapacity` orders of
  `UINT32_MAX` each cannot overflow it.
- **Slots**: `kMaxOrderCapacity` is 2³² − 2, so every slot index stays below
  the `kInvalidSlot` sentinel (2³² − 1) and the slot counter cannot wrap.

## Error handling and validation

- No exception crosses the C ABI. `submit`/`cancel`/`replace`/`top_of_book`
  are `noexcept` with no `try/catch`: nothing they call can throw, and
  `src/engine.cpp` `static_assert`s `noexcept` on every building block they
  use. A bug that throws there hits `std::terminate`, which is better than a
  half-updated book.
- Listener callbacks are `noexcept`; a throwing listener terminates.
- `configure()` validates every instrument before building any book:
  non-null pointer, `0 < count ≤ UINT32_MAX`,
  `0 < max_orders ≤ kMaxOrderCapacity`, tick-aligned reference price within
  the bound above, `band_bps ≤ kMaxBandBps`, ladder size ≤ `kMaxPriceLevels`,
  unique instrument ids. Any failure returns `kInvalidArgument`. It builds
  into locals and commits with nothrow moves (strong guarantee).
  `bad_alloc` maps to `kOutOfMemory`, any other exception to
  `kInternalError`.

## Memory layout

- `OrderRecord` is 48 bytes, with the 8-byte fields first and no padding
  (`static_assert`ed). A pool chunk is ~48 MB.
- `Level` is 16 bytes (`static_assert`ed); there are two arrays per
  instrument.
- `configure()` does all large allocations: pool, id index, and each book's
  level and bitmap arrays.
- `replace_in_place` splices the new slot into the old slot's exact list
  position, so a same-price size reduction never disturbs FIFO order.

## Rejected alternatives

- **`std::map<Price, Level>`**: O(log n) and pointer-chasing, when the band
  already makes the price domain small and known.
- **`std::unordered_map` for ids/orders**: one allocation per order on the
  hot path.
- **Recycling pool slots**: would need generation tracking to stay safe,
  for little gain since ids are permanent and capacity is fixed.
- **Growing the pool/index on demand**: a chunk allocation or a full rehash
  inside `submit()` would cause latency spikes and possible `bad_alloc`. A
  hard cap with `kCapacityExceeded` keeps order entry allocation-free.
- **Google Benchmark**: a small `chrono` driver keeps the repo dependency-free
  apart from GoogleTest and generates exactly the order-flow mix needed.

## Contract readings

Choices the API leaves open; tests pin each one.

- An unconfigured engine rejects every `submit`/`cancel`/`replace` with
  `kNotConfigured` before any other check. It consumes no id. Its enum value
  (7) breaks the "validation follows enum order" rule only to keep values
  stable.
- An `order_id` is consumed at the uniqueness step, even if a later step
  (quantity/TIF/tick/band) rejects the order. `replace()`'s `new_order_id`
  follows the same rule, except for a same-id replace or a replace down to
  zero open quantity (a plain cancel that never touches `new_order_id`).
- `replace()` re-validates tick and band before registering `new_order_id`,
  so a failed replace leaves the original order open under its own id.
- `kCapacityExceeded` (value 8) is checked right after the duplicate-id step
  in `submit()`, so the id is not consumed. In `replace()` it is the last
  check before any state change.
- A replace "increases quantity" (and loses priority) when the new open
  quantity exceeds the *current* open quantity, not the original size.
- A quantity-only replace cannot cross the book, so matching runs only when
  the price changes.

## Next steps for production

Self-trade prevention and per-account risk checks; a sequencer/journal for
crash recovery; per-instrument sharding across cores once one core becomes
the bottleneck; SIMD-probed id-index buckets if profiling shows the index as
the hot spot.
