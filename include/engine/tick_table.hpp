#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <limits>

#include "xmatch/matching_engine_api.hpp"

namespace xmatch::detail {

// Tick ladder: the minimum price increment widens as the price rises, the
// way cash-equity venues generally arrange it. The four brackets below are a
// deliberately small, self-contained table chosen for this engine; a real
// venue's published schedule has considerably more of them, and swapping one
// in means editing kTickSchedule and nothing else.
//
// All arithmetic is integer, in price units of 1/10000. Price math never goes
// through float or double.
//
//   0        <= price <  200000  (20.00) -> tick  100  (0.01)
//   200000   <= price <  500000  (50.00) -> tick  200  (0.02)
//   500000   <= price < 1000000 (100.00) -> tick  500  (0.05)
//   1000000  <= price                    -> tick 1000  (0.10)
//
// This table is the single source of truth for the schedule. PriceLadder
// segments a daily band by walking these same brackets, so a bracket added
// or moved here is picked up there with no second edit -- the arrangement
// the comment above promises. tick_table.cpp static_asserts the table's
// internal consistency at compile time.
struct TickBracket {
    Price upper_exclusive; // this bracket covers prices < upper_exclusive
    Price tick;
};

inline constexpr TickBracket kTickSchedule[] = {
    {200000, 100},
    {500000, 200},
    {1000000, 500},
    {std::numeric_limits<Price>::max(), 1000},
};

inline constexpr std::size_t kTickBracketCount =
    sizeof(kTickSchedule) / sizeof(kTickSchedule[0]);

// The widest tick in the schedule, i.e. the one the open-ended top bracket
// uses. Kept as a named constant so the fallback below states its intent.
inline constexpr Price kCoarsestTick = kTickSchedule[kTickBracketCount - 1].tick;

constexpr Price tick_size(Price price) noexcept {
    for (const TickBracket& bracket : kTickSchedule) {
        if (price < bracket.upper_exclusive) return bracket.tick;
    }
    return kCoarsestTick; // unreachable: the last bracket is open-ended
}

// True iff price > 0 and price is aligned to the tick of its bracket.
constexpr bool is_tick_aligned(Price price) noexcept {
    if (price <= 0) return false;
    return price % tick_size(price) == 0;
}

// Round UP to the nearest valid tick.
constexpr Price ceil_to_tick(Price price) noexcept {
    if (price <= 0) return tick_size(0);
    Price step = tick_size(price);
    Price rem = price % step;
    return rem == 0 ? price : price + (step - rem);
}

// Round DOWN to the nearest valid tick.
constexpr Price floor_to_tick(Price price) noexcept {
    if (price <= 0) return 0;
    Price step = tick_size(price);
    Price rem = price % step;
    return price - rem;
}

// An instrument's daily band, already pulled inward onto valid ticks.
struct BandLimits {
    Price lo; // lowest tick-aligned price in the band
    Price hi; // highest tick-aligned price in the band
};

// [reference_price * (1 - band_bps/10000), reference_price * (1 + band_bps/10000)],
// lower end rounded up and upper end rounded down onto a tick. Precondition
// (enforced by Engine::configure): 0 < reference_price <= INT64_MAX / kMaxBandBps
// and band_bps <= kMaxBandBps, so the product and the upper end fit in Price.
constexpr BandLimits band_limits(Price reference_price, std::uint32_t band_bps) noexcept {
    const Price delta = (reference_price * static_cast<Price>(band_bps)) / kBpsPerUnit;
    Price raw_lo = reference_price - delta;
    const Price raw_hi = reference_price + delta;
    if (raw_lo < 1) raw_lo = 1; // safety floor; band is normally well above zero
    return BandLimits{ceil_to_tick(raw_lo), floor_to_tick(raw_hi)};
}

// A run of prices that share one tick: lo, lo + tick, ..., hi. It is the part
// of a band that falls inside one bracket of kTickSchedule. Bounds are
// inclusive, like BandLimits.
struct TickSpan {
    Price lo;            // first price, tick-aligned
    Price hi;            // last price
    Price tick;
    std::uint64_t count; // prices in the span; 64-bit, so a huge band cannot wrap it
};

// The span that begins at `lo` (tick-aligned, <= band_hi) and runs to the end
// of lo's tick bracket or to band_hi, whichever comes first. PriceLadder::build
// and price_level_count both walk a band with this one function, so the level
// count configure() validates is exactly the number of levels the book
// later allocates.
constexpr TickSpan tick_span_at(Price lo, Price band_hi) noexcept {
    for (const TickBracket& bracket : kTickSchedule) {
        if (lo < bracket.upper_exclusive) {
            const Price hi = (bracket.upper_exclusive == std::numeric_limits<Price>::max())
                                 ? band_hi
                                 : std::min(band_hi, bracket.upper_exclusive - bracket.tick);
            return TickSpan{lo, hi, bracket.tick,
                            static_cast<std::uint64_t>((hi - lo) / bracket.tick) + 1};
        }
    }
    // unreachable: the last bracket is open-ended
    return TickSpan{lo, band_hi, kCoarsestTick,
                    static_cast<std::uint64_t>((band_hi - lo) / kCoarsestTick) + 1};
}

// Number of tick-aligned prices in [lo, hi], i.e. the ladder size for that
// band. Computed in 64 bits, so it is safe to call on a band far too wide to
// build; configure() compares it against kMaxPriceLevels before building.
constexpr std::uint64_t price_level_count(Price lo, Price hi) noexcept {
    std::uint64_t total = 0;
    for (Price cur = lo; cur <= hi;) {
        const TickSpan span = tick_span_at(cur, hi);
        total += span.count;
        cur = span.hi + span.tick;
    }
    return total;
}

} // namespace xmatch::detail
