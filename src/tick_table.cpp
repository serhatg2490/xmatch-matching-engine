#include "engine/tick_table.hpp"

// The tick functions are constexpr and live in the header, so this
// translation unit carries no runtime code. What it does carry is the
// schedule's invariants, checked at compile time: if someone swaps in a
// different venue's ladder and gets it wrong, the build fails here rather
// than the engine mispricing a tick at run time.

namespace xmatch::detail {
namespace {

// Brackets must be ascending and non-overlapping, every tick positive, and
// the final bracket open-ended so tick_size() always finds a match.
constexpr bool schedule_is_well_formed() {
    for (std::size_t i = 0; i < kTickBracketCount; ++i) {
        if (kTickSchedule[i].tick <= 0) return false;
        if (i > 0 && kTickSchedule[i].upper_exclusive <= kTickSchedule[i - 1].upper_exclusive) {
            return false;
        }
    }
    return kTickSchedule[kTickBracketCount - 1].upper_exclusive ==
           std::numeric_limits<Price>::max();
}

// Each bracket's lower edge must sit on its own tick. Otherwise a band that
// starts exactly at a regime boundary would begin off-tick, and PriceLadder
// would build a segment whose first price no order can ever hold.
constexpr bool bracket_edges_are_tick_aligned() {
    for (std::size_t i = 1; i < kTickBracketCount; ++i) {
        const Price edge = kTickSchedule[i - 1].upper_exclusive;
        if (edge % kTickSchedule[i].tick != 0) return false;
    }
    return true;
}

static_assert(kTickBracketCount >= 1, "the tick schedule must have a bracket");
static_assert(schedule_is_well_formed(),
              "kTickSchedule must be ascending, positively ticked, and open-ended at the top");
static_assert(bracket_edges_are_tick_aligned(),
              "each tick bracket must begin on a multiple of its own tick");

// Spot checks pinning the documented ladder, evaluated at compile time.
static_assert(tick_size(1) == 100);
static_assert(tick_size(199900) == 100);
static_assert(tick_size(200000) == 200);
static_assert(tick_size(499800) == 200);
static_assert(tick_size(500000) == 500);
static_assert(tick_size(999500) == 500);
static_assert(tick_size(1000000) == 1000);

static_assert(is_tick_aligned(251000));
static_assert(!is_tick_aligned(251100));
static_assert(!is_tick_aligned(0));
static_assert(!is_tick_aligned(-100));

static_assert(ceil_to_tick(251001) == 251200);
static_assert(ceil_to_tick(251000) == 251000);
static_assert(floor_to_tick(251199) == 251000);
static_assert(floor_to_tick(251000) == 251000);

} // namespace
} // namespace xmatch::detail
