#include <gtest/gtest.h>

#include "xmatch/matching_engine_api.hpp"
#include "test_helpers.hpp"

using namespace xmatch;
using namespace xmatch::test;

namespace {

class ValidationTest : public ::testing::Test {
protected:
    RecordingListener listener;
    EngineHandle engine{&listener};

    void SetUp() override {
        InstrumentConfig cfg{.instrument_id = 1, .reference_price = Px(25), .band_bps = 1000};
        engine->configure(&cfg, 1);
    }
};

TEST_F(ValidationTest, UnknownInstrumentRejected) {
    engine->submit(MakeLimit(1, /*inst=*/999, Side::kBuy, TimeInForce::kDay, Px(25), 10));
    ASSERT_EQ(listener.events.size(), 1u);
    EXPECT_EQ(listener.events[0], Rejected(1, RejectReason::kUnknownInstrument));
}

TEST_F(ValidationTest, DuplicateOrderIdRejected) {
    engine->submit(MakeLimit(1, 1, Side::kBuy, TimeInForce::kDay, Px(25), 10));
    engine->submit(MakeLimit(1, 1, Side::kBuy, TimeInForce::kDay, Px(25), 20));
    ASSERT_EQ(listener.events.size(), 2u);
    EXPECT_EQ(listener.events[1], Rejected(1, RejectReason::kDuplicateOrderId));
}

TEST_F(ValidationTest, DuplicateIdStillRejectedAfterFirstAttemptWasItselfRejected) {
    // id=1 is consumed by the uniqueness check even though it goes on to
    // fail a later validation step (out-of-band price).
    engine->submit(MakeLimit(1, 1, Side::kBuy, TimeInForce::kDay, Px(28), 10));
    ASSERT_EQ(listener.events.back(), Rejected(1, RejectReason::kPriceOutOfBand));
    engine->submit(MakeLimit(1, 1, Side::kBuy, TimeInForce::kDay, Px(25), 10));
    EXPECT_EQ(listener.events.back(), Rejected(1, RejectReason::kDuplicateOrderId));
}

TEST_F(ValidationTest, ZeroQuantityRejected) {
    engine->submit(MakeLimit(1, 1, Side::kBuy, TimeInForce::kDay, Px(25), 0));
    ASSERT_EQ(listener.events.size(), 1u);
    EXPECT_EQ(listener.events[0], Rejected(1, RejectReason::kInvalidQuantity));
}

TEST_F(ValidationTest, MarketDayRejected) {
    engine->submit(NewOrder{.order_id = 1, .instrument_id = 1, .side = Side::kBuy, .type = OrderType::kMarket,
                             .tif = TimeInForce::kDay, .price = 0, .quantity = 10});
    ASSERT_EQ(listener.events.size(), 1u);
    EXPECT_EQ(listener.events[0], Rejected(1, RejectReason::kInvalidTimeInForce));
}

TEST_F(ValidationTest, InvalidTickRejected) {
    // 25.11 is not a multiple of the 0.02 tick that applies at [20,50).
    engine->submit(MakeLimit(1, 1, Side::kBuy, TimeInForce::kDay, Px(25, 1100), 10));
    ASSERT_EQ(listener.events.size(), 1u);
    EXPECT_EQ(listener.events[0], Rejected(1, RejectReason::kInvalidPriceTick));
}

TEST_F(ValidationTest, PriceOutOfBandRejected) {
    // band is [22.50, 27.50]; 27.52 is tick-aligned (0.02) but outside.
    engine->submit(MakeLimit(1, 1, Side::kBuy, TimeInForce::kDay, Px(27, 5200), 10));
    ASSERT_EQ(listener.events.size(), 1u);
    EXPECT_EQ(listener.events[0], Rejected(1, RejectReason::kPriceOutOfBand));
}

TEST_F(ValidationTest, BandLimitsAreInclusive) {
    engine->submit(MakeLimit(1, 1, Side::kBuy, TimeInForce::kDay, Px(22, 5000), 10)); // lower edge
    engine->submit(MakeLimit(2, 1, Side::kSell, TimeInForce::kDay, Px(27, 5000), 10)); // upper edge
    ASSERT_EQ(listener.events.size(), 2u);
    EXPECT_EQ(listener.events[0], Accepted(1));
    EXPECT_EQ(listener.events[1], Accepted(2));
}

TEST_F(ValidationTest, TickCheckedBeforeBand) {
    // 27.51 is both mis-aligned (0.02 tick) AND out of band; tick must win.
    engine->submit(MakeLimit(1, 1, Side::kBuy, TimeInForce::kDay, Px(27, 5100), 10));
    ASSERT_EQ(listener.events.size(), 1u);
    EXPECT_EQ(listener.events[0], Rejected(1, RejectReason::kInvalidPriceTick));
}

TEST_F(ValidationTest, MarketOrderSkipsTickAndBandChecks) {
    // No liquidity present, so it just cancels for lack of a counterparty --
    // never for tick/band, since market orders ignore price entirely.
    engine->submit(MakeMarket(1, 1, Side::kBuy, TimeInForce::kIoc, 10));
    ASSERT_EQ(listener.events.size(), 2u);
    EXPECT_EQ(listener.events[0], Accepted(1));
    EXPECT_EQ(listener.events[1], Canceled(1, 10, CancelReason::kNoLiquidity));
}

TEST_F(ValidationTest, ValidationOrderInstrumentBeforeDuplicateId) {
    // Same order_id, but the first attempt targets an unknown instrument --
    // it must NOT consume the id (instrument is checked first).
    engine->submit(MakeLimit(1, 999, Side::kBuy, TimeInForce::kDay, Px(25), 10));
    engine->submit(MakeLimit(1, 1, Side::kBuy, TimeInForce::kDay, Px(25), 10));
    ASSERT_EQ(listener.events.size(), 2u);
    EXPECT_EQ(listener.events[0], Rejected(1, RejectReason::kUnknownInstrument));
    EXPECT_EQ(listener.events[1], Accepted(1));
}

} // namespace
