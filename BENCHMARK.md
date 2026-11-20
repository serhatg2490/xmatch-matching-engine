# BENCHMARK.md

## Environment

- CPU: Intel Core Ultra 5 125H (hybrid P+E cores), 18 logical CPUs, up to 4.5 GHz.
- RAM: 16 GB. On AC power.
- OS: Linux 7.0 (Ubuntu 24.04 userspace), GCC 13.3, `-O3 -DNDEBUG`, C++20.
- A shared development machine, not an isolated benchmark box: no
  `isolcpus`/`nohz_full`, no real-time scheduling, frequency scaling on.
  Runs are pinned with `taskset -c 4`. This mainly affects the `max` column.

## Method

`bench/benchmark_main.cpp` configures 120 instruments with reference prices
spread over 5–480, so several tick regimes are used. It then runs a seeded
(`std::mt19937_64`), deterministic mix:

- 55% new Day limit orders, priced near the reference so most rest
- 25% cancels
- 10% replaces
- 10% aggressive Limit IOC, Market IOC, Limit FOK and Market FOK

500,000 warm-up ops run untimed. Each of the 10,000,000 timed ops is one
`submit`/`cancel`/`replace` call, timed on its own with
`std::chrono::steady_clock`, single-threaded.

## Results

| seed | p50 (ns) | p90 (ns) | p99 (ns) | p99.9 (ns) | max (ns) | throughput (ops/s) |
|---|---|---|---|---|---|---|
| 42  | 235 | 438 | 658 | 909 | 30,601 | 2,542,394 |
| 123 | 234 | 435 | 644 | 887 | 62,728 | 2,561,172 |
| 777 | 234 | 434 | 639 | 886 | 27,189 | 2,562,682 |

- RSS is ~899 MB before and after the timed loop. The benchmark passes
  `max_orders = warmup_ops + num_ops` (10.5M) to `configure()`, which
  reserves the pool and id index once; nothing grows afterwards.
- Event mix (seed 42): 6.81M accepted, 1.78M rejected, 2.18M trades, 1.82M
  canceled, 88.7K replaced.

## Interpretation

- **p50–p99.9 are stable**, within a few nanoseconds across seeds: ~234 ns
  median, p99.9 under 1 µs, ~2.55M ops/sec on one core.
- **`max` is not reproducible.** It changes run to run without tracking any
  op type, which points to scheduler preemption on a non-isolated core, not
  the engine. Measuring true tail latency needs an isolated core.
- **Where the time goes.** Book navigation is O(1) (see `DESIGN.md`), so the
  cost is mainly the id-index lookup plus however many resting orders a
  crossing order fills. Most ops are one lookup and one level touch, which
  keeps p50 and p90 close; p99 and above come from multi-level IOC/FOK/market
  fills.

## Workload limitation

The generator keeps a simple list of "probably open" ids for cancel/replace
targets and does not track fills. This inflates `kUnknownOrder` rejects
(most of the rejects above) and lowers successful cancel/replace counts. It
does not affect timing: every op, accepted or rejected, is a real public-API
call.

## Reproducing

```sh
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
taskset -c <idle-core> ./build/engine_bench 10000000 120 500000 42
```
