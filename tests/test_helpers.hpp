#pragma once

#include <gtest/gtest.h>

#include <cstdint>
#include <memory>
#include <vector>

#include "xmatch/matching_engine_api.hpp"

namespace xmatch::test {

// Fixed-point helper: TRY with up to 4 decimals -> Price (1/10000 TRY).
constexpr Price try_price(std::int64_t whole_x10000) { return whole_x10000; }
// e.g. Px(25, 1000) == 25.1000 TRY == 251000. Kept explicit/verbose on
// purpose in the golden-scenario test so each price literal stays readable
// at a glance.
constexpr Price Px(std::int64_t major, std::int64_t minor4 = 0) {
    return major * 10000 + minor4;
}

enum class EventKind { kAccepted, kRejected, kTrade, kCanceled, kReplaced };

struct RecordedEvent {
    EventKind kind;
    OrderId order_id = 0;         // accepted / rejected / canceled
    RejectReason reject_reason{};
    TradeId trade_id = 0;         // trade
    InstrumentId instrument_id = 0;
    Price price = 0;
    Quantity quantity = 0;
    OrderId resting_order_id = 0;
    OrderId aggressive_order_id = 0;
    Side aggressor_side{};
    CancelReason cancel_reason{};
    OrderId new_order_id = 0;     // replaced
    Quantity new_open_quantity = 0;

    bool operator==(const RecordedEvent& o) const {
        if (kind != o.kind) return false;
        switch (kind) {
            case EventKind::kAccepted:
                return order_id == o.order_id;
            case EventKind::kRejected:
                return order_id == o.order_id && reject_reason == o.reject_reason;
            case EventKind::kTrade:
                return price == o.price && quantity == o.quantity &&
                       resting_order_id == o.resting_order_id &&
                       aggressive_order_id == o.aggressive_order_id &&
                       aggressor_side == o.aggressor_side &&
                       instrument_id == o.instrument_id;
            case EventKind::kCanceled:
                return order_id == o.order_id && quantity == o.quantity &&
                       cancel_reason == o.cancel_reason;
            case EventKind::kReplaced:
                return order_id == o.order_id && new_order_id == o.new_order_id &&
                       price == o.price && new_open_quantity == o.new_open_quantity;
        }
        return false;
    }
};

inline std::ostream& operator<<(std::ostream& os, const RecordedEvent& e) {
    switch (e.kind) {
        case EventKind::kAccepted: return os << "accepted(" << e.order_id << ")";
        case EventKind::kRejected:
            return os << "rejected(" << e.order_id << ", " << static_cast<int>(e.reject_reason) << ")";
        case EventKind::kTrade:
            return os << "trade(px=" << e.price << ", qty=" << e.quantity
                       << ", resting=" << e.resting_order_id << ", aggr=" << e.aggressive_order_id << ")";
        case EventKind::kCanceled:
            return os << "canceled(" << e.order_id << ", " << e.quantity << ", "
                       << static_cast<int>(e.cancel_reason) << ")";
        case EventKind::kReplaced:
            return os << "replaced(" << e.order_id << "->" << e.new_order_id << ", " << e.price
                       << ", " << e.new_open_quantity << ")";
    }
    return os;
}

class RecordingListener final : public IEventListener {
public:
    std::vector<RecordedEvent> events;
    TradeId next_expected_trade_id = 1;

    void on_accepted(OrderId order_id) override {
        events.push_back(RecordedEvent{.kind = EventKind::kAccepted, .order_id = order_id});
    }
    void on_rejected(OrderId order_id, RejectReason reason) override {
        events.push_back(RecordedEvent{.kind = EventKind::kRejected, .order_id = order_id, .reject_reason = reason});
    }
    void on_trade(TradeId trade_id, InstrumentId instrument_id, Price price, Quantity quantity,
                  OrderId resting_order_id, OrderId aggressive_order_id, Side aggressor_side) override {
        EXPECT_EQ(trade_id, next_expected_trade_id) << "trade ids must start at 1 and increment by 1";
        ++next_expected_trade_id;
        events.push_back(RecordedEvent{.kind = EventKind::kTrade,
                                        .trade_id = trade_id,
                                        .instrument_id = instrument_id,
                                        .price = price,
                                        .quantity = quantity,
                                        .resting_order_id = resting_order_id,
                                        .aggressive_order_id = aggressive_order_id,
                                        .aggressor_side = aggressor_side});
    }
    void on_canceled(OrderId order_id, Quantity canceled_quantity, CancelReason reason) override {
        events.push_back(RecordedEvent{.kind = EventKind::kCanceled,
                                        .order_id = order_id,
                                        .quantity = canceled_quantity,
                                        .cancel_reason = reason});
    }
    void on_replaced(OrderId old_order_id, OrderId new_order_id, Price new_price,
                      Quantity new_open_quantity) override {
        events.push_back(RecordedEvent{.kind = EventKind::kReplaced,
                                        .order_id = old_order_id,
                                        .price = new_price,
                                        .new_order_id = new_order_id,
                                        .new_open_quantity = new_open_quantity});
    }
};

inline RecordedEvent Accepted(OrderId id) {
    return RecordedEvent{.kind = EventKind::kAccepted, .order_id = id};
}
inline RecordedEvent Rejected(OrderId id, RejectReason r) {
    return RecordedEvent{.kind = EventKind::kRejected, .order_id = id, .reject_reason = r};
}
inline RecordedEvent Trade(InstrumentId inst, Price px, Quantity qty, OrderId resting, OrderId aggr, Side side) {
    return RecordedEvent{.kind = EventKind::kTrade, .instrument_id = inst, .price = px, .quantity = qty,
                          .resting_order_id = resting, .aggressive_order_id = aggr, .aggressor_side = side};
}
inline RecordedEvent Canceled(OrderId id, Quantity qty, CancelReason r) {
    return RecordedEvent{.kind = EventKind::kCanceled, .order_id = id, .quantity = qty, .cancel_reason = r};
}
inline RecordedEvent Replaced(OrderId old_id, OrderId new_id, Price px, Quantity open_qty) {
    return RecordedEvent{.kind = EventKind::kReplaced, .order_id = old_id,
                          .price = px, .new_order_id = new_id, .new_open_quantity = open_qty};
}

inline NewOrder MakeLimit(OrderId id, InstrumentId inst, Side side, TimeInForce tif, Price price, Quantity qty) {
    return NewOrder{.order_id = id, .instrument_id = inst, .side = side, .type = OrderType::kLimit,
                     .tif = tif, .price = price, .quantity = qty};
}
inline NewOrder MakeMarket(OrderId id, InstrumentId inst, Side side, TimeInForce tif, Quantity qty) {
    return NewOrder{.order_id = id, .instrument_id = inst, .side = side, .type = OrderType::kMarket,
                     .tif = tif, .price = 0, .quantity = qty};
}

// RAII wrapper around xmatch_create/destroy.
class EngineHandle {
public:
    explicit EngineHandle(IEventListener* listener) : engine_(xmatch_create(listener)) {}
    ~EngineHandle() { xmatch_destroy(engine_); }
    EngineHandle(const EngineHandle&) = delete;
    EngineHandle& operator=(const EngineHandle&) = delete;

    IMatchingEngine* operator->() { return engine_; }
    IMatchingEngine& operator*() { return *engine_; }

private:
    IMatchingEngine* engine_;
};

} // namespace xmatch::test
