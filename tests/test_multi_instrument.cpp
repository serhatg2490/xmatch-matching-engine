#include <gtest/gtest.h>

#include <vector>

#include "xmatch/matching_engine_api.hpp"
#include "test_helpers.hpp"

using namespace xmatch;
using namespace xmatch::test;

namespace {

TEST(MultiInstrumentTest, OrdersOnOneInstrumentDoNotAffectAnother) {
    RecordingListener listener;
    EngineHandle engine(&listener);

    std::vector<InstrumentConfig> cfgs = {
        InstrumentConfig{.instrument_id = 1, .reference_price = Px(25), .band_bps = 1000},
        InstrumentConfig{.instrument_id = 2, .reference_price = Px(60), .band_bps = 1000},
    };
    engine->configure(cfgs.data(), cfgs.size());

    engine->submit(MakeLimit(1, 1, Side::kBuy, TimeInForce::kDay, Px(25, 1000), 10));
    engine->submit(MakeLimit(2, 2, Side::kBuy, TimeInForce::kDay, Px(60), 20));

    TopOfBook tob1 = engine->top_of_book(1);
    TopOfBook tob2 = engine->top_of_book(2);
    EXPECT_EQ(tob1.bid_price, Px(25, 1000));
    EXPECT_EQ(tob1.bid_quantity, 10u);
    EXPECT_EQ(tob2.bid_price, Px(60));
    EXPECT_EQ(tob2.bid_quantity, 20u);

    // A marketable sell on instrument 2 must not touch instrument 1's book.
    engine->submit(MakeLimit(3, 2, Side::kSell, TimeInForce::kIoc, Px(60), 20));
    EXPECT_EQ(engine->top_of_book(1).bid_quantity, 10u);
    EXPECT_EQ(engine->top_of_book(2).bid_quantity, 0u);
}

TEST(MultiInstrumentTest, DifferentTickRegimesGetDifferentBands) {
    RecordingListener listener;
    EngineHandle engine(&listener);

    // Instrument 3: reference 120 TRY -> tick 0.10, band [108.00, 132.00].
    std::vector<InstrumentConfig> cfgs = {
        InstrumentConfig{.instrument_id = 3, .reference_price = Px(120), .band_bps = 1000},
    };
    engine->configure(cfgs.data(), cfgs.size());

    // 108.05 is not a multiple of 0.10 -> invalid tick.
    engine->submit(MakeLimit(1, 3, Side::kBuy, TimeInForce::kDay, Px(108, 500), 10));
    ASSERT_EQ(listener.events.size(), 1u);
    EXPECT_EQ(listener.events[0], Rejected(1, RejectReason::kInvalidPriceTick));

    listener.events.clear();
    engine->submit(MakeLimit(2, 3, Side::kBuy, TimeInForce::kDay, Px(108), 10)); // lower band edge, tick-aligned
    ASSERT_EQ(listener.events.size(), 1u);
    EXPECT_EQ(listener.events[0], Accepted(2));

    listener.events.clear();
    engine->submit(MakeLimit(3, 3, Side::kBuy, TimeInForce::kDay, Px(132), 10)); // upper band edge
    ASSERT_EQ(listener.events.size(), 1u);
    EXPECT_EQ(listener.events[0], Accepted(3));
}

TEST(MultiInstrumentTest, HundredPlusInstrumentsEachIndependentlyCorrect) {
    RecordingListener listener;
    EngineHandle engine(&listener);

    std::vector<InstrumentConfig> cfgs;
    constexpr int kCount = 150;
    for (int i = 0; i < kCount; ++i) {
        cfgs.push_back(InstrumentConfig{.instrument_id = static_cast<InstrumentId>(i + 1),
                                         .reference_price = Px(10 + i),
                                         .band_bps = 1000});
    }
    engine->configure(cfgs.data(), cfgs.size());

    OrderId next_id = 1;
    for (int i = 0; i < kCount; ++i) {
        InstrumentId inst = static_cast<InstrumentId>(i + 1);
        Price ref = Px(10 + i);
        engine->submit(MakeLimit(next_id++, inst, Side::kBuy, TimeInForce::kDay, ref, 10));
    }
    for (int i = 0; i < kCount; ++i) {
        InstrumentId inst = static_cast<InstrumentId>(i + 1);
        TopOfBook tob = engine->top_of_book(inst);
        EXPECT_EQ(tob.bid_price, Px(10 + i)) << "instrument " << inst;
        EXPECT_EQ(tob.bid_quantity, 10u) << "instrument " << inst;
        EXPECT_EQ(tob.ask_quantity, 0u) << "instrument " << inst;
    }
    EXPECT_EQ(listener.events.size(), static_cast<std::size_t>(kCount));
}

TEST(MultiInstrumentTest, TopOfBookOnUnconfiguredInstrumentIsSafelyEmpty) {
    RecordingListener listener;
    EngineHandle engine(&listener);
    InstrumentConfig cfg{.instrument_id = 1, .reference_price = Px(25), .band_bps = 1000};
    engine->configure(&cfg, 1);

    TopOfBook tob = engine->top_of_book(42);
    EXPECT_EQ(tob.bid_quantity, 0u);
    EXPECT_EQ(tob.ask_quantity, 0u);
}

} // namespace
