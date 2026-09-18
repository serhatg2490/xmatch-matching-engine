#include <gtest/gtest.h>

#include "xmatch/matching_engine_api.hpp"
#include "test_helpers.hpp"

using namespace xmatch;
using namespace xmatch::test;

namespace {

class MatchingTest : public ::testing::Test {
protected:
    RecordingListener listener;
    EngineHandle engine{&listener};

    void SetUp() override {
        InstrumentConfig cfg{.instrument_id = 1, .reference_price = Px(25), .band_bps = 1000};
        engine->configure(&cfg, 1);
    }
};

TEST_F(MatchingTest, PriceThenTimePriorityAcrossLevels) {
    // Two price levels on the bid; a marketable sell should take the
    // better price (25.12) before the worse one (25.10), and within a
    // level, earlier orders match first.
    engine->submit(MakeLimit(1, 1, Side::kBuy, TimeInForce::kDay, Px(25, 1000), 10)); // worse price, first
    engine->submit(MakeLimit(2, 1, Side::kBuy, TimeInForce::kDay, Px(25, 1200), 10)); // better price
    engine->submit(MakeLimit(3, 1, Side::kBuy, TimeInForce::kDay, Px(25, 1200), 5));  // same as #2, later
    listener.events.clear();

    engine->submit(MakeLimit(4, 1, Side::kSell, TimeInForce::kIoc, Px(25, 1000), 25));

    std::vector<RecordedEvent> expected = {
        Accepted(4),
        Trade(1, Px(25, 1200), 10, /*resting=*/2, /*aggr=*/4, Side::kSell),
        Trade(1, Px(25, 1200), 5, /*resting=*/3, /*aggr=*/4, Side::kSell),
        Trade(1, Px(25, 1000), 10, /*resting=*/1, /*aggr=*/4, Side::kSell),
    };
    ASSERT_EQ(listener.events.size(), expected.size());
    for (std::size_t i = 0; i < expected.size(); ++i) EXPECT_EQ(listener.events[i], expected[i]) << i;
}

TEST_F(MatchingTest, LimitDayRestsWhenNotMarketable) {
    engine->submit(MakeLimit(1, 1, Side::kBuy, TimeInForce::kDay, Px(25, 1000), 10));
    ASSERT_EQ(listener.events.size(), 1u);
    EXPECT_EQ(listener.events[0], Accepted(1));
    TopOfBook tob = engine->top_of_book(1);
    EXPECT_EQ(tob.bid_price, Px(25, 1000));
    EXPECT_EQ(tob.bid_quantity, 10u);
}

TEST_F(MatchingTest, LimitIocFullyFilledEmitsNoCancel) {
    engine->submit(MakeLimit(1, 1, Side::kSell, TimeInForce::kDay, Px(25, 1000), 10));
    listener.events.clear();
    engine->submit(MakeLimit(2, 1, Side::kBuy, TimeInForce::kIoc, Px(25, 1000), 10));
    std::vector<RecordedEvent> expected = {
        Accepted(2),
        Trade(1, Px(25, 1000), 10, 1, 2, Side::kBuy),
    };
    ASSERT_EQ(listener.events.size(), expected.size());
    EXPECT_EQ(listener.events[0], expected[0]);
    EXPECT_EQ(listener.events[1], expected[1]);
}

TEST_F(MatchingTest, LimitIocPartialRemainderCanceled) {
    engine->submit(MakeLimit(1, 1, Side::kSell, TimeInForce::kDay, Px(25, 1000), 5));
    listener.events.clear();
    engine->submit(MakeLimit(2, 1, Side::kBuy, TimeInForce::kIoc, Px(25, 1000), 10));
    std::vector<RecordedEvent> expected = {
        Accepted(2),
        Trade(1, Px(25, 1000), 5, 1, 2, Side::kBuy),
        Canceled(2, 5, CancelReason::kIocRemainder),
    };
    ASSERT_EQ(listener.events.size(), expected.size());
    for (std::size_t i = 0; i < expected.size(); ++i) EXPECT_EQ(listener.events[i], expected[i]) << i;
}

TEST_F(MatchingTest, LimitIocWithNoLiquidityCancelsWholeOrderAsIocRemainder) {
    engine->submit(MakeLimit(1, 1, Side::kBuy, TimeInForce::kIoc, Px(25, 1000), 10));
    ASSERT_EQ(listener.events.size(), 2u);
    EXPECT_EQ(listener.events[0], Accepted(1));
    EXPECT_EQ(listener.events[1], Canceled(1, 10, CancelReason::kIocRemainder));
}

TEST_F(MatchingTest, LimitFokUnfillableLeavesBookUnchanged) {
    engine->submit(MakeLimit(1, 1, Side::kSell, TimeInForce::kDay, Px(25, 1000), 5));
    listener.events.clear();
    engine->submit(MakeLimit(2, 1, Side::kBuy, TimeInForce::kFok, Px(25, 1000), 10));
    ASSERT_EQ(listener.events.size(), 2u);
    EXPECT_EQ(listener.events[0], Accepted(2));
    EXPECT_EQ(listener.events[1], Canceled(2, 10, CancelReason::kFokUnfillable));

    TopOfBook tob = engine->top_of_book(1);
    EXPECT_EQ(tob.ask_price, Px(25, 1000));
    EXPECT_EQ(tob.ask_quantity, 5u) << "resting sell must be untouched by the failed FOK";
}

TEST_F(MatchingTest, LimitFokFillsCompletelyAcrossMultipleLevels) {
    engine->submit(MakeLimit(1, 1, Side::kSell, TimeInForce::kDay, Px(25, 1000), 5));
    engine->submit(MakeLimit(2, 1, Side::kSell, TimeInForce::kDay, Px(25, 1200), 5));
    listener.events.clear();
    engine->submit(MakeLimit(3, 1, Side::kBuy, TimeInForce::kFok, Px(25, 1200), 10));

    std::vector<RecordedEvent> expected = {
        Accepted(3),
        Trade(1, Px(25, 1000), 5, 1, 3, Side::kBuy),
        Trade(1, Px(25, 1200), 5, 2, 3, Side::kBuy),
    };
    ASSERT_EQ(listener.events.size(), expected.size());
    for (std::size_t i = 0; i < expected.size(); ++i) EXPECT_EQ(listener.events[i], expected[i]) << i;
}

TEST_F(MatchingTest, MarketIocConsumesMultipleLevelsThenNoLiquidity) {
    engine->submit(MakeLimit(1, 1, Side::kSell, TimeInForce::kDay, Px(25, 1000), 5));
    engine->submit(MakeLimit(2, 1, Side::kSell, TimeInForce::kDay, Px(25, 1200), 5));
    listener.events.clear();
    engine->submit(MakeMarket(3, 1, Side::kBuy, TimeInForce::kIoc, 20));

    std::vector<RecordedEvent> expected = {
        Accepted(3),
        Trade(1, Px(25, 1000), 5, 1, 3, Side::kBuy),
        Trade(1, Px(25, 1200), 5, 2, 3, Side::kBuy),
        Canceled(3, 10, CancelReason::kNoLiquidity),
    };
    ASSERT_EQ(listener.events.size(), expected.size());
    for (std::size_t i = 0; i < expected.size(); ++i) EXPECT_EQ(listener.events[i], expected[i]) << i;
}

TEST_F(MatchingTest, MarketFokUnfillableExecutesNothing) {
    engine->submit(MakeLimit(1, 1, Side::kSell, TimeInForce::kDay, Px(25, 1000), 5));
    listener.events.clear();
    engine->submit(MakeMarket(2, 1, Side::kBuy, TimeInForce::kFok, 10));
    ASSERT_EQ(listener.events.size(), 2u);
    EXPECT_EQ(listener.events[0], Accepted(2));
    EXPECT_EQ(listener.events[1], Canceled(2, 10, CancelReason::kFokUnfillable));
    EXPECT_EQ(engine->top_of_book(1).ask_quantity, 5u);
}

TEST_F(MatchingTest, MarketFokFillsWhenSufficientLiquidity) {
    engine->submit(MakeLimit(1, 1, Side::kSell, TimeInForce::kDay, Px(25, 1000), 5));
    engine->submit(MakeLimit(2, 1, Side::kSell, TimeInForce::kDay, Px(25, 1200), 10));
    listener.events.clear();
    engine->submit(MakeMarket(3, 1, Side::kBuy, TimeInForce::kFok, 10));

    std::vector<RecordedEvent> expected = {
        Accepted(3),
        Trade(1, Px(25, 1000), 5, 1, 3, Side::kBuy),
        Trade(1, Px(25, 1200), 5, 2, 3, Side::kBuy),
    };
    ASSERT_EQ(listener.events.size(), expected.size());
    for (std::size_t i = 0; i < expected.size(); ++i) EXPECT_EQ(listener.events[i], expected[i]) << i;
}

TEST_F(MatchingTest, TradePriceIsAlwaysTheRestingOrdersPrice) {
    // Resting sell at 25.10; aggressive buy limit at 25.16 (willing to pay
    // more) must still trade at the resting price, 25.10.
    engine->submit(MakeLimit(1, 1, Side::kSell, TimeInForce::kDay, Px(25, 1000), 10));
    listener.events.clear();
    engine->submit(MakeLimit(2, 1, Side::kBuy, TimeInForce::kIoc, Px(25, 1600), 10));
    ASSERT_EQ(listener.events.size(), 2u);
    EXPECT_EQ(listener.events[1].price, Px(25, 1000));
}

} // namespace
