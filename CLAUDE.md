# CLAUDE.md

Keep your replies extremely concise and focus on conveying the key information. No unnecessary fluff, no long code snippets.

Whenever working with any third-party library or something similar, you MUST look up the official documentation to ensure that you're working with up-to-date information.

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project

`xmatch` — a single-threaded, price/time-priority matching engine for a continuous
cash-equity session, shipped as `libmatching_engine.so` behind a three-symbol C ABI
(`xmatch_api_version`, `xmatch_create`, `xmatch_destroy`). C++20, Linux x86-64,
standard library only; GoogleTest is built only for the test target.

`README.md` is the usage/feature overview 
`DESIGN.md` the data-structure design and the
`BENCHMARK.md` the measured numbers and method.
Keep all three in sync when behaviour or structure changes.

## Build, test, benchmark

```sh
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j

./build/engine_tests    # whole suite
./build/engine_tests --gtest_filter='CancelReplaceTest.*'   # one suite
./build/engine_tests --gtest_filter='GoldenScenario.*'    # one test
ctest --test-dir build    # same tests via gtest_discover_tests

./build/engine_bench [num_ops] [num_instruments] [warmup_ops] [seed]   # defaults 10000000 120 500000 42
taskset -c <idle-core> ./build/engine_bench   # pin for meaningful latency numbers
```

Library only build (no network requirement, no GoogleTest):

```sh
cmake -B build -DCMAKE_BUILD_TYPE=Release -DENGINE_BUILD_TESTS=OFF -DENGINE_BUILD_BENCH=OFF
```

`-DENGINE_NATIVE_ARCH=ON` adds `-march=native`; it is off by default so builds stay portable.

Binaries get a build-tree RUNPATH, so `./build/engine_tests` resolves the `.so` with no
`LD_LIBRARY_PATH`.

`configure()` pre-reserves 2^24 order slots plus the id index, so *any* process that
creates an engine — including each test binary run — sits at ~1.4 GB RSS. That is by
design (no hot-path allocation), not a leak.

## Architecture

Layering, outermost first:

- `include/xmatch/matching_engine_api.hpp` — the entire public contract: scalar types,
  enums, `IEventListener`, `IMatchingEngine`, and the `extern "C"` entry points. It also
  carries the normative prose (validation order, replace semantics, event ordering,
  determinism guarantee). Treat its comments as the spec; the tests assert against them.
- `src/engine_api.cpp` — the C ABI factory. Exports exactly the three symbols.
- `src/engine.cpp` / `include/engine/engine.hpp` — `detail::Engine`: validation, the
  matching loop, FOK feasibility pre-check, cancel and replace. Owns the shared
  `OrderPool`, the `order_id → slot` index, the `instrument_id → book` index, and the
  `TradeId` counter.
- `src/order_book.cpp` / `order_book.hpp` — one book per instrument: two flat
  `std::vector<Level>` arrays (bid/ask) indexed by ladder index, two `LevelBitset`s,
  and a cached best index per side.
- Building blocks in `include/engine/`: `PriceLadder` (price↔dense index, up to 4
  tick-regime segments), `TickTable` (`kTickSchedule`, the single source of truth
  for the 4-bracket tick ladder, plus `constexpr` free functions over it;
  `src/tick_table.cpp` holds only compile-time `static_assert`s on the table,
  and `PriceLadder` segments bands by walking that same table),
  `OrderPool` (chunked append-only `OrderRecord` store, intrusive FIFO links),
  `LevelBitset` (word-skipping occupancy bitmap), `FlatHashMap` (open-addressing,
  insert/find only).

The load-bearing idea: an instrument's daily price band, fixed at `configure()` time,
bounds the set of prices a resting order can ever have. So the book is an array indexed
by arithmetic instead of a tree — `price_to_index`, `add_resting`, `remove_resting` and
`top_of_book` are all O(1). Everything else (bitset for best-price relocation, intrusive
list for FIFO, append-only pool, custom hash map) exists to keep that path allocation-free.

## Invariants to preserve

These are contract, not style. Changing any of them means changing the API header's
prose and the tests together.

- **No heap allocation, syscall, lock, logging or exception on `submit`/`cancel`/`replace`
  success paths.** All large allocation belongs in `configure()`.
- **No exception crosses the C ABI.** Every public `Engine` method body is wrapped in
  `try { ... } catch (...) {}`. This is the one place blanket catching is correct.
- **Integer price arithmetic only** — `Price` is `int64_t` at 1/10000 units. No `float`
  or `double` anywhere near tick, band, or trade-price math.
- **Pool slots are never recycled and `FlatHashMap` never erases.** `order_id` is unique
  for the engine's lifetime, so `replace()` allocates a *fresh* slot for `new_order_id`
  rather than renaming the old record.
- **Validation order is fixed** and mirrors the `RejectReason` enum order. An `order_id`
  is consumed at the uniqueness step — it stays spent even if a later step rejects the
  submission.
- **`replace()` re-validates tick/band before registering `new_order_id` or allocating**,
  so a failed same-id replace leaves the original order open and reachable under its own id.
  Priority is judged against *current open* quantity: reprice or size increase goes to the
  back of the queue; a same-price size reduction keeps its place via
  `OrderBook::replace_in_place`, which splices the new slot into the old one's exact
  list position. A `new_quantity` that collapses open quantity to zero is a plain cancel
  and never touches `new_order_id`.
- **Events are synchronous** — all callbacks fire before the triggering call returns.
  That is what makes call-site latency timing meaningful; don't introduce queuing.
- **Determinism**: no dependence on wall-clock, addresses, hash iteration order, or
  uninitialized memory. `TradeId` starts at 1 and increments by 1 (the test listener
  asserts this on every trade).

## Tests

`tests/test_helpers.hpp` carries `RecordingListener` (captures an event stream),
`RecordedEvent` with kind-aware equality, the `Accepted`/`Rejected`/`Trade`/`Canceled`/
`Replaced` constructors, `Px(major, minor4)`, and the `EngineHandle` RAII wrapper around
`xmatch_create`/`xmatch_destroy`. Tests drive the engine through the public API only —
except `test_internal_structures.cpp`, which unit-tests the `detail::` building blocks
directly.

`test_golden_scenario.cpp` asserts one end-to-end run event-for-event against an exact
expected vector; it is the regression net for event ordering and replace semantics, so
expect it to fail first when either changes. New behaviour that the API header describes
should also get a targeted test in the matching/validation/cancel-replace file it belongs to.

## Scope

Deliberately out of scope, and should stay that way unless asked: opening/closing
auctions, circuit breakers, self-trade prevention, accounts/positions, persistence,
networking, multithreading.

## Documentation

### Feed Documentation

If the user prompts to generate project documentation for posting to feeds, use the FeedImageCreator.md subagent for creation of visual image files and use the FeedTextCreator.md subagent for creation of text files.

Put all the files in the docs folder in the main directory of the project. If does not exist, create
that folder.
