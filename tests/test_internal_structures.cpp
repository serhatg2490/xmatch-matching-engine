// Focused unit tests for the low-level building blocks, independent of the
// IMatchingEngine surface: the tick table, the price ladder (band -> array
// index mapping), the level bitset (best-price relocation), the flat hash
// map (order id index), and the chunked order pool.
#include <gtest/gtest.h>

#include "engine/flat_hash_map.hpp"
#include "engine/level_bitset.hpp"
#include "engine/order_pool.hpp"
#include "engine/price_ladder.hpp"
#include "engine/tick_table.hpp"

using namespace xmatch;
using namespace xmatch::detail;

// --------------------------------------------------------------------- //
// tick_table
// --------------------------------------------------------------------- //

TEST(TickTable, BracketBoundaries) {
    EXPECT_EQ(tick_size(1), 100);
    EXPECT_EQ(tick_size(199900), 100);
    EXPECT_EQ(tick_size(200000), 200);
    EXPECT_EQ(tick_size(499800), 200);
    EXPECT_EQ(tick_size(500000), 500);
    EXPECT_EQ(tick_size(999500), 500);
    EXPECT_EQ(tick_size(1000000), 1000);
    EXPECT_EQ(tick_size(5000000), 1000);
}

TEST(TickTable, AlignmentChecks) {
    EXPECT_TRUE(is_tick_aligned(251000));  // 25.10, tick 0.02
    EXPECT_FALSE(is_tick_aligned(251100)); // 25.11, not a multiple of 0.02
    EXPECT_TRUE(is_tick_aligned(100));     // 0.01, smallest bracket
    EXPECT_FALSE(is_tick_aligned(0));
    EXPECT_FALSE(is_tick_aligned(-100));
    EXPECT_TRUE(is_tick_aligned(1000000)); // exactly at the 100 TRY boundary, tick 0.10
    EXPECT_FALSE(is_tick_aligned(1000050));
}

TEST(TickTable, CeilAndFloorToTick) {
    EXPECT_EQ(ceil_to_tick(251001), 251200);  // next 0.02 step up
    EXPECT_EQ(ceil_to_tick(251000), 251000);  // already aligned
    EXPECT_EQ(floor_to_tick(251199), 251000);
    EXPECT_EQ(floor_to_tick(251000), 251000);
}

// --------------------------------------------------------------------- //
// PriceLadder
// --------------------------------------------------------------------- //

TEST(PriceLadderTest, RoundTripWithinSingleRegime) {
    PriceLadder ladder;
    ladder.build(225000, 275000); // 25.00 reference, +/-10% band
    ASSERT_GT(ladder.size(), 0u);
    for (Price p = 225000; p <= 275000; p += 200) {
        auto idx = ladder.price_to_index(p);
        ASSERT_GE(idx, 0) << p;
        EXPECT_EQ(ladder.index_to_price(static_cast<std::uint32_t>(idx)), p);
    }
}

TEST(PriceLadderTest, OutOfBandAndMisalignedReturnNegativeOne) {
    PriceLadder ladder;
    ladder.build(225000, 275000);
    EXPECT_EQ(ladder.price_to_index(224800), -1); // just below band
    EXPECT_EQ(ladder.price_to_index(275200), -1); // just above band
    EXPECT_EQ(ladder.price_to_index(225100), -1); // inside band, wrong tick
}

TEST(PriceLadderTest, BandSpanningTwoTickRegimes) {
    // Reference just below the 20 TRY breakpoint with a wide band forces
    // the ladder to straddle the 0.01 / 0.02 regimes.
    PriceLadder ladder;
    Price lo = ceil_to_tick(180000);  // 18.00
    Price hi = floor_to_tick(220000); // 22.00
    ladder.build(lo, hi);

    // Just below the breakpoint: tick 0.01.
    auto below = ladder.price_to_index(199900);
    ASSERT_GE(below, 0);
    EXPECT_EQ(ladder.index_to_price(static_cast<std::uint32_t>(below)), 199900);

    // At/after the breakpoint: tick 0.02.
    auto at = ladder.price_to_index(200000);
    ASSERT_GE(at, 0);
    EXPECT_EQ(ladder.index_to_price(static_cast<std::uint32_t>(at)), 200000);
    EXPECT_EQ(ladder.price_to_index(200100), -1) << "200100 is not a 0.02 multiple past the breakpoint";

    auto above = ladder.price_to_index(200200);
    ASSERT_GE(above, 0);
    EXPECT_EQ(static_cast<std::uint32_t>(above), static_cast<std::uint32_t>(at) + 1)
        << "indices must stay contiguous across the regime boundary";
}

// --------------------------------------------------------------------- //
// LevelBitset
// --------------------------------------------------------------------- //

TEST(LevelBitsetTest, FindHighestAndLowestAcrossWordBoundaries) {
    LevelBitset bs;
    bs.resize(200);
    bs.set(5);
    bs.set(63);
    bs.set(64);
    bs.set(150);

    EXPECT_EQ(bs.find_highest_le(199), 150);
    EXPECT_EQ(bs.find_highest_le(149), 64);
    EXPECT_EQ(bs.find_highest_le(64), 64);
    EXPECT_EQ(bs.find_highest_le(63), 63);
    EXPECT_EQ(bs.find_highest_le(5), 5);
    EXPECT_EQ(bs.find_highest_le(4), -1);

    EXPECT_EQ(bs.find_lowest_ge(0), 5);
    EXPECT_EQ(bs.find_lowest_ge(6), 63);
    EXPECT_EQ(bs.find_lowest_ge(64), 64);
    EXPECT_EQ(bs.find_lowest_ge(65), 150);
    EXPECT_EQ(bs.find_lowest_ge(151), -1);

    bs.clear(63);
    EXPECT_EQ(bs.find_lowest_ge(6), 64);
}

// --------------------------------------------------------------------- //
// FlatHashMap
// --------------------------------------------------------------------- //

TEST(FlatHashMapTest, InsertFindContains) {
    FlatHashMap map;
    map.insert(42, 7);
    map.insert(1000, 8);
    EXPECT_TRUE(map.contains(42));
    EXPECT_TRUE(map.contains(1000));
    EXPECT_FALSE(map.contains(43));
    ASSERT_NE(map.find(42), nullptr);
    EXPECT_EQ(*map.find(42), 7u);
}

TEST(FlatHashMapTest, AssignExistingUpdatesValueOnly) {
    FlatHashMap map;
    map.insert(5, 1);
    map.assign_existing(5, 2);
    ASSERT_NE(map.find(5), nullptr);
    EXPECT_EQ(*map.find(5), 2u);
}

TEST(FlatHashMapTest, GrowsCorrectlyUnderManyInsertsWithSequentialKeys) {
    FlatHashMap map;
    constexpr std::uint64_t kN = 200000; // forces several rehashes from the default capacity
    for (std::uint64_t i = 0; i < kN; ++i) {
        map.insert(i, static_cast<std::uint32_t>(i));
    }
    EXPECT_EQ(map.size(), kN);
    for (std::uint64_t i = 0; i < kN; i += 997) { // sparse spot-check, full scan would be slow
        ASSERT_NE(map.find(i), nullptr) << i;
        EXPECT_EQ(*map.find(i), i);
    }
    EXPECT_EQ(map.find(kN + 1), nullptr);
}

TEST(FlatHashMapTest, ReserveAvoidsNeedingLaterGrowthButStillWorks) {
    FlatHashMap map;
    map.reserve(10000);
    for (std::uint64_t i = 0; i < 5000; ++i) map.insert(i * 3 + 1, static_cast<std::uint32_t>(i));
    for (std::uint64_t i = 0; i < 5000; ++i) {
        ASSERT_NE(map.find(i * 3 + 1), nullptr);
        EXPECT_EQ(*map.find(i * 3 + 1), i);
    }
}

TEST(FlatHashMapTest, ReserveSmallerThanCurrentIsNoOp) {
    FlatHashMap map;
    for (std::uint64_t i = 0; i < 1000; ++i) map.insert(i, static_cast<std::uint32_t>(i));
    std::uint32_t* v = map.find(500);

    map.reserve(0);
    map.reserve(1);
    map.reserve(10);

    EXPECT_EQ(map.find(500), v) << "a smaller reserve must not rehash";
    EXPECT_EQ(map.size(), 1000u);
    for (std::uint64_t i = 0; i < 1000; ++i) {
        ASSERT_NE(map.find(i), nullptr);
        EXPECT_EQ(*map.find(i), i);
    }
}

TEST(FlatHashMapTest, FailedReserveLeavesMapUnchanged) {
    FlatHashMap map;
    for (std::uint64_t i = 0; i < 1000; ++i) map.insert(i, static_cast<std::uint32_t>(i));
    std::uint32_t* v = map.find(500);

    // 2^61 slots exceeds vector::max_size(), so this throws (length_error or bad_alloc).
    EXPECT_THROW(map.reserve(std::size_t{1} << 60), std::exception);

    EXPECT_EQ(map.find(500), v) << "a failed reserve must leave the slot array in place";
    EXPECT_EQ(map.size(), 1000u);
    for (std::uint64_t i = 0; i < 1000; ++i) {
        ASSERT_NE(map.find(i), nullptr);
        EXPECT_EQ(*map.find(i), i);
    }
    map.insert(5000, 5000);
    ASSERT_NE(map.find(5000), nullptr);
    EXPECT_EQ(*map.find(5000), 5000u);
}

TEST(FlatHashMapTest, HasRoomForTracksLoadLimit) {
    FlatHashMap map;
    map.reserve(100);
    EXPECT_TRUE(map.has_room_for(100)) << "reserve(n) must leave room for n entries";
    for (std::uint64_t i = 0; i < 100; ++i) map.insert_no_grow(i, static_cast<std::uint32_t>(i));
    EXPECT_TRUE(map.has_room_for(0));
    EXPECT_FALSE(map.has_room_for(std::size_t{1} << 20));
}

TEST(FlatHashMapTest, InsertNoGrowNeverRehashes) {
    FlatHashMap map;
    map.reserve(1000);
    map.insert_no_grow(0, 0);
    std::uint32_t* v = map.find(0);
    for (std::uint64_t i = 1; i < 1000; ++i) map.insert_no_grow(i * 7, static_cast<std::uint32_t>(i));
    EXPECT_EQ(map.find(0), v) << "insert_no_grow must not move the slot array";
    EXPECT_EQ(map.size(), 1000u);
    for (std::uint64_t i = 1; i < 1000; ++i) {
        ASSERT_NE(map.find(i * 7), nullptr);
        EXPECT_EQ(*map.find(i * 7), i);
    }
}

// --------------------------------------------------------------------- //
// OrderPool
// --------------------------------------------------------------------- //

// A real slot index is always < capacity <= kMaxOrderCapacity, so it can never
// be mistaken for the "no order" link sentinel.
static_assert(kMaxOrderCapacity < kInvalidSlot);

TEST(OrderPoolTest, DefaultPoolHasNoCapacity) {
    OrderPool pool;
    EXPECT_EQ(pool.capacity(), 0u);
    EXPECT_TRUE(pool.full());
    EXPECT_EQ(pool.try_allocate(), kInvalidSlot);
    EXPECT_EQ(pool.size(), 0u);
}

TEST(OrderPoolTest, AllocateReturnsStableIndicesAndData) {
    OrderPool pool(4);
    std::uint32_t a = pool.try_allocate();
    std::uint32_t b = pool.try_allocate();
    EXPECT_NE(a, b);
    pool[a].id = 111;
    pool[b].id = 222;
    EXPECT_EQ(pool[a].id, 111u);
    EXPECT_EQ(pool[b].id, 222u);
    EXPECT_EQ(pool.size(), 2u);
    EXPECT_EQ(pool.capacity(), 4u);
}

TEST(OrderPoolTest, TryAllocateReturnsInvalidSlotWhenFull) {
    OrderPool pool(2);
    std::uint32_t a = pool.try_allocate();
    std::uint32_t b = pool.try_allocate();
    pool[a].id = 123;
    pool[b].id = 456;
    EXPECT_TRUE(pool.full());

    EXPECT_EQ(pool.try_allocate(), kInvalidSlot);
    EXPECT_EQ(pool.try_allocate(), kInvalidSlot) << "a full pool stays full";

    EXPECT_EQ(pool.size(), 2u);
    EXPECT_EQ(pool[a].id, 123u) << "a failed allocation must not touch existing slots";
    EXPECT_EQ(pool[b].id, 456u);
}

TEST(OrderPoolTest, SpansSeveralChunksWithAPartialLastChunk) {
    // kChunkSize + 10 slots: one full chunk plus a 10-slot remainder chunk.
    constexpr std::uint32_t kCapacity = static_cast<std::uint32_t>(OrderPool::kChunkSize) + 10;
    OrderPool pool(kCapacity);
    std::uint32_t first = pool.try_allocate();
    pool[first].id = 999;
    OrderRecord* first_rec = &pool[first];
    for (std::uint32_t i = 1; i < kCapacity; ++i) {
        std::uint32_t idx = pool.try_allocate();
        ASSERT_EQ(idx, i);
        pool[idx].id = idx;
    }
    EXPECT_EQ(&pool[first], first_rec) << "slots never move";
    EXPECT_EQ(pool[first].id, 999u);
    EXPECT_EQ(pool.size(), kCapacity);
    EXPECT_EQ(pool[kCapacity - 1].id, kCapacity - 1);
    EXPECT_EQ(pool.try_allocate(), kInvalidSlot);
}
