#pragma once

#include <vector>

#include "xmatch/matching_engine_api.hpp"
#include "engine/flat_hash_map.hpp"
#include "engine/order_book.hpp"
#include "engine/order_pool.hpp"

namespace xmatch::detail {

// IMatchingEngine implementation. See DESIGN.md for the data-structure
// rationale (array/bitmap order book bounded by the price band, chunked
// order pool, open-addressing id index -- all sized for max_orders in
// configure() so the hot path never allocates).
class Engine final : public IMatchingEngine {
public:
    explicit Engine(IEventListener* listener);

    // configure() is the only method that allocates; it catches everything
    // internally and reports it as a ConfigureResult. The order-entry methods
    // below are noexcept by construction: nothing they call can throw, so an
    // exception there would be a bug and terminates rather than leaving a
    // half-applied book behind.
    ConfigureResult configure(const InstrumentConfig* instruments, std::size_t count,
                              std::uint32_t max_orders) noexcept override;
    void submit(const NewOrder& order) noexcept override;
    void cancel(const CancelOrder& request) noexcept override;
    void replace(const ReplaceOrder& request) noexcept override;
    TopOfBook top_of_book(InstrumentId instrument_id) const noexcept override;

private:
    OrderBook* find_book(InstrumentId id) noexcept;
    const OrderBook* find_book(InstrumentId id) const noexcept;

    static bool crosses(Side aggressor_side, Price limit_price, Price resting_price) noexcept;

    // Matches pool_[aggressor_slot] against `book` in price-time priority,
    // emitting on_trade events and mutating both sides' state as it goes.
    // Leaves the remaining (unfilled) quantity in pool_[aggressor_slot].open_qty.
    void run_matching(OrderBook& book, InstrumentId instrument_id,
                       std::uint32_t aggressor_slot, Side side,
                       Price limit_price, bool is_market) noexcept;

    // Non-mutating FOK feasibility check: can `needed` lots fully cross
    // the book at/through `limit_price` (or unconditionally, for market)?
    bool can_fully_fill(const OrderBook& book, Side side, bool is_market,
                         Price limit_price, Quantity needed) const noexcept;

    IEventListener* listener_;
    FlatHashMap order_index_;      // order_id -> pool slot
    FlatHashMap instrument_index_; // instrument_id -> books_ index
    OrderPool pool_;
    std::vector<OrderBook> books_;
    TradeId next_trade_id_ = 1;
    bool configured_ = false; // configure() is one-shot; see its definition
};

} // namespace xmatch::detail
