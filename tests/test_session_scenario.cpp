// End-to-end session scenario: a short run against one instrument in the
// 0.05-tick regime that exercises resting orders, an IOC sweep that fills in
// full across two resting orders, a same-price size reduction of a partially
// filled order, an IOC remainder, an FOK that fills exactly, a tick reject and
// a cancel of an id that is no longer open.
//
// The expected event stream below is written out in full and asserted
// position by position, so any change in event ordering, trade sequencing or
// field values fails here first.
#include <gtest/gtest.h>

#include "xmatch/matching_engine_api.hpp"
#include "test_helpers.hpp"

using namespace xmatch;
using namespace xmatch::test;

TEST(SessionScenario, FullSessionEventForEvent) {
    RecordingListener listener;
    EngineHandle engine(&listener);

    // Reference 80.00, +/-5% band -> [76.00, 84.00], tick 0.05.
    InstrumentConfig cfg{.instrument_id = 7, .reference_price = Px(80), .band_bps = 500};
    ASSERT_EQ(engine->configure(&cfg, 1), ConfigureResult::kOk);

    // 1: submit id=101, Sell Limit Day, 80.50, 200 -> accepted(101)
    engine->submit(MakeLimit(101, 7, Side::kSell, TimeInForce::kDay, Px(80, 5000), 200));
    // 2: submit id=102, Sell Limit Day, 80.50, 100 -> accepted(102)
    engine->submit(MakeLimit(102, 7, Side::kSell, TimeInForce::kDay, Px(80, 5000), 100));
    // 3: submit id=103, Buy Limit Day, 80.00, 150 -> accepted(103)
    engine->submit(MakeLimit(103, 7, Side::kBuy, TimeInForce::kDay, Px(80), 150));
    // 4: submit id=104, Buy Limit IOC, 80.50, 250
    //    -> accepted(104), trade(80.50,200,resting=101,aggr=104,Buy), trade(80.50,50,resting=102,aggr=104,Buy)
    engine->submit(MakeLimit(104, 7, Side::kBuy, TimeInForce::kIoc, Px(80, 5000), 250));

    {
        TopOfBook tob = engine->top_of_book(7);
        EXPECT_EQ(tob.bid_price, Px(80));
        EXPECT_EQ(tob.bid_quantity, 150u);
        EXPECT_EQ(tob.ask_price, Px(80, 5000));
        EXPECT_EQ(tob.ask_quantity, 50u);
    }

    // 5: replace id=102->105, 80.50, new_qty=80 (50 already filled) -> replaced(102,105,80.50,30)
    engine->replace(ReplaceOrder{.order_id = 102, .new_order_id = 105, .new_price = Px(80, 5000), .new_quantity = 80});

    // 6: submit id=106, Sell Limit IOC, 79.90, 200
    //    -> accepted(106), trade(80.00,150,resting=103,aggr=106,Sell), canceled(106,50,kIocRemainder)
    engine->submit(MakeLimit(106, 7, Side::kSell, TimeInForce::kIoc, Px(79, 9000), 200));

    // 7: submit id=107, Buy Limit FOK, 80.50, 30 -> accepted(107), trade(80.50,30,resting=105,aggr=107,Buy)
    engine->submit(MakeLimit(107, 7, Side::kBuy, TimeInForce::kFok, Px(80, 5000), 30));

    // 8: submit id=108, Buy Limit Day, 79.97, 10 -> rejected(108, InvalidPriceTick)
    engine->submit(MakeLimit(108, 7, Side::kBuy, TimeInForce::kDay, Px(79, 9700), 10));

    // 9: cancel id=105 (fully filled in step 7) -> rejected(105, UnknownOrder)
    engine->cancel(CancelOrder{.order_id = 105});

    {
        TopOfBook tob = engine->top_of_book(7);
        EXPECT_EQ(tob.bid_quantity, 0u) << "book must be empty after step 9";
        EXPECT_EQ(tob.ask_quantity, 0u) << "book must be empty after step 9";
    }

    std::vector<RecordedEvent> expected = {
        Accepted(101),
        Accepted(102),
        Accepted(103),
        Accepted(104),
        Trade(7, Px(80, 5000), 200, /*resting=*/101, /*aggr=*/104, Side::kBuy),
        Trade(7, Px(80, 5000), 50, /*resting=*/102, /*aggr=*/104, Side::kBuy),
        Replaced(102, 105, Px(80, 5000), 30),
        Accepted(106),
        Trade(7, Px(80), 150, /*resting=*/103, /*aggr=*/106, Side::kSell),
        Canceled(106, 50, CancelReason::kIocRemainder),
        Accepted(107),
        Trade(7, Px(80, 5000), 30, /*resting=*/105, /*aggr=*/107, Side::kBuy),
        Rejected(108, RejectReason::kInvalidPriceTick),
        Rejected(105, RejectReason::kUnknownOrder),
    };

    ASSERT_EQ(listener.events.size(), expected.size());
    for (std::size_t i = 0; i < expected.size(); ++i) {
        EXPECT_EQ(listener.events[i], expected[i]) << "event #" << i;
    }
}
