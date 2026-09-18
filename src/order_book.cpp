#include "engine/order_book.hpp"

#include "engine/tick_table.hpp"

namespace xmatch::detail {

void OrderBook::configure(InstrumentId instrument_id, Price reference_price, std::uint32_t band_bps) {
    instrument_id_ = instrument_id;
    reference_price_ = reference_price;

    Price delta = (reference_price * static_cast<Price>(band_bps)) / 10000;
    Price raw_lo = reference_price - delta;
    Price raw_hi = reference_price + delta;
    if (raw_lo < 1) raw_lo = 1; // safety floor; band is normally well above zero

    Price lo = ceil_to_tick(raw_lo);
    Price hi = floor_to_tick(raw_hi);
    ladder_.build(lo, hi);

    std::uint32_t n = ladder_.size();
    bid_levels_.assign(n, Level{});
    ask_levels_.assign(n, Level{});
    bid_bitmap_.resize(n);
    ask_bitmap_.resize(n);
    best_bid_idx_ = -1;
    best_ask_idx_ = -1;
}

TopOfBook OrderBook::top_of_book() const {
    TopOfBook tob{};
    if (best_bid_idx_ >= 0) {
        const Level& lvl = bid_levels_[static_cast<std::uint32_t>(best_bid_idx_)];
        tob.bid_price = ladder_.index_to_price(static_cast<std::uint32_t>(best_bid_idx_));
        tob.bid_quantity = lvl.total_qty;
    }
    if (best_ask_idx_ >= 0) {
        const Level& lvl = ask_levels_[static_cast<std::uint32_t>(best_ask_idx_)];
        tob.ask_price = ladder_.index_to_price(static_cast<std::uint32_t>(best_ask_idx_));
        tob.ask_quantity = lvl.total_qty;
    }
    return tob;
}

void OrderBook::add_resting(OrderPool& pool, std::uint32_t slot, Side side, Price price) {
    std::int64_t signed_idx = ladder_.price_to_index(price);
    std::uint32_t idx = static_cast<std::uint32_t>(signed_idx);

    OrderRecord& rec = pool[slot];
    rec.side = side;
    rec.price = price;
    rec.level_index = idx;
    rec.is_open = true;

    Level& lvl = level_at(side, idx);
    rec.prev = lvl.tail;
    rec.next = kInvalidSlot;
    if (lvl.tail != kInvalidSlot) {
        pool[lvl.tail].next = slot;
    } else {
        lvl.head = slot;
    }
    lvl.tail = slot;
    lvl.total_qty += rec.open_qty;
    ++lvl.order_count;

    LevelBitset& bitmap = (side == Side::kBuy) ? bid_bitmap_ : ask_bitmap_;
    if (lvl.order_count == 1) bitmap.set(idx);

    if (side == Side::kBuy) {
        if (best_bid_idx_ < 0 || static_cast<std::int64_t>(idx) > best_bid_idx_) best_bid_idx_ = idx;
    } else {
        if (best_ask_idx_ < 0 || static_cast<std::int64_t>(idx) < best_ask_idx_) best_ask_idx_ = idx;
    }
}

void OrderBook::remove_resting(OrderPool& pool, std::uint32_t slot) {
    OrderRecord& rec = pool[slot];
    Side side = rec.side;
    std::uint32_t idx = rec.level_index;

    Level& lvl = level_at(side, idx);
    if (rec.prev != kInvalidSlot) pool[rec.prev].next = rec.next; else lvl.head = rec.next;
    if (rec.next != kInvalidSlot) pool[rec.next].prev = rec.prev; else lvl.tail = rec.prev;
    lvl.total_qty -= rec.open_qty; // safe no-op if already fully filled (open_qty == 0)
    --lvl.order_count;

    rec.is_open = false;
    rec.prev = kInvalidSlot;
    rec.next = kInvalidSlot;

    if (lvl.order_count == 0) {
        LevelBitset& bitmap = (side == Side::kBuy) ? bid_bitmap_ : ask_bitmap_;
        bitmap.clear(idx);
        if (side == Side::kBuy && best_bid_idx_ == static_cast<std::int64_t>(idx)) {
            best_bid_idx_ = bitmap.find_highest_le(static_cast<std::int64_t>(idx) - 1);
        } else if (side == Side::kSell && best_ask_idx_ == static_cast<std::int64_t>(idx)) {
            best_ask_idx_ = bitmap.find_lowest_ge(static_cast<std::int64_t>(idx) + 1);
        }
    }
}

void OrderBook::replace_in_place(OrderPool& pool, std::uint32_t old_slot, std::uint32_t new_slot) {
    OrderRecord& old_rec = pool[old_slot];
    OrderRecord& new_rec = pool[new_slot];
    Side side = old_rec.side;
    std::uint32_t idx = old_rec.level_index;
    Level& lvl = level_at(side, idx);

    new_rec.side = side;
    new_rec.price = old_rec.price;
    new_rec.level_index = idx;
    new_rec.is_open = true;
    new_rec.prev = old_rec.prev;
    new_rec.next = old_rec.next;

    if (old_rec.prev != kInvalidSlot) pool[old_rec.prev].next = new_slot; else lvl.head = new_slot;
    if (old_rec.next != kInvalidSlot) pool[old_rec.next].prev = new_slot; else lvl.tail = new_slot;

    lvl.total_qty = lvl.total_qty - old_rec.open_qty + new_rec.open_qty;
    // order_count, bitmap, and best index are unchanged: the level held
    // exactly one occupant before and holds exactly one (the new slot) now.

    old_rec.is_open = false;
    old_rec.prev = kInvalidSlot;
    old_rec.next = kInvalidSlot;
}

} // namespace xmatch::detail
