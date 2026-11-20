#include "engine/price_ladder.hpp"

#include <cstdint>
#include <stdexcept>

#include "engine/tick_table.hpp"

namespace xmatch::detail {

void PriceLadder::build(Price band_lo, Price band_hi) {
    // Engine::configure rejects such a band with kInvalidArgument before it
    // gets here. Checking again costs nothing at configure time, and it
    // keeps the uint32 indices below from wrapping in a release build,
    // where an assert would compile away.
    if (price_level_count(band_lo, band_hi) > kMaxPriceLevels) {
        throw std::length_error("PriceLadder: band spans more than kMaxPriceLevels levels");
    }

    std::vector<Segment> segments;
    segments.reserve(kTickBracketCount);

    // Walk the one shared tick schedule (engine/tick_table.hpp) rather than a
    // private copy of its breakpoints, so adding or moving a bracket there
    // re-segments the ladder here automatically. price_level_count() walks
    // it with the same tick_span_at(), so the two always agree.
    std::uint64_t next_index = 0;
    for (Price cur = band_lo; cur <= band_hi;) {
        const TickSpan span = tick_span_at(cur, band_hi);
        // Fits in uint32: the total is <= kMaxPriceLevels (checked above).
        segments.push_back(Segment{span, static_cast<std::uint32_t>(next_index)});
        next_index += span.count;
        cur = span.hi + span.tick;
    }
    // Commit only after every allocating step has succeeded (strong guarantee)
    segments_.swap(segments);
    total_count_ = static_cast<std::uint32_t>(next_index);
    band_lo_ = band_lo;
    band_hi_ = band_hi;
}

std::int64_t PriceLadder::price_to_index(Price price) const noexcept {
    if (price < band_lo_ || price > band_hi_) return -1;
    for (const Segment& seg : segments_) {
        if (price >= seg.span.lo && price <= seg.span.hi) {
            Price rem = price - seg.span.lo;
            if (rem % seg.span.tick != 0) return -1;
            return static_cast<std::int64_t>(seg.first_index + rem / seg.span.tick);
        }
    }
    return -1;
}

Price PriceLadder::index_to_price(std::uint32_t index) const noexcept {
    for (const Segment& seg : segments_) {
        if (index >= seg.first_index && index < seg.first_index + seg.span.count) {
            return seg.span.lo + static_cast<Price>(index - seg.first_index) * seg.span.tick;
        }
    }
    return 0;
}

} // namespace xmatch::detail
