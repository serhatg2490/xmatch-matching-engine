#include "engine/price_ladder.hpp"

#include <algorithm>
#include <limits>

#include "engine/tick_table.hpp"

namespace xmatch::detail {

void PriceLadder::build(Price band_lo, Price band_hi) {
    segments_.clear();
    total_count_ = 0;
    band_lo_ = band_lo;
    band_hi_ = band_hi;

    // Walk the one shared tick schedule (engine/tick_table.hpp) rather than a
    // private copy of its breakpoints, so adding or moving a bracket there
    // re-segments the ladder here automatically.
    Price cur = band_lo;
    std::uint32_t offset = 0;
    while (cur <= band_hi) {
        Price step = tick_size(cur);
        Price next_breakpoint = std::numeric_limits<Price>::max();
        for (const TickBracket& bracket : kTickSchedule) {
            if (bracket.upper_exclusive > cur) { next_breakpoint = bracket.upper_exclusive; break; }
        }
        Price regime_last = (next_breakpoint == std::numeric_limits<Price>::max())
                                 ? band_hi
                                 : std::min(band_hi, next_breakpoint - step);
        std::uint32_t count = static_cast<std::uint32_t>((regime_last - cur) / step) + 1;
        segments_.push_back(Segment{cur, regime_last, step, offset, count});
        offset += count;
        cur = regime_last + step;
    }
    total_count_ = offset;
}

std::int64_t PriceLadder::price_to_index(Price price) const {
    if (price < band_lo_ || price > band_hi_) return -1;
    for (const Segment& seg : segments_) {
        if (price >= seg.start_price && price <= seg.end_price) {
            Price rem = price - seg.start_price;
            if (rem % seg.tick != 0) return -1;
            return static_cast<std::int64_t>(seg.index_offset + rem / seg.tick);
        }
    }
    return -1;
}

Price PriceLadder::index_to_price(std::uint32_t index) const {
    for (const Segment& seg : segments_) {
        if (index >= seg.index_offset && index < seg.index_offset + seg.count) {
            return seg.start_price + static_cast<Price>(index - seg.index_offset) * seg.tick;
        }
    }
    return 0;
}

} // namespace xmatch::detail
