#pragma once

#include <cstdint>
#include <optional>
#include <vector>

#include "xmatch/matching_engine_api.hpp"

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
class PriceLadder {
public:
    void build(Price band_lo, Price band_hi);

    // -1 if price is outside the band or not tick-aligned within it.
    std::int64_t price_to_index(Price price) const;

    Price index_to_price(std::uint32_t index) const;

    std::uint32_t size() const { return total_count_; }

    Price band_lo() const { return band_lo_; }
    Price band_hi() const { return band_hi_; }

private:
    struct Segment {
        Price start_price; // inclusive, tick-aligned
        Price end_price;   // inclusive, tick-aligned
        Price tick;
        std::uint32_t index_offset;
        std::uint32_t count;
    };

    std::vector<Segment> segments_; // ascending by price, at most 4 entries
    std::uint32_t total_count_ = 0;
    Price band_lo_ = 0;
    Price band_hi_ = 0;
};

} // namespace xmatch::detail
