#pragma once

#include <cstdint>
#include <vector>

#include "xmatch/matching_engine_api.hpp"
#include "engine/level_bitset.hpp"
#include "engine/order_pool.hpp"
#include "engine/price_ladder.hpp"

namespace xmatch::detail {

// A single price level's FIFO queue of resting orders (intrusive linked
// list stored in the shared OrderPool) plus the aggregate quantity the
// matching engine needs for O(1) top_of_book.
struct Level {
    std::uint32_t head = kInvalidSlot; // oldest (highest priority)
    std::uint32_t tail = kInvalidSlot; // newest
    Quantity total_qty = 0;
    std::uint32_t order_count = 0;
};

// Per-instrument order book. Price levels live in flat arrays indexed via
// PriceLadder (bounded by the instrument's daily price band), with a
// LevelBitset per side for O(1)-amortized relocation of the best price
// when it empties out. See DESIGN.md for the full rationale.
class OrderBook {
public:
    void configure(InstrumentId instrument_id, Price reference_price, std::uint32_t band_bps);

    InstrumentId instrument_id() const { return instrument_id_; }
    Price band_lo() const { return ladder_.band_lo(); }
    Price band_hi() const { return ladder_.band_hi(); }

    // -1 if price is not a valid resting price for this instrument (outside
    // the band, or not tick-aligned).
    std::int64_t price_to_index(Price price) const { return ladder_.price_to_index(price); }
    Price index_to_price(std::uint32_t index) const { return ladder_.index_to_price(index); }

    TopOfBook top_of_book() const;

    bool has_best(Side side) const {
        return (side == Side::kBuy) ? best_bid_idx_ >= 0 : best_ask_idx_ >= 0;
    }
    std::int64_t best_index(Side side) const {
        return (side == Side::kBuy) ? best_bid_idx_ : best_ask_idx_;
    }
    Price best_price(Side side) const {
        return ladder_.index_to_price(static_cast<std::uint32_t>(best_index(side)));
    }

    // Continues a priority-ordered walk of occupied levels for `side`,
    // strictly past `after` (exclusive), or -1 if none remain. Used by the
    // FOK feasibility pre-check, which needs to sum liquidity across
    // several levels without mutating book state.
    std::int64_t next_index(Side side, std::int64_t after) const {
        return (side == Side::kBuy) ? bid_bitmap_.find_highest_le(after - 1)
                                     : ask_bitmap_.find_lowest_ge(after + 1);
    }

    Level& level_at(Side side, std::uint32_t idx) {
        return (side == Side::kBuy) ? bid_levels_[idx] : ask_levels_[idx];
    }
    const Level& level_at(Side side, std::uint32_t idx) const {
        return (side == Side::kBuy) ? bid_levels_[idx] : ask_levels_[idx];
    }

    // Appends `slot` to the tail of the level for (side, price). Caller must
    // have already set pool[slot].open_qty to the resting quantity.
    void add_resting(OrderPool& pool, std::uint32_t slot, Side side, Price price);

    // Unlinks `slot` from its current level (pool[slot].side/level_index
    // must still be valid). Safe to call with open_qty already at 0 (e.g.
    // right after a full fill) or with the live remaining quantity (cancel,
    // replace-away).
    void remove_resting(OrderPool& pool, std::uint32_t slot);

    // Swaps `new_slot` into the exact FIFO position currently held by
    // `old_slot` on the same side/level -- used by replace() when time
    // priority is retained (same price, quantity unchanged or decreased).
    // `old_slot` must currently be resting; `new_slot` must not be, and
    // must already carry the side/price/open_qty it will rest with. The
    // level's aggregate quantity is adjusted by the qty delta; order_count,
    // the bitmap, and the best-index cache are untouched (the level stays
    // non-empty throughout).
    void replace_in_place(OrderPool& pool, std::uint32_t old_slot, std::uint32_t new_slot);

private:
    InstrumentId instrument_id_ = 0;
    Price reference_price_ = 0;
    PriceLadder ladder_;

    std::vector<Level> bid_levels_;
    std::vector<Level> ask_levels_;
    LevelBitset bid_bitmap_;
    LevelBitset ask_bitmap_;
    std::int64_t best_bid_idx_ = -1; // highest occupied index, -1 = empty
    std::int64_t best_ask_idx_ = -1; // lowest occupied index, -1 = empty
};

} // namespace xmatch::detail
