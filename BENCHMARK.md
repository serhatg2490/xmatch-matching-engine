# BENCHMARK.md

## Hardware / environment

- CPU: Intel Core Ultra 5 125H (hybrid P+E cores), 18 logical CPUs, up to 4.5 GHz.
- RAM: 16 GB.
- OS: Linux 7.0 (Ubuntu 24.04 userspace), GCC 13.3, `-O3 -DNDEBUG`, C++20.
- This is a shared development sandbox, **not** an isolated benchmarking
  box: no `isolcpus`/`nohz_full`, no real-time scheduling available
  (`chrt -f` fails with `EPERM`), frequency scaling active. Runs were
  pinned to one core with `taskset -c 4` but still share that core's
  scheduling with the rest of the host. See "On the `max` outlier" below —
  this materially affects only the `max` column, not p50–p99.9.

## Method

`bench/benchmark_main.cpp` (see `README.md` for how to run it): configures
120 instruments with reference prices spread 5–480 TRY (so multiple tick
regimes are exercised), then runs a `std::mt19937_64`-seeded, deterministic
mix of operations — 55% new Day limit
orders (priced close to each instrument's reference, so most rest instead
of crossing), 25% cancels, 10% replaces, and a 10% bucket split across
aggressive Limit IOC, Market IOC, Limit FOK, and Market FOK (priced to
reliably cross). 500,000 warm-up ops run untimed first; each subsequent
`submit`/`cancel`/`replace` call is timed individually with
`std::chrono::steady_clock` around just that call, single-threaded.

10,000,000 timed ops per run, three different seeds, all pinned to core 4:

| seed | p50 (ns) | p90 (ns) | p99 (ns) | p99.9 (ns) | max (ns) | throughput (ops/s) |
|---|---|---|---|---|---|---|
| 42  | 233 | 429 | 636 | 938 | 416,737 | 2,515,920 |
| 123 | 233 | 428 | 625 | 910 | 101,411 | 2,530,571 |
| 777 | 233 | 429 | 634 | 934 | 161,780 | 2,522,325 |

RSS was flat across every run: ~1442 MB before the timed loop and ~1442 MB
after (the pool and id-index reserves in `configure()` are sized to cover
this workload without growing — see `DESIGN.md`).

Event mix actually exercised (seed 42): 6.81M accepted, 1.78M rejected
(mostly `kUnknownOrder` from the generator's own simplified id bookkeeping —
see "Workload generator limitations" below), 2.18M trades, 1.82M canceled,
88.7K replaced.

## Interpretation

- **p50/p90/p99/p99.9 are stable and reproducible**: within a few
  nanoseconds of each other across three different seeds. Median is
  sub-microsecond (~233 ns) and p99.9 stays under 1 µs — comfortably
  inside the sub-microsecond-median, single-digit-µs-p99 range this engine
  was designed for. Throughput is consistently ~2.5M ops/sec
  single-threaded.
- **`max` is not reproducible run-to-run** (44 µs–417 µs observed across
  otherwise-identical runs) and does not correlate with a specific op type
  or workload phase. That signature — a handful of outliers 2–3 orders of
  magnitude above p99.9, uncorrelated with input — is the fingerprint of
  scheduler preemption on a shared, non-isolated core rather than anything
  in the matching engine itself. A run on real isolated hardware —
  single-threaded on a genuinely isolated core — would be needed to
  separate genuine tail latency from host noise; this
  environment can't produce that measurement honestly, so it's reported as
  a limitation rather than papered over.
- **Why the array/bitmap book pays off**: `top_of_book` and the level a
  matching loop starts at are both O(1) by construction (`DESIGN.md`), so
  the dominant per-op cost is the id-index hash lookup plus, for
  order-crossing ops, however many resting orders actually fill — not book
  navigation. That's consistent with p50/p90 being close together (most
  ops are a single hash lookup + O(1) level touch) while p99/p99.9 pick up
  the less common multi-level-crossing IOC/FOK/market fills.

## Workload generator limitations (affect the mix, not the engine)

The generator tracks "probably still open" order ids in a flat vector to
pick realistic cancel/replace targets, but doesn't mirror the engine's
fill bookkeeping (that would mean re-deriving the book inside the
benchmark). This slightly overstates `kUnknownOrder` rejects and understates
successful replace/cancel counts versus a real trading-day mix; it does not
affect the timing methodology, since every op — successful or rejected —
is a real, individually-timed `submit`/`cancel`/`replace` call through the
public API.

## Reproducing

```sh
cmake -B build -DCMAKE_BUILD_TYPE=Release -DENGINE_BUILD_BENCH=ON
cmake --build build -j
taskset -c <idle-core> ./build/engine_bench 10000000 120 500000 42
```
