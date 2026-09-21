#pragma once

#include <cstddef>
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

constexpr Price tick_size(Price price) {
    for (const TickBracket& bracket : kTickSchedule) {
        if (price < bracket.upper_exclusive) return bracket.tick;
    }
    return kCoarsestTick; // unreachable: the last bracket is open-ended
}

// True iff price > 0 and price is aligned to the tick of its bracket.
constexpr bool is_tick_aligned(Price price) {
    if (price <= 0) return false;
    return price % tick_size(price) == 0;
}

// Round UP to the nearest valid tick.
constexpr Price ceil_to_tick(Price price) {
    if (price <= 0) return tick_size(0);
    Price step = tick_size(price);
    Price rem = price % step;
    return rem == 0 ? price : price + (step - rem);
}

// Round DOWN to the nearest valid tick.
constexpr Price floor_to_tick(Price price) {
    if (price <= 0) return 0;
    Price step = tick_size(price);
    Price rem = price % step;
    return price - rem;
}

} // namespace xmatch::detail
