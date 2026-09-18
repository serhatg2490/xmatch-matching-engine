#include <gtest/gtest.h>

#include "xmatch/matching_engine_api.hpp"
#include "test_helpers.hpp"

using namespace xmatch;
using namespace xmatch::test;

namespace {

class CancelReplaceTest : public ::testing::Test {
protected:
    RecordingListener listener;
    EngineHandle engine{&listener};

    void SetUp() override {
        InstrumentConfig cfg{.instrument_id = 1, .reference_price = Px(25), .band_bps = 1000};
        engine->configure(&cfg, 1);
    }
};

TEST_F(CancelReplaceTest, CancelUnknownOrderRejected) {
    engine->cancel(CancelOrder{.order_id = 999});
    ASSERT_EQ(listener.events.size(), 1u);
    EXPECT_EQ(listener.events[0], Rejected(999, RejectReason::kUnknownOrder));
}

TEST_F(CancelReplaceTest, DoubleCancelRejectedSecondTime) {
    engine->submit(MakeLimit(1, 1, Side::kBuy, TimeInForce::kDay, Px(25, 1000), 10));
    engine->cancel(CancelOrder{.order_id = 1});
    engine->cancel(CancelOrder{.order_id = 1});
    ASSERT_EQ(listener.events.size(), 3u);
    EXPECT_EQ(listener.events[2], Rejected(1, RejectReason::kUnknownOrder));
}

TEST_F(CancelReplaceTest, CancelFullyFilledOrderRejected) {
    engine->submit(MakeLimit(1, 1, Side::kSell, TimeInForce::kDay, Px(25, 1000), 10));
    engine->submit(MakeLimit(2, 1, Side::kBuy, TimeInForce::kIoc, Px(25, 1000), 10)); // fully fills id1
    engine->cancel(CancelOrder{.order_id = 1});
    EXPECT_EQ(listener.events.back(), Rejected(1, RejectReason::kUnknownOrder));
}

TEST_F(CancelReplaceTest, ReplaceUnknownOrderRejected) {
    engine->replace(ReplaceOrder{.order_id = 999, .new_order_id = 1000, .new_price = Px(25), .new_quantity = 10});
    ASSERT_EQ(listener.events.size(), 1u);
    EXPECT_EQ(listener.events[0], Rejected(1000, RejectReason::kUnknownOrder));
}

TEST_F(CancelReplaceTest, QuantityDecreaseAtSamePriceKeepsTimePriority) {
    engine->submit(MakeLimit(1, 1, Side::kBuy, TimeInForce::kDay, Px(25, 1000), 100)); // head
    engine->submit(MakeLimit(2, 1, Side::kBuy, TimeInForce::kDay, Px(25, 1000), 50));  // tail
    listener.events.clear();

    engine->replace(ReplaceOrder{.order_id = 1, .new_order_id = 10, .new_price = Px(25, 1000), .new_quantity = 60});
    ASSERT_EQ(listener.events.size(), 1u);
    EXPECT_EQ(listener.events[0], Replaced(1, 10, Px(25, 1000), 60));

    listener.events.clear();
    engine->submit(MakeLimit(3, 1, Side::kSell, TimeInForce::kIoc, Px(25, 1000), 70));
    // id10 (was id1) must still be head: fills first, for its full 60, then
    // id2 fills for the remaining 10.
    std::vector<RecordedEvent> expected = {
        Accepted(3),
        Trade(1, Px(25, 1000), 60, /*resting=*/10, /*aggr=*/3, Side::kSell),
        Trade(1, Px(25, 1000), 10, /*resting=*/2, /*aggr=*/3, Side::kSell),
    };
    ASSERT_EQ(listener.events.size(), expected.size());
    for (std::size_t i = 0; i < expected.size(); ++i) EXPECT_EQ(listener.events[i], expected[i]) << i;
}

TEST_F(CancelReplaceTest, QuantityIncreaseAtSamePriceLosesTimePriority) {
    engine->submit(MakeLimit(1, 1, Side::kBuy, TimeInForce::kDay, Px(25, 1000), 50)); // head
    engine->submit(MakeLimit(2, 1, Side::kBuy, TimeInForce::kDay, Px(25, 1000), 50)); // tail
    listener.events.clear();

    engine->replace(ReplaceOrder{.order_id = 1, .new_order_id = 10, .new_price = Px(25, 1000), .new_quantity = 80});
    ASSERT_EQ(listener.events.size(), 1u);
    EXPECT_EQ(listener.events[0], Replaced(1, 10, Px(25, 1000), 80));

    listener.events.clear();
    engine->submit(MakeLimit(3, 1, Side::kSell, TimeInForce::kIoc, Px(25, 1000), 60));
    // id2 kept its original head position (never touched); id10 now sits
    // behind it, having lost priority via the quantity increase.
    std::vector<RecordedEvent> expected = {
        Accepted(3),
        Trade(1, Px(25, 1000), 50, /*resting=*/2, /*aggr=*/3, Side::kSell),
        Trade(1, Px(25, 1000), 10, /*resting=*/10, /*aggr=*/3, Side::kSell),
    };
    ASSERT_EQ(listener.events.size(), expected.size());
    for (std::size_t i = 0; i < expected.size(); ++i) EXPECT_EQ(listener.events[i], expected[i]) << i;
}

TEST_F(CancelReplaceTest, PriceChangeMakesOrderMarketableAfterReplace) {
    engine->submit(MakeLimit(1, 1, Side::kSell, TimeInForce::kDay, Px(25, 1200), 30)); // resting ask
    engine->submit(MakeLimit(2, 1, Side::kBuy, TimeInForce::kDay, Px(25, 1000), 50));  // non-marketable bid
    listener.events.clear();

    // Move id2's price up to 25.12: now crosses the resting ask.
    engine->replace(ReplaceOrder{.order_id = 2, .new_order_id = 20, .new_price = Px(25, 1200), .new_quantity = 50});

    std::vector<RecordedEvent> expected = {
        Replaced(2, 20, Px(25, 1200), 50),
        Trade(1, Px(25, 1200), 30, /*resting=*/1, /*aggr=*/20, Side::kBuy),
    };
    ASSERT_EQ(listener.events.size(), expected.size());
    for (std::size_t i = 0; i < expected.size(); ++i) EXPECT_EQ(listener.events[i], expected[i]) << i;

    TopOfBook tob = engine->top_of_book(1);
    EXPECT_EQ(tob.ask_quantity, 0u);
    EXPECT_EQ(tob.bid_price, Px(25, 1200));
    EXPECT_EQ(tob.bid_quantity, 20u) << "remainder (50-30) rests at the new price";
}

TEST_F(CancelReplaceTest, ReplaceDownToZeroOpenQuantityIsSimplyCanceled) {
    engine->submit(MakeLimit(1, 1, Side::kBuy, TimeInForce::kDay, Px(25, 1000), 100));
    engine->submit(MakeLimit(2, 1, Side::kSell, TimeInForce::kIoc, Px(25, 1000), 40)); // partial fill: open=60, filled=40
    listener.events.clear();

    engine->replace(ReplaceOrder{.order_id = 1, .new_order_id = 10, .new_price = Px(25, 1000), .new_quantity = 40});
    ASSERT_EQ(listener.events.size(), 1u);
    EXPECT_EQ(listener.events[0], Canceled(1, 60, CancelReason::kUserRequested));

    EXPECT_EQ(engine->top_of_book(1).bid_quantity, 0u);

    // new_order_id must NOT have been consumed.
    listener.events.clear();
    engine->submit(MakeLimit(10, 1, Side::kBuy, TimeInForce::kDay, Px(25, 1000), 5));
    ASSERT_EQ(listener.events.size(), 1u);
    EXPECT_EQ(listener.events[0], Accepted(10));
}

TEST_F(CancelReplaceTest, ReplaceTickFailureLeavesOldOrderUntouched) {
    engine->submit(MakeLimit(1, 1, Side::kBuy, TimeInForce::kDay, Px(25, 1000), 50));
    listener.events.clear();

    engine->replace(ReplaceOrder{.order_id = 1, .new_order_id = 10, .new_price = Px(25, 1100), .new_quantity = 50});
    ASSERT_EQ(listener.events.size(), 1u);
    EXPECT_EQ(listener.events[0], Rejected(10, RejectReason::kInvalidPriceTick));

    TopOfBook tob = engine->top_of_book(1);
    EXPECT_EQ(tob.bid_price, Px(25, 1000));
    EXPECT_EQ(tob.bid_quantity, 50u);

    // original id still cancellable; new_order_id was never consumed.
    listener.events.clear();
    engine->cancel(CancelOrder{.order_id = 1});
    EXPECT_EQ(listener.events[0], Canceled(1, 50, CancelReason::kUserRequested));
}

TEST_F(CancelReplaceTest, ReplaceBandFailureLeavesOldOrderUntouched) {
    engine->submit(MakeLimit(1, 1, Side::kBuy, TimeInForce::kDay, Px(25, 1000), 50));
    listener.events.clear();

    engine->replace(ReplaceOrder{.order_id = 1, .new_order_id = 10, .new_price = Px(28), .new_quantity = 50});
    ASSERT_EQ(listener.events.size(), 1u);
    EXPECT_EQ(listener.events[0], Rejected(10, RejectReason::kPriceOutOfBand));
    EXPECT_EQ(engine->top_of_book(1).bid_quantity, 50u);
}

TEST_F(CancelReplaceTest, ReplaceWithDuplicateNewOrderIdRejected) {
    engine->submit(MakeLimit(1, 1, Side::kBuy, TimeInForce::kDay, Px(25, 1000), 50));
    engine->submit(MakeLimit(2, 1, Side::kBuy, TimeInForce::kDay, Px(25, 1000), 20));
    listener.events.clear();

    engine->replace(ReplaceOrder{.order_id = 1, .new_order_id = 2, .new_price = Px(25, 1200), .new_quantity = 50});
    ASSERT_EQ(listener.events.size(), 1u);
    EXPECT_EQ(listener.events[0], Rejected(2, RejectReason::kDuplicateOrderId));
    EXPECT_EQ(engine->top_of_book(1).bid_quantity, 70u) << "both original orders must remain resting";
}

TEST_F(CancelReplaceTest, SelfIdReplaceModifiesInPlace) {
    engine->submit(MakeLimit(1, 1, Side::kBuy, TimeInForce::kDay, Px(25, 1000), 50));
    listener.events.clear();

    engine->replace(ReplaceOrder{.order_id = 1, .new_order_id = 1, .new_price = Px(25, 1200), .new_quantity = 30});
    ASSERT_EQ(listener.events.size(), 1u);
    EXPECT_EQ(listener.events[0], Replaced(1, 1, Px(25, 1200), 30));

    listener.events.clear();
    engine->cancel(CancelOrder{.order_id = 1});
    ASSERT_EQ(listener.events.size(), 1u);
    EXPECT_EQ(listener.events[0], Canceled(1, 30, CancelReason::kUserRequested));
}

TEST_F(CancelReplaceTest, ReplaceNewQuantityBelowAlreadyFilledIsClampedNotUnderflowed) {
    engine->submit(MakeLimit(1, 1, Side::kBuy, TimeInForce::kDay, Px(25, 1000), 100));
    engine->submit(MakeLimit(2, 1, Side::kSell, TimeInForce::kIoc, Px(25, 1000), 70)); // open=30, filled=70
    listener.events.clear();

    // new_quantity (10) is less than already_filled (70): open must clamp to 0, not underflow.
    engine->replace(ReplaceOrder{.order_id = 1, .new_order_id = 10, .new_price = Px(25, 1000), .new_quantity = 10});
    ASSERT_EQ(listener.events.size(), 1u);
    EXPECT_EQ(listener.events[0], Canceled(1, 30, CancelReason::kUserRequested));
}

} // namespace
