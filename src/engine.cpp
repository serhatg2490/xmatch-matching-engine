#include "engine/engine.hpp"

#include <algorithm>
#include <limits>
#include <new>
#include <type_traits>
#include <utility>

#include "engine/tick_table.hpp"

namespace xmatch::detail {

// The order-entry path must not be able to throw: these are the building
// blocks it calls. Dropping noexcept from one of them is a build failure,
// not a silent change to the engine's failure mode.
static_assert(noexcept(std::declval<OrderPool&>().try_allocate()));
static_assert(noexcept(std::declval<OrderPool&>()[0u]));
static_assert(noexcept(std::declval<FlatHashMap&>().find(0)));
static_assert(noexcept(std::declval<FlatHashMap&>().insert_no_grow(0, 0)));
static_assert(noexcept(std::declval<FlatHashMap&>().assign_existing(0, 0)));
static_assert(noexcept(std::declval<OrderBook&>().price_to_index(0)));
static_assert(noexcept(std::declval<OrderBook&>().add_resting(std::declval<OrderPool&>(), 0u, Side::kBuy, 0)));
static_assert(noexcept(std::declval<OrderBook&>().remove_resting(std::declval<OrderPool&>(), 0u)));
static_assert(noexcept(std::declval<OrderBook&>().replace_in_place(std::declval<OrderPool&>(), 0u, 0u)));
static_assert(noexcept(std::declval<const OrderBook&>().top_of_book()));
static_assert(noexcept(is_tick_aligned(0)));

Engine::Engine(IEventListener* listener) : listener_(listener) {}

ConfigureResult Engine::configure(const InstrumentConfig* instruments, std::size_t count,
                                  std::uint32_t max_orders) noexcept {
    try {
        // If the engine is already configured, do not configure it again.
        // If the configuration fails, the engine remains unconfigured and can be retried.
        if (configured_) return ConfigureResult::kAlreadyConfigured;

        // Validate the input parameters before proceeding with configuration.
        if (!instruments || count == 0 || count > UINT32_MAX ||
            max_orders == 0 || max_orders > kMaxOrderCapacity) {
            return ConfigureResult::kInvalidArgument;
        }

        std::vector<OrderBook> books(count);
        FlatHashMap instrument_index;
        instrument_index.reserve(count);

        for (std::size_t i = 0; i < count; ++i) {
            const InstrumentConfig& cfg = instruments[i];
            if (cfg.reference_price > std::numeric_limits<Price>::max() / kMaxBandBps ||
                !is_tick_aligned(cfg.reference_price) ||
                cfg.band_bps > kMaxBandBps)
            {
                return ConfigureResult::kInvalidArgument;
            }
            if (instrument_index.contains(cfg.instrument_id)) {
                return ConfigureResult::kInvalidArgument; // duplicate instrument_id
            }
            books[i].configure(cfg.instrument_id, cfg.reference_price, cfg.band_bps);
            instrument_index.insert(cfg.instrument_id, static_cast<std::uint32_t>(i));
        }

        // Everything the order-entry path will ever need, allocated now: the
        // pool holds max_orders slots and the id index can take one entry per
        // slot without reaching its load limit, so neither grows later.
        OrderPool pool(max_orders);
        FlatHashMap order_index;
        order_index.reserve(max_orders);

        static_assert(std::is_nothrow_move_assignable_v<OrderPool>, "OrderPool must be nothrow move assignable");
        static_assert(std::is_nothrow_move_assignable_v<FlatHashMap>, "FlatHashMap must be nothrow move assignable");
        books_ = std::move(books);
        pool_ = std::move(pool);
        instrument_index_ = std::move(instrument_index);
        order_index_ = std::move(order_index);

        configured_ = true;
        return ConfigureResult::kOk;
    }
    catch (const std::bad_alloc&) {
        return ConfigureResult::kOutOfMemory;
    } 
    catch (...) {
        // Never let an exception cross the API boundary.
        // A failure here leaves the engine unconfigured.
        return ConfigureResult::kInternalError;
    }
}

OrderBook* Engine::find_book(InstrumentId id) noexcept {
    std::uint32_t* idx = instrument_index_.find(id);
    return idx ? &books_[*idx] : nullptr;
}

const OrderBook* Engine::find_book(InstrumentId id) const noexcept {
    const std::uint32_t* idx = instrument_index_.find(id);
    return idx ? &books_[*idx] : nullptr;
}

bool Engine::crosses(Side aggressor_side, Price limit_price, Price resting_price) noexcept {
    return aggressor_side == Side::kBuy ? (limit_price >= resting_price)
                                         : (limit_price <= resting_price);
}

void Engine::run_matching(OrderBook& book, InstrumentId instrument_id,
                           std::uint32_t aggressor_slot, Side side,
                           Price limit_price, bool is_market) noexcept {
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

            agg.open_qty -= fill;
            agg.filled_qty += fill;
            resting.open_qty -= fill;
            resting.filled_qty += fill;
            lvl.total_qty -= fill;

            if (resting.open_qty == 0) {
                book.remove_resting(pool_, resting_slot);
            }

            listener_->on_trade(trade_id, instrument_id, resting.price, fill,
                        resting.id, agg.id, side);
        }
    }
}

bool Engine::can_fully_fill(const OrderBook& book, Side side, bool is_market,
                             Price limit_price, Quantity needed) const noexcept {
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

void Engine::submit(const NewOrder& order) noexcept {
    if (!configured_) {
        listener_->on_rejected(order.order_id, RejectReason::kNotConfigured);
        return;
    }

    OrderBook* book = find_book(order.instrument_id);
    if (!book) {
        listener_->on_rejected(order.order_id, RejectReason::kUnknownInstrument);
        return;
    }
    if (order_index_.contains(order.order_id)) {
        listener_->on_rejected(order.order_id, RejectReason::kDuplicateOrderId);
        return;
    }
    // Out of slots: the id cannot be recorded, so it is not consumed.
    if (pool_.full()) {
        listener_->on_rejected(order.order_id, RejectReason::kCapacityExceeded);
        return;
    }

    // The id is consumed here, permanently, regardless of whether
    // downstream validation (quantity/TIF/tick/band) still rejects
    // this particular submission -- matching the header's "order_id
    // unique per engine instance" contract. Neither call allocates:
    // the pool has a free slot (checked above) and the index was
    // reserved for the pool's whole capacity.
    std::uint32_t slot = pool_.try_allocate();
    OrderRecord& rec = pool_[slot];
    rec.id = order.order_id;
    rec.instrument_id = order.instrument_id;
    rec.side = order.side;
    order_index_.insert_no_grow(order.order_id, slot);

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
}

void Engine::cancel(const CancelOrder& request) noexcept {
    if (!configured_) {
        listener_->on_rejected(request.order_id, RejectReason::kNotConfigured);
        return;
    }

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
}

void Engine::replace(const ReplaceOrder& request) noexcept {
    if (!configured_) {
        listener_->on_rejected(request.new_order_id, RejectReason::kNotConfigured);
        return;
    }

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

    // Last check before any state changes: out of slots leaves the old
    // order on the book exactly as it was.
    if (pool_.full()) {
        listener_->on_rejected(request.new_order_id, RejectReason::kCapacityExceeded);
        return;
    }

    std::uint32_t new_slot = pool_.try_allocate();
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
        order_index_.insert_no_grow(request.new_order_id, new_slot);
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
}

TopOfBook Engine::top_of_book(InstrumentId instrument_id) const noexcept {
    const OrderBook* book = find_book(instrument_id);
    if (!book) return TopOfBook{};
    return book->top_of_book();
}

} // namespace xmatch::detail
