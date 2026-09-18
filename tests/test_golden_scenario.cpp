// End-to-end golden scenario: a short trading session that exercises resting
// orders, a multi-level IOC sweep, a market order running out of liquidity, a
// repricing replace that gives up time priority, an unfillable FOK, a band
// rejection and a user cancel -- in one run, against one instrument.
//
// The expected event stream below is written out in full and asserted
// position by position, so any change in event ordering, trade sequencing or
// field values fails here first. It is the regression test the rest of the
// suite is factored out of.
#include <gtest/gtest.h>

#include "xmatch/matching_engine_api.hpp"
#include "test_helpers.hpp"

using namespace xmatch;
using namespace xmatch::test;

TEST(GoldenScenario, FullSessionEventForEvent) {
    RecordingListener listener;
    EngineHandle engine(&listener);

    InstrumentConfig cfg{.instrument_id = 1, .reference_price = Px(25), .band_bps = 1000};
    engine->configure(&cfg, 1);

    // 1: submit id=1, Buy Limit Day, 25.10, 100 -> accepted(1)
    engine->submit(MakeLimit(1, 1, Side::kBuy, TimeInForce::kDay, Px(25, 1000), 100));
    // 2: submit id=2, Buy Limit Day, 25.10, 50 -> accepted(2)
    engine->submit(MakeLimit(2, 1, Side::kBuy, TimeInForce::kDay, Px(25, 1000), 50));
    // 3: submit id=3, Sell Limit Day, 25.16, 80 -> accepted(3)
    engine->submit(MakeLimit(3, 1, Side::kSell, TimeInForce::kDay, Px(25, 1600), 80));
    // 4: submit id=4, Sell Limit IOC, 25.10, 120
    //    -> accepted(4), trade(T1,25.10,100,resting=1,aggr=4,Sell), trade(T2,25.10,20,resting=2,aggr=4,Sell)
    engine->submit(MakeLimit(4, 1, Side::kSell, TimeInForce::kIoc, Px(25, 1000), 120));

    {
        TopOfBook tob = engine->top_of_book(1);
        EXPECT_EQ(tob.bid_price, Px(25, 1000));
        EXPECT_EQ(tob.bid_quantity, 30u);
        EXPECT_EQ(tob.ask_price, Px(25, 1600));
        EXPECT_EQ(tob.ask_quantity, 80u);
    }

    // 5: submit id=5, Buy Market IOC, qty 100
    //    -> accepted(5), trade(T3,25.16,80,resting=3,aggr=5,Buy), canceled(5,20,kNoLiquidity)
    engine->submit(MakeMarket(5, 1, Side::kBuy, TimeInForce::kIoc, 100));

    // 6: replace id=2->6, 25.12, new_qty=60 -> replaced(2,6,25.12,40)
    engine->replace(ReplaceOrder{.order_id = 2, .new_order_id = 6, .new_price = Px(25, 1200), .new_quantity = 60});

    // 7: submit id=7, Sell Limit FOK, 25.12, 50 -> accepted(7), canceled(7,50,kFokUnfillable)
    engine->submit(MakeLimit(7, 1, Side::kSell, TimeInForce::kFok, Px(25, 1200), 50));

    // 8: submit id=8, Buy Limit Day, 28.00, 10 -> rejected(8, PriceOutOfBand)
    engine->submit(MakeLimit(8, 1, Side::kBuy, TimeInForce::kDay, Px(28), 10));

    // 9: cancel id=6 -> canceled(6, 40, kUserRequested)
    engine->cancel(CancelOrder{.order_id = 6});

    {
        TopOfBook tob = engine->top_of_book(1);
        EXPECT_EQ(tob.bid_quantity, 0u) << "book must be empty after step 9";
        EXPECT_EQ(tob.ask_quantity, 0u) << "book must be empty after step 9";
    }

    std::vector<RecordedEvent> expected = {
        Accepted(1),
        Accepted(2),
        Accepted(3),
        Accepted(4),
        Trade(1, Px(25, 1000), 100, /*resting=*/1, /*aggr=*/4, Side::kSell),
        Trade(1, Px(25, 1000), 20, /*resting=*/2, /*aggr=*/4, Side::kSell),
        Accepted(5),
        Trade(1, Px(25, 1600), 80, /*resting=*/3, /*aggr=*/5, Side::kBuy),
        Canceled(5, 20, CancelReason::kNoLiquidity),
        Replaced(2, 6, Px(25, 1200), 40),
        Accepted(7),
        Canceled(7, 50, CancelReason::kFokUnfillable),
        Rejected(8, RejectReason::kPriceOutOfBand),
        Canceled(6, 40, CancelReason::kUserRequested),
    };

    ASSERT_EQ(listener.events.size(), expected.size());
    for (std::size_t i = 0; i < expected.size(); ++i) {
        EXPECT_EQ(listener.events[i], expected[i]) << "event #" << i;
    }
}
