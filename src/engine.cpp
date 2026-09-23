#include "engine/engine.hpp"

#include <algorithm>

#include "engine/tick_table.hpp"

namespace xmatch::detail {

namespace {
// Pre-reserved capacity, sized so that a run on the order of 10M operations
// never grows the order pool or the id index -- and therefore never
// allocates -- on the timed hot path. See DESIGN.md for the memory/latency
// trade-off this represents.
constexpr std::size_t kDefaultOrderCapacity = 1u << 24; // ~16.7M slots
} // namespace

Engine::Engine(IEventListener* listener) : listener_(listener) {}

void Engine::configure(const InstrumentConfig* instruments, std::size_t count) {
    try {
        // One-shot. A second call would reset books that still hold open
        // orders and re-insert instrument ids into the index, corrupting
        // both, so it is ignored. The flag is set before any work so a
        // first call that fails part-way is not retried on top of its
        // partial state either.
        if (configured_) return;
        configured_ = true;

        books_.resize(count);
        instrument_index_.reserve(count);
        for (std::size_t i = 0; i < count; ++i) {
            const InstrumentConfig& cfg = instruments[i];
            books_[i].configure(cfg.instrument_id, cfg.reference_price, cfg.band_bps);
            instrument_index_.insert(cfg.instrument_id, static_cast<std::uint32_t>(i));
        }
        pool_.reserve(kDefaultOrderCapacity);
        order_index_.reserve(kDefaultOrderCapacity);
    } catch (...) {
        // Never let an exception cross the API boundary. A failure here
        // leaves the engine unusable, but that's preferable to crashing
        // the host process.
    }
}

OrderBook* Engine::find_book(InstrumentId id) {
    std::uint32_t* idx = instrument_index_.find(id);
    return idx ? &books_[*idx] : nullptr;
}

const OrderBook* Engine::find_book(InstrumentId id) const {
    const std::uint32_t* idx = instrument_index_.find(id);
    return idx ? &books_[*idx] : nullptr;
}

bool Engine::crosses(Side aggressor_side, Price limit_price, Price resting_price) {
    return aggressor_side == Side::kBuy ? (limit_price >= resting_price)
                                         : (limit_price <= resting_price);
}

void Engine::run_matching(OrderBook& book, InstrumentId instrument_id,
                           std::uint32_t aggressor_slot, Side side,
                           Price limit_price, bool is_market) {
    OrderRecord& agg = pool_[aggressor_slot];
    Side opp_side = (side == Side::kBuy) ? Side::kSell : Side::kBuy;

    while (agg.open_qty > 0 && book.has_best(opp_side)) {
        std::int64_t idx = book.best_index(opp_side);
        Price resting_price = book.index_to_price(static_cast<std::uint32_t>(idx));
        if (!is_market && !crosses(side, limit_price, resting_price)) break;

        Level& lvl = book.level_at(opp_side, static_cast<std::uint32_t>(idx));
        while (agg.open_qty > 0 && lvl.head != kInvalidSlot) {
            std::uint32_t resting_slot = lvl.head;
            OrderRecord& resting = pool_[resting_slot];

            Quantity fill = std::min(agg.open_qty, resting.open_qty);
            TradeId trade_id = next_trade_id_++;
            listener_->on_trade(trade_id, instrument_id, resting.price, fill,
                                 resting.id, agg.id, side);

            agg.open_qty -= fill;
            agg.filled_qty += fill;
            resting.open_qty -= fill;
            resting.filled_qty += fill;
            lvl.total_qty -= fill;

            if (resting.open_qty == 0) {
                book.remove_resting(pool_, resting_slot);
            }
        }
    }
}

bool Engine::can_fully_fill(const OrderBook& book, Side side, bool is_market,
                             Price limit_price, Quantity needed) const {
    Side opp_side = (side == Side::kBuy) ? Side::kSell : Side::kBuy;
    if (!book.has_best(opp_side)) return false;

    Quantity available = 0;
    std::int64_t idx = book.best_index(opp_side);
    while (idx >= 0) {
        Price resting_price = book.index_to_price(static_cast<std::uint32_t>(idx));
        if (!is_market && !crosses(side, limit_price, resting_price)) break;
        available += book.level_at(opp_side, static_cast<std::uint32_t>(idx)).total_qty;
        if (available >= needed) return true;
        idx = book.next_index(opp_side, idx);
    }
    return available >= needed;
}

void Engine::submit(const NewOrder& order) {
    try {
        OrderBook* book = find_book(order.instrument_id);
        if (!book) {
            listener_->on_rejected(order.order_id, RejectReason::kUnknownInstrument);
            return;
        }
        if (order_index_.contains(order.order_id)) {
            listener_->on_rejected(order.order_id, RejectReason::kDuplicateOrderId);
            return;
        }

        // The id is consumed here, permanently, regardless of whether
        // downstream validation (quantity/TIF/tick/band) still rejects
        // this particular submission -- matching the header's "order_id
        // unique per engine instance" contract.
        std::uint32_t slot = pool_.allocate();
        OrderRecord& rec = pool_[slot];
        rec.id = order.order_id;
        rec.instrument_id = order.instrument_id;
        rec.side = order.side;
        order_index_.insert(order.order_id, slot);

        if (order.quantity == 0) {
            listener_->on_rejected(order.order_id, RejectReason::kInvalidQuantity);
            return;
        }

        bool is_market = (order.type == OrderType::kMarket);
        if (is_market && order.tif == TimeInForce::kDay) {
            listener_->on_rejected(order.order_id, RejectReason::kInvalidTimeInForce);
            return;
        }

        Price price = order.price;
        if (!is_market) {
            if (!is_tick_aligned(price)) {
                listener_->on_rejected(order.order_id, RejectReason::kInvalidPriceTick);
                return;
            }
            if (book->price_to_index(price) < 0) {
                listener_->on_rejected(order.order_id, RejectReason::kPriceOutOfBand);
                return;
            }
        } else {
            price = 0; // ignored for market orders
        }

        rec.open_qty = order.quantity;
        rec.filled_qty = 0;
        listener_->on_accepted(order.order_id);

        if (order.tif == TimeInForce::kFok) {
            if (!can_fully_fill(*book, order.side, is_market, price, order.quantity)) {
                rec.open_qty = 0;
                listener_->on_canceled(order.order_id, order.quantity, CancelReason::kFokUnfillable);
                return;
            }
        }

        run_matching(*book, order.instrument_id, slot, order.side, price, is_market);

        if (rec.open_qty > 0) {
            if (order.tif == TimeInForce::kDay) {
                book->add_resting(pool_, slot, order.side, price);
            } else {
                CancelReason reason = is_market ? CancelReason::kNoLiquidity
                                                 : (order.tif == TimeInForce::kIoc
                                                        ? CancelReason::kIocRemainder
                                                        : CancelReason::kFokUnfillable);
                Quantity remainder = rec.open_qty;
                rec.open_qty = 0;
                listener_->on_canceled(order.order_id, remainder, reason);
            }
        }
    } catch (...) {
        // swallow: never let an exception cross the API boundary.
    }
}

void Engine::cancel(const CancelOrder& request) {
    try {
        std::uint32_t* slot_ptr = order_index_.find(request.order_id);
        if (!slot_ptr || !pool_[*slot_ptr].is_open) {
            listener_->on_rejected(request.order_id, RejectReason::kUnknownOrder);
            return;
        }
        std::uint32_t slot = *slot_ptr;
        OrderRecord& rec = pool_[slot];
        OrderBook* book = find_book(rec.instrument_id);
        Quantity open_qty = rec.open_qty;
        book->remove_resting(pool_, slot);
        listener_->on_canceled(request.order_id, open_qty, CancelReason::kUserRequested);
    } catch (...) {
    }
}

void Engine::replace(const ReplaceOrder& request) {
    try {
        std::uint32_t* old_slot_ptr = order_index_.find(request.order_id);
        if (!old_slot_ptr || !pool_[*old_slot_ptr].is_open) {
            listener_->on_rejected(request.new_order_id, RejectReason::kUnknownOrder);
            return;
        }
        std::uint32_t old_slot = *old_slot_ptr;
        OrderRecord old_copy = pool_[old_slot]; // snapshot: old_slot gets mutated below
        OrderBook* book = find_book(old_copy.instrument_id);

        Quantity new_open = (request.new_quantity > old_copy.filled_qty)
                                 ? (request.new_quantity - old_copy.filled_qty)
                                 : 0;

        if (new_open == 0) {
            // "the order is simply canceled": behaves exactly like
            // cancel(order_id). new_order_id is never assigned to
            // anything, so it stays available.
            Quantity open_qty = old_copy.open_qty;
            book->remove_resting(pool_, old_slot);
            listener_->on_canceled(request.order_id, open_qty, CancelReason::kUserRequested);
            return;
        }

        bool same_id = (request.new_order_id == request.order_id);
        if (!same_id && order_index_.contains(request.new_order_id)) {
            listener_->on_rejected(request.new_order_id, RejectReason::kDuplicateOrderId);
            return;
        }

        // Re-validate before touching any state: on failure the old order
        // must stay on the book exactly as it was, still reachable under
        // order_id. Only once tick/band validation passes do we allocate
        // the new slot and (for same_id) repoint order_id's map entry --
        // otherwise a same_id replace that fails validation would leave
        // the still-open old order unreachable.
        if (!is_tick_aligned(request.new_price)) {
            listener_->on_rejected(request.new_order_id, RejectReason::kInvalidPriceTick);
            return;
        }
        if (book->price_to_index(request.new_price) < 0) {
            listener_->on_rejected(request.new_order_id, RejectReason::kPriceOutOfBand);
            return;
        }

        std::uint32_t new_slot = pool_.allocate();
        OrderRecord& nrec = pool_[new_slot];
        nrec.id = request.new_order_id;
        nrec.instrument_id = old_copy.instrument_id;
        nrec.side = old_copy.side;
        nrec.price = request.new_price;
        nrec.open_qty = new_open;
        nrec.filled_qty = old_copy.filled_qty;

        if (same_id) {
            order_index_.assign_existing(request.new_order_id, new_slot);
        } else {
            order_index_.insert(request.new_order_id, new_slot);
        }

        bool price_changed = (request.new_price != old_copy.price);
        bool qty_increased = new_open > old_copy.open_qty;
        bool loses_priority = price_changed || qty_increased;

        if (loses_priority) {
            book->remove_resting(pool_, old_slot);
            listener_->on_replaced(request.order_id, request.new_order_id, request.new_price, new_open);
            if (price_changed) {
                // A quantity-only change can never newly cross the book:
                // the resting-order invariant (best_bid < best_ask) was
                // already intact at the old price, so only a price change
                // can make the order marketable.
                run_matching(*book, old_copy.instrument_id, new_slot, old_copy.side,
                             request.new_price, /*is_market=*/false);
            }
            if (pool_[new_slot].open_qty > 0) {
                book->add_resting(pool_, new_slot, old_copy.side, request.new_price);
            }
        } else {
            book->replace_in_place(pool_, old_slot, new_slot);
            listener_->on_replaced(request.order_id, request.new_order_id, request.new_price, new_open);
        }
    } catch (...) {
    }
}

TopOfBook Engine::top_of_book(InstrumentId instrument_id) const {
    try {
        const OrderBook* book = find_book(instrument_id);
        if (!book) return TopOfBook{};
        return book->top_of_book();
    } catch (...) {
        return TopOfBook{};
    }
}

} // namespace xmatch::detail
