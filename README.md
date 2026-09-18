# xmatch — a low-latency equity matching engine in C++20

A single-threaded, price/time-priority matching engine for a continuous
cash-equity trading session, built as a shared library (`libmatching_engine.so`)
with a small C ABI so a host program can link it or `dlopen()` it.

The design goal was a sub-microsecond median `submit`/`cancel`/`replace`, with
no heap allocation, syscalls, logging, or exceptions on the hot path. On a
laptop-class x86-64 core it measures a ~233 ns median and a p99.9 under 1 µs at
~2.5M ops/sec single-threaded — see [`BENCHMARK.md`](BENCHMARK.md) for the full
numbers, the method, and an honest account of what this environment can and
cannot measure.

See [`DESIGN.md`](DESIGN.md) for the data-structure rationale: an
array-indexed price ladder bounded by the daily price band, a bitset for
best-price relocation, an intrusive FIFO inside a chunked order pool, and a
purpose-built open-addressing id index.

## Feature set

- Continuous trading with price/time priority across many instruments
  (tested at 150, benchmarked at 120).
- Limit and market orders; Day, IOC and FOK time-in-force.
- Atomic cancel and replace, with correct time-priority semantics — a
  reprice or size increase goes to the back of the queue, a size reduction
  at the same price keeps its place.
- A tick-size ladder (coarser ticks at higher prices) and a daily price band
  derived from a reference price, both enforced on entry and on replace.
- Fixed-point prices throughout; no floating-point price arithmetic anywhere.
- Deterministic: the same configuration and operation sequence always
  produces the same event stream.
- Synchronous event delivery — every callback has fired before the
  triggering call returns, which is what makes call-site latency
  measurement meaningful.

Deliberately out of scope: opening/closing auctions, circuit breakers,
self-trade prevention, accounts and positions, persistence, and networking.

## Build

Requires CMake >= 3.20 and GCC >= 12 or Clang >= 15 on Linux x86-64.

```sh
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
```

This produces `build/libmatching_engine.so`, exporting exactly three C
symbols:

```sh
nm -D build/libmatching_engine.so | grep " T xmatch"
# xmatch_api_version
# xmatch_create
# xmatch_destroy
```

The engine itself has no dependencies beyond the standard library.
`ENGINE_BUILD_TESTS` (default `ON`) fetches GoogleTest via CMake
`FetchContent` on first configure (needs network access once); set it to
`OFF` to build only the shared library:

```sh
cmake -B build -DCMAKE_BUILD_TYPE=Release -DENGINE_BUILD_TESTS=OFF -DENGINE_BUILD_BENCH=OFF
```

## Usage

The public API is [`include/xmatch/matching_engine_api.hpp`](include/xmatch/matching_engine_api.hpp).
Implement `IEventListener`, create an engine, declare your instruments once,
then submit:

```cpp
#include "xmatch/matching_engine_api.hpp"

using namespace xmatch;

struct MyListener final : IEventListener {
    void on_accepted(OrderId) override {}
    void on_rejected(OrderId, RejectReason) override {}
    void on_trade(TradeId, InstrumentId, Price, Quantity,
                  OrderId, OrderId, Side) override {}
    void on_canceled(OrderId, Quantity, CancelReason) override {}
    void on_replaced(OrderId, OrderId, Price, Quantity) override {}
};

MyListener listener;
IMatchingEngine* engine = xmatch_create(&listener);

InstrumentConfig cfg{.instrument_id = 1,
                     .reference_price = 250000, // 25.0000
                     .band_bps = 1000};         // +/- 10%
engine->configure(&cfg, 1);

engine->submit(NewOrder{.order_id = 1,
                        .instrument_id = 1,
                        .side = Side::kBuy,
                        .type = OrderType::kLimit,
                        .tif = TimeInForce::kDay,
                        .price = 251000, // 25.1000
                        .quantity = 100});

xmatch_destroy(engine);
```

## Run the tests

```sh
cmake --build build -j
./build/engine_tests
```

53 tests across 10 suites: an end-to-end golden scenario asserted event for
event (`test_golden_scenario.cpp`), validation ordering and reject reasons,
matching/priority/TIF semantics, cancel/replace edge cases (priority
retention, replace-to-zero, tick/band re-validation, duplicate and self
`new_order_id`), multi-instrument isolation at 150 instruments, and focused
unit tests for the internal building blocks (hash map, order pool, price
ladder, tick table, level bitset).

## Run the benchmark

```sh
./build/engine_bench [num_ops] [num_instruments] [warmup_ops] [seed]
# defaults:   10000000        120               500000       42
```

Drives 100+ instruments with a deterministic seeded mix (~55% new limit /
25% cancel / 10% replace / 10% marketable IOC/FOK/market flow), times every
`submit`/`cancel`/`replace` call individually, and prints p50/p90/p99/p99.9/max
latency, throughput, and RSS. For best results, pin it to an otherwise-idle
core:

```sh
taskset -c 4 ./build/engine_bench
```

## Repository layout

```
include/xmatch/           public API header (the C ABI and event contract)
include/engine/           internal headers (hash map, order pool, tick
                           table, price ladder, order book, engine)
src/                      implementation + the extern "C" factory functions
tests/                    GoogleTest suite, including the golden scenario
bench/                    standalone latency/throughput benchmark
```

## Tools and resources used

- Language/toolchain: C++20, GCC 13 (Ubuntu 24.04), CMake 3.28.
- Testing: GoogleTest 1.15.2 (fetched via CMake `FetchContent`).
- No third-party runtime dependencies in the engine itself — standard
  library only.
- AI assistance: this implementation was developed with Claude (Anthropic)
  as a pair-programming and code-generation aid. Every design decision, and
  the code and documentation that came out of it, was reviewed by the
  author.

## License

MIT — see [`LICENSE`](LICENSE). Copyright (c) 2025 Serhat Gul.
