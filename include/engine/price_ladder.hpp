#pragma once

#include <cstdint>
#include <optional>
#include <vector>

#include "xmatch/matching_engine_api.hpp"
#include "engine/tick_table.hpp"

namespace xmatch::detail {

// Maps every tick-aligned price within an instrument's daily price band to
// a dense array index, and back. Because limit orders are validated against
// the band before they can ever rest on the book, this ladder covers every
// price a resting order can possibly have -- letting the order book use
// flat arrays (+ bitmaps) for price levels instead of a tree/map.
//
// A band can straddle up to 4 tick regimes only when the reference price
// sits close to one of the regime boundaries (20/50/100 TRY); we handle
// that by keeping a handful of contiguous segments, one per regime the
// band touches.
// Ladder indices are uint32 (Level arrays, OrderRecord::level_index) and the
// book's cached best index is int64 with -1 meaning "empty". Capping the
// ladder at kMaxPriceLevels keeps every index, and offset + count, in range.
static_assert(kMaxPriceLevels < UINT32_MAX, "ladder indices must fit in uint32 with room to spare");

class PriceLadder {
public:
    // Throws std::length_error if [band_lo, band_hi] holds more than
    // kMaxPriceLevels tick-aligned prices; may throw bad_alloc.
    void build(Price band_lo, Price band_hi);

    // -1 if price is outside the band or not tick-aligned within it.
    std::int64_t price_to_index(Price price) const noexcept;

    Price index_to_price(std::uint32_t index) const noexcept;

    std::uint32_t size() const noexcept { return total_count_; }

    Price band_lo() const noexcept { return band_lo_; }
    Price band_hi() const noexcept { return band_hi_; }

private:
    // One tick span of the band, placed in the ladder: span.lo sits at
    // first_index, span.lo + k*span.tick at first_index + k.
    struct Segment {
        TickSpan span;
        std::uint32_t first_index; // ladder index of span.lo
    };

    std::vector<Segment> segments_; // ascending by price, at most 4 entries
    std::uint32_t total_count_ = 0;
    Price band_lo_ = 0;
    Price band_hi_ = 0;
};

} // namespace xmatch::detail
