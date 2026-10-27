#include <gtest/gtest.h>

#include <vector>

#include "xmatch/matching_engine_api.hpp"
#include "test_helpers.hpp"

using namespace xmatch;
using namespace xmatch::test;

namespace {

TEST(ConfigureTest, ConfigureAfterFirstSuccessfulConfigureIsIgnored) {
    RecordingListener listener;
    EngineHandle engine(&listener);
    InstrumentConfig cfg{.instrument_id = 1, .reference_price = Px(25), .band_bps = 1000};
    ASSERT_EQ(engine->configure(&cfg, 1), ConfigureResult::kOk);
    engine->submit(MakeLimit(1, 1, Side::kBuy, TimeInForce::kDay, Px(25), 10));

    InstrumentConfig other{.instrument_id = 2, .reference_price = Px(60), .band_bps = 1000};
    ASSERT_EQ(engine->configure(&cfg, 1), ConfigureResult::kAlreadyConfigured);
    ASSERT_EQ(engine->configure(&other, 1), ConfigureResult::kAlreadyConfigured);

    // The resting order survives, stays cancelable, and the book stays consistent.
    TopOfBook tob = engine->top_of_book(1);
    EXPECT_EQ(tob.bid_price, Px(25));
    EXPECT_EQ(tob.bid_quantity, 10u);

    listener.events.clear();
    engine->cancel(CancelOrder{.order_id = 1});
    engine->submit(MakeLimit(2, 1, Side::kBuy, TimeInForce::kDay, Px(25), 5));
    engine->submit(MakeLimit(3, 2, Side::kBuy, TimeInForce::kDay, Px(60), 5));
    ASSERT_EQ(listener.events.size(), 3u);
    EXPECT_EQ(listener.events[0], Canceled(1, 10, CancelReason::kUserRequested));
    EXPECT_EQ(listener.events[1], Accepted(2));
    EXPECT_EQ(listener.events[2], Rejected(3, RejectReason::kUnknownInstrument));
    EXPECT_EQ(engine->top_of_book(1).bid_quantity, 5u);
}

TEST(ConfigureTest, CanBeConfiguredAfterFirstConfigureFails) {
    RecordingListener listener;
    EngineHandle engine(&listener);
    InstrumentConfig cfg{.instrument_id = 1, .reference_price = Px(25), .band_bps = 1000};
    // Configuring with a null pointer and SIZE_MAX returns ConfigureResult::kInvalidArgument
    ASSERT_EQ(engine->configure(nullptr, SIZE_MAX), ConfigureResult::kInvalidArgument);
    // Now configure with a proper instrument, which should succeed 
    // and leave the engine usable.
    ASSERT_EQ(engine->configure(&cfg, 1), ConfigureResult::kOk);
    engine->submit(MakeLimit(1, 1, Side::kBuy, TimeInForce::kDay, Px(25), 10));

    // There is one resting order on instrument 1, and the book is consistent.
    TopOfBook tob = engine->top_of_book(1);
    EXPECT_EQ(tob.bid_price, Px(25));
    EXPECT_EQ(tob.bid_quantity, 10u);
    ASSERT_EQ(listener.events.size(), 1u);
    EXPECT_EQ(listener.events[0], Accepted(1));
}

TEST(ConfigureTest, InvalidConfigurationParametersAreRejected) {
    RecordingListener listener;
    EngineHandle engine(&listener);
    InstrumentConfig cfg{.instrument_id = 1, .reference_price = Px(25), .band_bps = 1000};
    ASSERT_EQ(engine->configure(nullptr, 0), ConfigureResult::kInvalidArgument);
    ASSERT_EQ(engine->configure(nullptr, 1), ConfigureResult::kInvalidArgument);
    ASSERT_EQ(engine->configure(&cfg, std::size_t{UINT32_MAX} + 1), ConfigureResult::kInvalidArgument);
    ASSERT_EQ(engine->configure(&cfg, 0), ConfigureResult::kInvalidArgument);
    // Reference price is zero
    InstrumentConfig cfg2{.instrument_id = 1, .reference_price = Px(0), .band_bps = 1000};
    ASSERT_EQ(engine->configure(&cfg2, 1), ConfigureResult::kInvalidArgument);
    // Reference price is negative
    InstrumentConfig cfg3{.instrument_id = 1, .reference_price = Px(-1), .band_bps = 1000};
    ASSERT_EQ(engine->configure(&cfg3, 1), ConfigureResult::kInvalidArgument);
    // Bps > kMaxBandBps
    InstrumentConfig cfg4{.instrument_id = 1, .reference_price = Px(25), .band_bps = 10001};
    ASSERT_EQ(engine->configure(&cfg4, 1), ConfigureResult::kInvalidArgument);

    // Duplicate instrument ids
    std::vector<InstrumentConfig> cfgs = {
        InstrumentConfig{.instrument_id = 1, .reference_price = Px(25), .band_bps = 1000},
        InstrumentConfig{.instrument_id = 1, .reference_price = Px(60), .band_bps = 1000},
    };
    ASSERT_EQ(engine->configure(cfgs.data(), cfgs.size()), ConfigureResult::kInvalidArgument);

    // Reference price is not tick aligned
    InstrumentConfig cfg5{.instrument_id = 1, .reference_price = Px(25, 50), .band_bps = 1000};
    ASSERT_EQ(engine->configure(&cfg5, 1), ConfigureResult::kInvalidArgument);

    // Reference price is too large
    InstrumentConfig cfg6{.instrument_id = 1,
                                        // ceil_to_tick(std::numeric_limits<Price>::max() / kMaxBandBps + 1)
                                       .reference_price = Price{922337203686000},
                                       .band_bps = 1000};
    ASSERT_EQ(engine->configure(&cfg6, 1), ConfigureResult::kInvalidArgument);

    // Bps == kMaxBandBps and Bps = 0 are accepted
    {
        EngineHandle engine2(&listener);
        InstrumentConfig cfg7{.instrument_id = 1, .reference_price = Px(25), .band_bps = kMaxBandBps};
        ASSERT_EQ(engine2->configure(&cfg7, 1), ConfigureResult::kOk);
    }
    {
        EngineHandle engine3(&listener);
        InstrumentConfig cfg8{.instrument_id = 1, .reference_price = Px(25), .band_bps = 0};
        ASSERT_EQ(engine3->configure(&cfg8, 1), ConfigureResult::kOk);
    }
}

TEST(ConfigureTest, SubmitCancelReplaceAreRejectedWithUnconfiguredEngine) {
    RecordingListener listener;
    EngineHandle engine(&listener);
    engine->submit(MakeLimit(1, 1, Side::kBuy, TimeInForce::kDay, Px(25), 10));
    engine->cancel(CancelOrder{.order_id = 2});
    engine->replace(ReplaceOrder{.order_id = 3, .new_order_id = 10, .new_price = Px(25, 1000), .new_quantity = 60});
    ASSERT_EQ(listener.events.size(), 3u);
    EXPECT_EQ(listener.events[0], Rejected(1, RejectReason::kNotConfigured));
    EXPECT_EQ(listener.events[1], Rejected(2, RejectReason::kNotConfigured));
    EXPECT_EQ(listener.events[2], Rejected(10, RejectReason::kNotConfigured));

    listener.events.clear();

    // Submit/cancel/replace requests after invalid config attempt are rejected too
    ASSERT_EQ(engine->configure(nullptr, 0), ConfigureResult::kInvalidArgument);
    engine->submit(MakeLimit(1, 1, Side::kBuy, TimeInForce::kDay, Px(25), 10));
    engine->cancel(CancelOrder{.order_id = 2});
    engine->replace(ReplaceOrder{.order_id = 3, .new_order_id = 10, .new_price = Px(25, 1000), .new_quantity = 60});
    ASSERT_EQ(listener.events.size(), 3u);
    EXPECT_EQ(listener.events[0], Rejected(1, RejectReason::kNotConfigured));
    EXPECT_EQ(listener.events[1], Rejected(2, RejectReason::kNotConfigured));
    EXPECT_EQ(listener.events[2], Rejected(10, RejectReason::kNotConfigured));

    // Now configure with a valid configuration
    InstrumentConfig cfg{.instrument_id = 1, .reference_price = Px(25), .band_bps = 1000};
    listener.events.clear();
    ASSERT_EQ(engine->configure(&cfg, 1), ConfigureResult::kOk);
    // Submit an order
    engine->submit(MakeLimit(1, 1, Side::kBuy, TimeInForce::kDay, Px(25), 10));
    // There is one resting order on instrument 1, and the book is consistent.
    TopOfBook tob = engine->top_of_book(1);
    EXPECT_EQ(tob.bid_price, Px(25));
    EXPECT_EQ(tob.bid_quantity, 10u);
    ASSERT_EQ(listener.events.size(), 1u);
    EXPECT_EQ(listener.events[0], Accepted(1));
}
} // namespace

