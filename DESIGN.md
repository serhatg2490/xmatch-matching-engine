# DESIGN.md

## Overview

Per-instrument order books, each an array-indexed price ladder bounded by
that instrument's daily price band, plus a chunked order pool and an
open-addressing id index shared across the whole engine. Nothing on the
`submit`/`cancel`/`replace` hot path allocates, throws, or takes a lock
(the engine is single-threaded by contract).

## Data structures and complexity

**Price ladder → array-indexed order book, not a `std::map`.** A resting
limit order's price is only ever valid if it already passed the band check,
so every price a level can ever need is known at `configure()` time:
`[ceil_to_tick(ref·(1−b)), floor_to_tick(ref·(1+b))]`. `PriceLadder` walks
that range once, in up to 4 segments (the tick table has 4 regimes;
a band only straddles more than one when the reference sits within ~10% of
a 20/50/100 TRY breakpoint), and gives O(1) `price↔index` via direct
arithmetic within a segment (`include/engine/price_ladder.hpp`). Each side
of each book is then a flat `std::vector<Level>` indexed by that number —
`price_to_index`/`add_resting`/`remove_resting` are all O(1), versus
O(log n) for a tree-based book, and the levels a matching loop actually
touches are contiguous, not pointer-chased.

For a ±10% band this bounds book depth to a few hundred–~2000 levels per
side per instrument (e.g. ref=25 TRY → tick 0.02 → ~250 ticks each way).
At 150 instruments that's low tens of MB for the level arrays, dwarfed by
the order pool.

**Best-price tracking: cached index + `LevelBitset`.** `top_of_book` must
be O(1), so each side keeps a cached best index. When the best level empties,
`LevelBitset::find_highest_le`/`find_lowest_ge` relocate the new best by
skipping whole 64-bit words via `clz`/`ctz` rather than scanning price by
price — O(1) amortized, worst case O(depth/64). The scans go through
`std::countl_zero`/`std::countr_zero` (`<bit>`, C++20) rather than the
`__builtin_*` intrinsics, so the file needs no compiler extension; both
loops test the word for zero before calling, which is what lets GCC emit
the same bare `bsr`/`tzcnt` despite the std functions being defined at zero.

**FIFO priority: intrusive doubly-linked list inside the order pool.**
Each `OrderRecord` carries `prev`/`next` pool indices; a `Level` is just a
`head`/`tail` pair. Insertion at tail, removal from anywhere, and head-first
consumption during matching are all O(1), with no per-order node allocation.

**Order pool: chunked, append-only, never recycled.** `order_id` is unique
for the engine's lifetime (part of the API contract), so a
slot is never reused for a different id — this sidesteps ABA-style bugs and
makes "is this id known" a single lookup. The pool has a fixed capacity,
`max_orders`, passed to `configure()`, which allocates every ~1M-entry chunk
(`OrderPool`, `include/engine/order_pool.hpp`) in one shot; the last chunk
holds only the remainder, so a small capacity stays small. Chunks never
move, so slot indices and in-flight references stay valid for the engine's
lifetime. `try_allocate()` never allocates: when the pool is full it returns
`kInvalidSlot` and the engine rejects with `kCapacityExceeded`. The
benchmark passes `warmup_ops + num_ops` (~10.5M), since each op records at
most one id, so the one-time cost lands in `configure()`, which isn't
measured.

**Id lookup: custom open-addressing hash map, insert/find only.**
`FlatHashMap` (`include/engine/flat_hash_map.hpp`) never erases — ids are
permanent, so there's no tombstone/backward-shift-deletion complexity to
pay for. Linear probing, `splitmix64`-mixed keys (client ids are often
small sequential integers, which would otherwise cluster), load factor
capped at 0.7, reserved in `configure()` for `max_orders` entries. Since
every recorded id takes a pool slot, the index can never reach its load
limit, and the hot path inserts with `insert_no_grow()`, which never
rehashes. The growing `insert()` remains for `configure()`-time use (the
instrument index). The table is grow-only: `rehash` never goes below the
capacity its entries need, allocates the new array before touching any
member, and re-places entries without a load check, so a failed growth
leaves the map intact. `std::unordered_map` was
rejected here specifically because of its node-per-entry allocation on
every `submit()` — one array-backed table amortizes to O(1) with no hot-path
`malloc`.

**Fixed-point everywhere.** `Price`/`Quantity` stay `int64_t`/`uint32_t`
throughout tick, band, and trade-price arithmetic — never float/double.
Rounding drift in a price comparison is a correctness bug, not a rounding
inconvenience, so the type system rules it out.

## Memory layout / hot-path decisions

- `OrderRecord` fields are ordered 8-byte-first to minimize padding
  (id, price, then the four `uint32_t`s, then the two 1-byte flags) — 48
  bytes/order, so a chunk (2²⁰ entries) is ~48 MB, i.e. matching loops that
  walk a level's linked list stay within a small number of cache lines per
  order touched.
- No heap allocation, syscalls, or exceptions inside `submit`/`cancel`/
  `replace`'s success paths. `configure()` does the only large allocations
  (pool + id-index reserve, one per instrument's level/bitmap arrays).
- No exception may cross the C ABI boundary, since the host may well be
  built with a different toolchain. `submit`/`cancel`/`replace`/`top_of_book`
  are `noexcept` (API v4) *by construction*: nothing they call can throw,
  and every building block they rely on is `static_assert`ed `noexcept` in
  `src/engine.cpp`. They carry no `try/catch`. Before v4 they swallowed
  exceptions with `catch (...) {}`, which meant a growth-path `bad_alloc`
  produced no event at all and could leave an id half-registered; worse, a
  future throw between two book mutations would have been silently swallowed
  into a half-applied book. Now such a bug hits `std::terminate`: fail-stop
  beats a corrupted book. `configure()` and `xmatch_create()` are the only
  places that catch. `configure()` reports what it caught: `std::bad_alloc`
  becomes `ConfigureResult::kOutOfMemory`, anything else `kInternalError`.
- `IEventListener` callbacks are `noexcept` (API v3): a throwing listener
  hits `std::terminate`.
- `configure()` validates every input before it builds anything (non-null
  pointer, `0 < count <= UINT32_MAX`, `0 < max_orders <= kMaxOrderCapacity`,
  tick-aligned reference price no larger
  than `INT64_MAX / kMaxBandBps`, `band_bps <= kMaxBandBps`, unique
  instrument ids) and returns `kInvalidArgument` otherwise. The reference
  price bound keeps `ref · band_bps` and the band's upper edge inside
  `int64_t`. A tick-aligned reference also guarantees the ladder holds at
  least one level, even with `band_bps = 0`. The band divisor is
  `kBpsPerUnit`, not `kPriceScale`: both are 10000, but one converts basis
  points to a ratio and the other fixes the price's decimal places.
  `kMaxOrderCapacity` is 2³² − 2, so every slot index stays below
  `kInvalidSlot` (2³² − 1), the "no order" sentinel in the FIFO links, and
  the 32-bit slot counter can never wrap.
- The unconfigured-engine guard in `submit`/`cancel`/`replace` is one
  well-predicted branch on `configured_`; side-by-side runs against the
  previous build showed no measurable latency change.
- `replace()`'s "keeps priority" path (`OrderBook::replace_in_place`)
  splices the new slot into the *exact* linked-list position the old one
  held — no unlink-then-append, so a pure quantity decrease never touches
  the FIFO ordering, rather than approximately preserving it.

## Rejected alternatives

- **`std::map<Price, Level>` per side.** O(log n) per touch and pointer-
  chasing/cache-unfriendly; rejected once it was clear the price band makes
  the domain of valid prices small and known upfront.
- **`std::unordered_map` for both the id index and the order objects.**
  Simpler to write, but a `malloc`/`free` per order is exactly what "no
  heap allocation on the hot path" rules out; a 10M-op benchmark would pay
  for ~10M allocator round-trips.
- **Recycling order-pool slots on close/cancel.** Would shrink steady-state
  memory, but reintroduces id/slot lifetime bugs (a slot's "generation" has
  to be tracked and checked everywhere) for a saving that doesn't matter
  given ids are permanent and pre-reserved capacity already covers the
  benchmark's scale.
- **Growing the pool and id index on demand.** The pre-v4 engine did this
  past its reserve: a 48 MB chunk allocation or a full ~1 GB rehash inside
  a `submit()`, i.e. a latency spike and a possible `bad_alloc` on the hot
  path. A hard cap with an explicit `kCapacityExceeded` reject keeps the
  "no hot-path allocation" rule unconditional and makes exhaustion visible
  to the caller.
- **Google Benchmark for `bench/`.** Used a small custom `chrono`-based
  driver instead, so the repo has zero network/build dependencies beyond
  GoogleTest for the test suite; it also let the workload generator produce
  a specific order-flow mix and print exactly the percentiles of interest.

## Assumptions

The API contract leaves a few corners to the implementation. These are the
readings this engine commits to, and the tests pin each of them down.

- An engine with no successful `configure()` rejects every `submit`,
  `cancel` and `replace` with `RejectReason::kNotConfigured`, before any
  other check. The id carried by that reject follows the usual rule
  (`new_order_id` for replace), and no `order_id` is consumed. This is the
  one exception to "validation order mirrors the enum order": the value is 7
  because appending keeps the existing enum values stable.
- `order_id` uniqueness is permanent and checked at the *uniqueness step*:
  an id is "spent" the moment it's found non-duplicate, even if a later
  validation step (quantity/TIF/tick/band) still rejects that submission.
  `replace()`'s `new_order_id` follows the same rule, except when it equals
  `order_id` (in-place replace) or when `new_quantity` collapses to zero
  open quantity ("simply canceled" — behaves exactly like `cancel()`, and
  `new_order_id` is never touched at all in that case).
  `replace()` re-validates tick/band *before* registering `new_order_id`,
  so a same-id replace that fails validation leaves the still-open original
  order reachable under its own id, exactly as "stays on the book,
  untouched" requires.
- `kCapacityExceeded` (value 8, appended like `kNotConfigured`) is the
  other exception to enum-order validation. `submit()` checks it right after
  the duplicate-id check: the id cannot be recorded, so it is *not*
  consumed, and resubmitting it yields `kCapacityExceeded` again rather than
  `kDuplicateOrderId`. `replace()` checks it after tick/band, as the last
  step before any state changes, so the original order stays as it was. A
  same-id replace needs a fresh slot too; a replace down to zero is a plain
  cancel and needs none.
- `replace()`'s "quantity increase" (which loses priority) is judged
  against the order's *current open quantity*, not its original submitted
  quantity — this is the reading under which a partially filled order being
  resized behaves unsurprisingly, and it is what the session scenario's
  step 5 locks in.
- A quantity-only replace can never newly cross the book: the resting-order
  invariant (best bid < best ask) was already intact at the unchanged
  price, so matching is only attempted when the price actually changes.

## What's next for production

Self-trade prevention and per-account risk checks (out of scope here, but
the natural next hot-path additions); a sequencer/journal for
crash recovery, since the pool is in-process memory only; NUMA-aware
per-instrument sharding across cores once a single core's throughput becomes
the bottleneck (today everything is intentionally single-threaded, which the
API contract assumes); replacing the id index's linear probing with SIMD-probed
buckets if profiling ever shows it, not the matching loop, as the hot spot.
