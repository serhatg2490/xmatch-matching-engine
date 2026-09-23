# Practices

Each item: rule → why → bad/good → anchor in the project.
The examples are there to show the difference; they don't need to compile.

## Design principles

### 1. Keep the public interface narrow

Every symbol you expose is a permanent contract.

```cpp
OrderPool* xmatch_get_pool(Engine*);              // bad  — leaks an internal type
IMatchingEngine* xmatch_create(IEventListener*);  // good — opaque pointer
```

In the project: `include/xmatch/matching_engine_api.hpp` — the library sits behind three C symbols.

### 2. Give each type a single responsibility

Combined types can't be tested or changed independently.

```cpp
class OrderBook { Price index_to_price(...); OrderRecord* allocate(); };  // bad
class PriceLadder { /* price <-> index */ };   // good — three separate building blocks
class OrderPool   { /* slot allocation */ };
class OrderBook   { /* levels, FIFO    */ };
```

In the project: `include/engine/` — the `price_ladder` / `order_pool` / `order_book` split.

### 3. Single source of truth

If the same information is defined in two places, sooner or later one of them is left un-updated.

```cpp
Price breaks[] = {200000, 500000, 1000000};        // bad  — a second copy of the table
for (const TickBracket& b : kTickSchedule) { ... } // good — walk the one table
```

In the project: `include/engine/tick_table.hpp` is the single source, and `src/price_ladder.cpp`
walks it. This divergence was a real bug (commit `d6627e0`).

### 4. Move the contract to compile time

Don't leave an error the compiler could catch to run time.

```cpp
// OrderRecord must be 48 bytes, the docs rely on it        // bad  — wishful thinking
static_assert(sizeof(OrderRecord) == 48, "see DESIGN.md"); // good — the build breaks
```

In the project: `include/engine/order_pool.hpp:37`, `include/engine/order_book.hpp:27`.

## Performance and scalability

### 5. Measure first, then optimize

Guess-based optimization complicates the code, and the gain is often zero.

```cpp
// bad : the "if I unroll it by hand it'll be faster" assumption, no measurement
// good: measure before/after the change, verify the generated code, write down why
//       "on GCC 13 -O3 a bare bsr remains, no extra branch — checked with objdump"
```

In the project: `include/engine/level_bitset.hpp` — the switch to `<bit>` was verified by
inspecting the generated code; the method and numbers are in `BENCHMARK.md`.

### 6. Complexity comes before micro-optimization

Reducing O(log n) to O(1) is worth more than tweaking a constant factor.

```cpp
std::map<Price, Level> levels_;    // bad  — every access walks a tree
std::vector<Level> levels_;        // good — levels_[ladder_.price_to_index(p)]
```

In the project: `include/engine/order_book.hpp` — `price_to_index`, `add_resting` and
`top_of_book` are all O(1). This is possible because the daily price band bounds the set of indices.

### 7. No allocation, syscall, lock or logging on the hot path

A single allocation or syscall pushes p99 into microseconds.

```cpp
void Engine::submit(...) { orders_.push_back(OrderRecord{}); }  // bad  — may grow
void Engine::configure(...) { pool_.reserve(kDefaultOrderCapacity); }  // good
void Engine::submit(...) { std::uint32_t slot = pool_.allocate(); }    //      advances an index
```

In the project: `src/engine.cpp` — `configure()` pre-allocates the pool and the id index.

### 8. Think about data layout

Scanning a contiguous array is many times faster than chasing pointers; frequently
touched structs must stay narrow.

```cpp
struct Level { std::list<Order*> orders; };   // bad  — a cache miss at every step
struct Level { std::uint32_t head, tail; Quantity total_qty;
               std::uint32_t order_count; }; // good — 16 bytes, no padding
```

In the project: `include/engine/order_book.hpp` — `Level` is 16 bytes, in two flat arrays.

## Readability

### 9. Use early returns

Nested conditions multiply the reading load and make reordering hard.

```cpp
if (book) { if (!dup) { if (qty) {...} else reject(...); } else reject(...); }  // bad

if (!book)   { reject(kUnknownInstrument); return; }   // good
if (dup)     { reject(kDuplicateOrderId);  return; }
if (qty == 0){ reject(kInvalidQuantity);   return; }
```

In the project: `src/engine.cpp` `submit()` — the guard chain maps one-to-one onto the
`RejectReason` enum order, so the contract can be read from the code.

### 10. Let names state intent; no magic numbers

A bare constant forces the reader to go looking for where the value came from.

```cpp
if (rec.next == 0xFFFFFFFFu) ...  pool_.reserve(16777216);              // bad
if (rec.next == kInvalidSlot) ... pool_.reserve(kDefaultOrderCapacity); // good
```

In the project: `kInvalidSlot`, `kChunkSize` (`order_pool.hpp`), `kDefaultOrderCapacity`
(`engine.cpp`).

### 11. Keep a function at a single level of abstraction

Mixing high-level flow with bit-level detail makes a function untestable.

```cpp
while (...) { std::uint64_t w = words_[i >> 6] & mask; ... }        // bad
while (agg.open_qty > 0 && book.has_best(opp)) { ... }              // good
```

In the project: `src/engine.cpp` `run_matching()` carries only the flow; the word scan
stays inside `LevelBitset`.

### 12. Comments should explain "why"

"What it does" can be read from the code; a comment's job is to say what the code can't.

```cpp
// check whether the word is non-zero                                // bad
// The guard must stay BEFORE the call: GCC proves w != 0 from it    // good
// and emits a bare bsr. Remove it and the zero test comes back as a branch.
if (w) return ...;
```

In the project: the header comment of `include/engine/level_bitset.hpp`.

## Security and robustness

### 13. Don't trust external input; validate at the boundary

Validation must happen in one place and in a fixed order, and the reason for a rejection must be reported to the caller.

```cpp
book->add_resting(pool_, slot, o.side, o.price);      // bad  — unvalidated input

if (!is_tick_aligned(price))       { reject(kInvalidPriceTick); return; }  // good
if (book->price_to_index(price)<0) { reject(kPriceOutOfBand);   return; }
```

In the project: `src/engine.cpp` — `submit()` and `replace()` apply the same order.

### 14. Be careful with integer arithmetic

Floats silently drift in money calculations; mixing signed/unsigned silently wraps.

```cpp
double px = 12.34;  for (int i = 0; i < levels_.size(); ++i)          // bad
Price px = 123400;  for (std::size_t i = 0; i < levels_.size(); ++i)  // good
```

In the project: `Price` = `int64_t` @ 1/10000 units; no `float`/`double` in price, tick or
band math. (`kMaxLoadFactor` in `flat_hash_map.hpp` is a capacity calculation that is
off the hot path and unrelated to prices — the rule is about price math.)

### 15. Guarantee bounds on index access

An index's validity must be proven where it is computed, not assumed where it is used.

```cpp
levels_[ladder_.price_to_index(price)].total_qty += qty;   // bad  — may return -1

std::int64_t idx = ladder_.price_to_index(price);          // good — sentinel made explicit
if (idx < 0) { reject(kPriceOutOfBand); return; }
```

In the project: `src/price_ladder.cpp` — `-1` on a band or tick violation; `submit()` checks
it before the order rests.

### 16. Don't swallow errors silently

`catch (...)` is a boundary guard, not error handling; used inside, it hides bugs.

```cpp
void OrderBook::add_resting(...) { try { ... } catch (...) {} }   // bad

IMatchingEngine* xmatch_create(IEventListener* l) {               // good
    try { return new detail::Engine(l); }
    catch (...) { return nullptr; }   // an exception can't cross the C boundary
}
```

In the project: `src/engine_api.cpp` and the public methods of `detail::Engine` — nowhere
else.
