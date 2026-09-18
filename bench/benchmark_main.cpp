// Standalone chrono-based benchmark driver. It models a continuous trading
// session across 100+ instruments with a deterministic, seeded operation mix
// (roughly 55% new limit orders, 25% cancels, 10% replaces and 10% aggressive
// IOC/FOK/market flow), runs single-threaded, discards a warm-up phase, and
// times every submit/cancel/replace call individually.
//
// A hand-rolled driver was preferred over Google Benchmark so that this file
// has zero build/network dependencies and doubles as a standalone
// reproduction tool -- see BENCHMARK.md for measured numbers and
// discussion, and README.md for how to run it.
#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <random>
#include <string>
#include <vector>

#include "xmatch/matching_engine_api.hpp"
#include "engine/tick_table.hpp"

using namespace xmatch;

namespace {

struct NullListener final : IEventListener {
    std::uint64_t accepted = 0, rejected = 0, trades = 0, canceled = 0, replaced = 0;
    void on_accepted(OrderId) override { ++accepted; }
    void on_rejected(OrderId, RejectReason) override { ++rejected; }
    void on_trade(TradeId, InstrumentId, Price, Quantity, OrderId, OrderId, Side) override { ++trades; }
    void on_canceled(OrderId, Quantity, CancelReason) override { ++canceled; }
    void on_replaced(OrderId, OrderId, Price, Quantity) override { ++replaced; }
};

std::size_t current_rss_bytes() {
    std::ifstream f("/proc/self/status");
    std::string line;
    while (std::getline(f, line)) {
        if (line.compare(0, 6, "VmRSS:") == 0) {
            return static_cast<std::size_t>(std::strtoull(line.c_str() + 6, nullptr, 10)) * 1024;
        }
    }
    return 0;
}

double percentile(const std::vector<std::uint64_t>& sorted_ns, double p) {
    if (sorted_ns.empty()) return 0.0;
    double idx = p * static_cast<double>(sorted_ns.size() - 1);
    std::size_t lo = static_cast<std::size_t>(idx);
    std::size_t hi = std::min(lo + 1, sorted_ns.size() - 1);
    double frac = idx - static_cast<double>(lo);
    return static_cast<double>(sorted_ns[lo]) * (1.0 - frac) + static_cast<double>(sorted_ns[hi]) * frac;
}

struct Args {
    std::uint64_t num_ops = 10'000'000;
    std::size_t num_instruments = 120;
    std::uint64_t warmup_ops = 500'000;
    std::uint64_t seed = 42;
};

Args parse_args(int argc, char** argv) {
    Args a;
    if (argc > 1) a.num_ops = std::strtoull(argv[1], nullptr, 10);
    if (argc > 2) a.num_instruments = static_cast<std::size_t>(std::strtoull(argv[2], nullptr, 10));
    if (argc > 3) a.warmup_ops = std::strtoull(argv[3], nullptr, 10);
    if (argc > 4) a.seed = std::strtoull(argv[4], nullptr, 10);
    return a;
}

} // namespace

int main(int argc, char** argv) {
    Args args = parse_args(argc, argv);

    std::printf("xmatch matching engine benchmark\n");
    std::printf("  ops=%llu warmup=%llu instruments=%zu seed=%llu\n",
                 static_cast<unsigned long long>(args.num_ops),
                 static_cast<unsigned long long>(args.warmup_ops),
                 args.num_instruments,
                 static_cast<unsigned long long>(args.seed));

    NullListener listener;
    IMatchingEngine* engine = xmatch_create(&listener);
    if (!engine) {
        std::fprintf(stderr, "xmatch_create failed\n");
        return 1;
    }

    // --- instrument setup (not timed; configure() is called once) --------
    std::mt19937_64 setup_rng(args.seed);
    std::uniform_real_distribution<double> price_try_dist(5.0, 480.0);

    std::vector<InstrumentConfig> configs;
    configs.reserve(args.num_instruments);
    for (std::size_t i = 0; i < args.num_instruments; ++i) {
        Price ref = static_cast<Price>(price_try_dist(setup_rng) * 10000.0);
        Price step = xmatch::detail::tick_size(ref);
        ref -= ref % step;
        if (ref <= 0) ref = step;
        configs.push_back(InstrumentConfig{static_cast<InstrumentId>(i + 1), ref, 1000u});
    }
    engine->configure(configs.data(), configs.size());

    std::vector<Price> inst_tick(args.num_instruments);
    for (std::size_t i = 0; i < args.num_instruments; ++i) {
        inst_tick[i] = xmatch::detail::tick_size(configs[i].reference_price);
    }

    // --- workload generator state ------------------------------------------
    std::mt19937_64 rng(args.seed ^ 0x9E3779B97F4A7C15ULL);
    std::uniform_int_distribution<int> op_pct(0, 99);
    std::uniform_int_distribution<std::size_t> inst_pick(0, args.num_instruments - 1);
    std::uniform_int_distribution<int> side_pick(0, 1);
    std::uniform_int_distribution<int> passive_offset_pick(1, 20);
    std::uniform_int_distribution<int> aggressive_offset_pick(20, 60);
    std::uniform_int_distribution<int> qty_pick(1, 500);
    std::uniform_int_distribution<int> submarket_pick(0, 3);

    // reserve() only reserves address space; on Linux that's backed by
    // lazily-committed (demand-zero) pages, so the *first* write to each
    // element would otherwise page-fault during the timed loop and show up
    // as driver noise in the tail latencies. resize()+clear() forces every
    // page to be touched (and physically resident) up front, and clear()
    // keeps the reserved capacity, so later push_back() calls just reuse
    // already-faulted-in memory.
    std::vector<OrderId> open_ids;
    open_ids.resize(args.num_ops + args.warmup_ops);
    open_ids.clear();
    OrderId next_id = 1;

    // Passive quotes sit close to the reference, offset to the correct side
    // of it (bids below, asks above) so most Day limit orders rest instead
    // of crossing -- approximating a real two-sided book with a stable
    // spread. New/cancel/replace therefore make up the book-building
    // majority, and the dedicated 10% bucket is the flow that actually
    // trades.
    auto gen_passive_price = [&](std::size_t inst_idx, Side side) -> Price {
        Price ref = configs[inst_idx].reference_price;
        Price delta = static_cast<Price>(passive_offset_pick(rng)) * inst_tick[inst_idx];
        Price p = (side == Side::kBuy) ? (ref - delta) : (ref + delta);
        if (p <= 0) p = inst_tick[inst_idx];
        return p;
    };
    // Aggressive quotes are pushed well past the reference on the crossing
    // side, so they reliably walk through the passive quotes generated above.
    auto gen_aggressive_price = [&](std::size_t inst_idx, Side side) -> Price {
        Price ref = configs[inst_idx].reference_price;
        Price delta = static_cast<Price>(aggressive_offset_pick(rng)) * inst_tick[inst_idx];
        Price p = (side == Side::kBuy) ? (ref + delta) : (ref - delta);
        if (p <= 0) p = inst_tick[inst_idx];
        return p;
    };

    std::vector<std::uint64_t> latencies_ns;
    latencies_ns.resize(args.num_ops); // pre-fault; see open_ids comment above
    latencies_ns.clear();

    // Runs exactly one op. If `timed`, times only the submit/cancel/replace
    // call itself and appends the sample; all random-parameter generation
    // and bookkeeping happens outside the timed window.
    auto run_one = [&](bool timed) {
        int r = op_pct(rng);
        if (r >= 55 && open_ids.empty()) r = 0; // no cancel/replace targets yet; fall back to a new order
        std::size_t inst_idx = inst_pick(rng);
        InstrumentId inst = static_cast<InstrumentId>(inst_idx + 1);

        if (r < 55) { // new limit day order
            NewOrder o{};
            o.order_id = next_id++;
            o.instrument_id = inst;
            o.side = side_pick(rng) ? Side::kSell : Side::kBuy;
            o.type = OrderType::kLimit;
            o.tif = TimeInForce::kDay;
            o.price = gen_passive_price(inst_idx, o.side);
            o.quantity = static_cast<Quantity>(qty_pick(rng));

            auto t0 = std::chrono::steady_clock::now();
            engine->submit(o);
            if (timed) latencies_ns.push_back(static_cast<std::uint64_t>(
                (std::chrono::steady_clock::now() - t0).count()));
            open_ids.push_back(o.order_id);

        } else if (r < 80) { // cancel
            std::uniform_int_distribution<std::size_t> pick(0, open_ids.size() - 1);
            std::size_t idx = pick(rng);
            CancelOrder c{open_ids[idx]};
            open_ids[idx] = open_ids.back();
            open_ids.pop_back();

            auto t0 = std::chrono::steady_clock::now();
            engine->cancel(c);
            if (timed) latencies_ns.push_back(static_cast<std::uint64_t>(
                (std::chrono::steady_clock::now() - t0).count()));

        } else if (r < 90) { // replace
            std::uniform_int_distribution<std::size_t> pick(0, open_ids.size() - 1);
            std::size_t idx = pick(rng);
            ReplaceOrder req{};
            req.order_id = open_ids[idx];
            req.new_order_id = next_id++;
            // We don't track the original order's side here (would need a
            // side-table keyed by id); reuse a freshly-picked side for the
            // passive-price bias, which is still representative since the
            // engine itself doesn't let replace() change side.
            req.new_price = gen_passive_price(inst_idx, side_pick(rng) ? Side::kSell : Side::kBuy);
            req.new_quantity = static_cast<Quantity>(qty_pick(rng));

            auto t0 = std::chrono::steady_clock::now();
            engine->replace(req);
            if (timed) latencies_ns.push_back(static_cast<std::uint64_t>(
                (std::chrono::steady_clock::now() - t0).count()));
            open_ids[idx] = req.new_order_id;

        } else { // marketable / IOC / market flow
            int kind = submarket_pick(rng);
            NewOrder o{};
            o.order_id = next_id++;
            o.instrument_id = inst;
            o.side = side_pick(rng) ? Side::kSell : Side::kBuy;
            o.quantity = static_cast<Quantity>(qty_pick(rng));
            switch (kind) {
                case 0: // aggressive limit IOC, biased to cross
                    o.type = OrderType::kLimit;
                    o.tif = TimeInForce::kIoc;
                    o.price = gen_aggressive_price(inst_idx, o.side);
                    break;
                case 1: // market IOC
                    o.type = OrderType::kMarket;
                    o.tif = TimeInForce::kIoc;
                    o.price = 0;
                    break;
                case 2: // aggressive limit FOK
                    o.type = OrderType::kLimit;
                    o.tif = TimeInForce::kFok;
                    o.price = gen_aggressive_price(inst_idx, o.side);
                    break;
                default: // market FOK
                    o.type = OrderType::kMarket;
                    o.tif = TimeInForce::kFok;
                    o.price = 0;
                    break;
            }

            auto t0 = std::chrono::steady_clock::now();
            engine->submit(o);
            if (timed) latencies_ns.push_back(static_cast<std::uint64_t>(
                (std::chrono::steady_clock::now() - t0).count()));
            // IOC/FOK never rest, so no open_ids bookkeeping needed here.
        }
    };

    for (std::uint64_t i = 0; i < args.warmup_ops; ++i) run_one(false);

    std::size_t rss_before = current_rss_bytes();
    auto wall_start = std::chrono::steady_clock::now();
    for (std::uint64_t i = 0; i < args.num_ops; ++i) run_one(true);
    auto wall_end = std::chrono::steady_clock::now();
    std::size_t rss_after = current_rss_bytes();

    double wall_seconds = std::chrono::duration<double>(wall_end - wall_start).count();
    double throughput = static_cast<double>(latencies_ns.size()) / wall_seconds;

    std::vector<std::uint64_t> sorted = latencies_ns;
    std::sort(sorted.begin(), sorted.end());
    std::uint64_t max_ns = sorted.empty() ? 0 : sorted.back();

    std::printf("\n--- results (%zu timed ops, wall=%.3fs) ---\n", sorted.size(), wall_seconds);
    std::printf("throughput: %.0f ops/sec\n", throughput);
    std::printf("p50:   %8.0f ns\n", percentile(sorted, 0.50));
    std::printf("p90:   %8.0f ns\n", percentile(sorted, 0.90));
    std::printf("p99:   %8.0f ns\n", percentile(sorted, 0.99));
    std::printf("p99.9: %8.0f ns\n", percentile(sorted, 0.999));
    std::printf("max:   %8llu ns\n", static_cast<unsigned long long>(max_ns));
    std::printf("RSS before timed loop: %.1f MB\n", rss_before / (1024.0 * 1024.0));
    std::printf("RSS after timed loop:  %.1f MB\n", rss_after / (1024.0 * 1024.0));
    std::printf("events: accepted=%llu rejected=%llu trades=%llu canceled=%llu replaced=%llu\n",
                 static_cast<unsigned long long>(listener.accepted),
                 static_cast<unsigned long long>(listener.rejected),
                 static_cast<unsigned long long>(listener.trades),
                 static_cast<unsigned long long>(listener.canceled),
                 static_cast<unsigned long long>(listener.replaced));

    xmatch_destroy(engine);
    return 0;
}
