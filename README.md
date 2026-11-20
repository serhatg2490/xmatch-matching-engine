# xmatch — a low-latency equity matching engine in C++20

A single-threaded, price/time-priority matching engine for a continuous
cash-equity session. It ships as a shared library (`libmatching_engine.so`)
behind a three-symbol C ABI, so a host can link it or `dlopen()` it.

`submit`/`cancel`/`replace` do no heap allocation, syscalls, logging or
exception handling. On a laptop-class x86-64 core they run at a ~234 ns
median, p99.9 under 1 µs, ~2.55M ops/sec. See [`BENCHMARK.md`](BENCHMARK.md)
for the method and numbers, and [`DESIGN.md`](DESIGN.md) for the data
structures.

## Features

- Price/time priority across many instruments.
- Limit and market orders; Day, IOC and FOK time-in-force.
- Atomic cancel and replace. A reprice or size increase goes to the back of
  the queue; a size reduction at the same price keeps its place.
- Tick-size ladder and a daily price band around a reference price, enforced
  on entry and on replace.
- Validated, retryable `configure()` that returns a `ConfigureResult`. An
  unconfigured engine rejects every order with `kNotConfigured`.
- Fixed order capacity: memory for `max_orders` is reserved up front; once
  it is used up, orders are rejected with `kCapacityExceeded`.
- Overflow-safe sizes: a band spans at most `kMaxPriceLevels` (2²⁴) price
  levels, and aggregate quantities are 64-bit (`AggregateQuantity`).
- Integer fixed-point prices; no floating point in price math.
- Deterministic: the same inputs always produce the same event stream.
- Synchronous events: every callback fires before the triggering call
  returns.

Out of scope: auctions, circuit breakers, self-trade prevention,
accounts/positions, persistence, networking, multithreading.

## Build

Requires CMake ≥ 3.20 and GCC ≥ 12 or Clang ≥ 15 on Linux x86-64.

```sh
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
```

This builds `build/libmatching_engine.so`, which exports only
`xmatch_api_version`, `xmatch_create` and `xmatch_destroy`. The engine uses
only the standard library. Tests fetch GoogleTest via `FetchContent` (network
needed once). To build just the library:

```sh
cmake -B build -DCMAKE_BUILD_TYPE=Release -DENGINE_BUILD_TESTS=OFF -DENGINE_BUILD_BENCH=OFF
```

## Usage

The whole public API (version 5) is
[`include/xmatch/matching_engine_api.hpp`](include/xmatch/matching_engine_api.hpp);
its comments are the specification.

```cpp
#include "xmatch/matching_engine_api.hpp"

using namespace xmatch;

struct MyListener final : IEventListener {
    void on_accepted(OrderId) noexcept override {}
    void on_rejected(OrderId, RejectReason) noexcept override {}
    void on_trade(TradeId, InstrumentId, Price, Quantity,
                  OrderId, OrderId, Side) noexcept override {}
    void on_canceled(OrderId, Quantity, CancelReason) noexcept override {}
    void on_replaced(OrderId, OrderId, Price, Quantity) noexcept override {}
};

MyListener listener;
IMatchingEngine* engine = xmatch_create(&listener);

InstrumentConfig cfg{.instrument_id = 1,
                     .reference_price = 250000, // 25.0000
                     .band_bps = 1000};         // +/- 10%
if (engine->configure(&cfg, 1, /*max_orders=*/1'000'000) != ConfigureResult::kOk) {
    // nothing was applied; fix the configuration and call configure() again
}

engine->submit(NewOrder{.order_id = 1,
                        .instrument_id = 1,
                        .side = Side::kBuy,
                        .type = OrderType::kLimit,
                        .tif = TimeInForce::kDay,
                        .price = 251000, // 25.1000
                        .quantity = 100});

xmatch_destroy(engine);
```

## Tests

```sh
./build/engine_api_tests       # public API, through libmatching_engine.so
./build/engine_internal_tests  # internal building blocks
ctest --test-dir build         # both
```

They cover an end-to-end session asserted event for event, configuration
validation, reject reasons and their order, matching and TIF semantics,
cancel/replace edge cases, multi-instrument isolation, overflow limits, and
the internal building blocks.

## Benchmark

```sh
taskset -c 4 ./build/engine_bench [num_ops] [num_instruments] [warmup_ops] [seed]
# defaults:                        10000000  120               500000       42
```

Runs a seeded mix of new orders, cancels, replaces and aggressive IOC/FOK/
market orders, times every call, and prints latency percentiles, throughput
and RSS. Pin it to an idle core.

## Layout

```
include/xmatch/   public API header
include/engine/   internal headers (tick table, price ladder, order book, pool, hash map, engine)
src/              implementation and the extern "C" entry points
tests/            GoogleTest suites
bench/            latency/throughput benchmark
```

## Tools

C++20, GCC 13, CMake 3.28, GoogleTest 1.15.2. Developed with Claude
(Anthropic) as a pair-programming aid; all design decisions, code and
documentation were reviewed by the author.

## License

MIT — see [`LICENSE`](LICENSE). Copyright (c) 2025 Serhat Gul.
