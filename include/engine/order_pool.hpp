#pragma once

#include <cstdint>
#include <memory>
#include <type_traits>
#include <utility>
#include <vector>

#include "xmatch/matching_engine_api.hpp"

namespace xmatch::detail {

inline constexpr std::uint32_t kInvalidSlot = 0xFFFFFFFFu;

// One resting/closed order's book-side state. A slot is permanent once
// created: order ids are unique for the life of the engine, so we never
// recycle a slot for a different id (see DESIGN.md). Replace() therefore
// allocates a fresh slot for new_order_id rather than renaming the old one.
struct OrderRecord {
    OrderId id = 0;
    Price price = 0;
    InstrumentId instrument_id = 0;
    Quantity open_qty = 0;   // 0 once fully filled / canceled / replaced away
    Quantity filled_qty = 0; // cumulative fills against this id
    std::uint32_t prev = kInvalidSlot; // intrusive FIFO links within its level
    std::uint32_t next = kInvalidSlot;
    std::uint32_t level_index = kInvalidSlot; // index into the book's level array
    Side side = Side::kBuy;
    bool is_open = false; // true iff currently resting on the book
};

// DESIGN.md and BENCHMARK.md both quote 48 bytes per order, with the 8-byte
// fields ordered first so the struct carries no interior padding: a 2^20-slot
// chunk is then ~48 MB and walking a level's FIFO touches few cache lines per
// order. That figure is load-bearing for the memory numbers those documents
// report, so pin it here -- reordering a field or widening one turns a silent
// regression into a build failure. (Linux x86-64 is the only supported target;
// see the requirements note in the public API header.)
static_assert(sizeof(OrderRecord) == 48,
              "OrderRecord must stay 48 bytes; see the memory layout section of DESIGN.md");
static_assert(alignof(OrderRecord) == 8, "OrderRecord should need no more than 8-byte alignment");
static_assert(std::is_nothrow_move_constructible_v<std::unique_ptr<OrderRecord[]>>,
              "std::unique_ptr<OrderRecord[]> must be nothrow move constructible");

// Every slot index is < capacity <= kMaxOrderCapacity, so a real slot can
// never collide with the kInvalidSlot sentinel used by the FIFO links.
static_assert(kMaxOrderCapacity < kInvalidSlot, "a real slot index must never equal kInvalidSlot");

// Fixed-capacity, chunked, append-only pool of OrderRecord. All memory is
// allocated by the constructor (called from configure()); try_allocate()
// never allocates and reports exhaustion by returning kInvalidSlot. Chunks
// never move, so slot indices and references stay valid for the engine's
// lifetime.
class OrderPool {
public:
    static constexpr std::size_t kChunkSize = 1u << 20; // ~1,048,576 slots/chunk

    OrderPool() noexcept = default; // capacity 0: every try_allocate() fails

    // Allocates room for `capacity` slots up front; may throw bad_alloc.
    // The last chunk holds only the remainder, so a small pool stays small.
    explicit OrderPool(std::uint32_t capacity) {
        std::size_t remaining = capacity;
        chunks_.reserve((remaining + kChunkSize - 1) / kChunkSize);
        while (remaining > 0) {
            std::size_t n = remaining < kChunkSize ? remaining : kChunkSize;
            chunks_.push_back(std::make_unique<OrderRecord[]>(n));
            remaining -= n;
        }
        capacity_ = capacity;
    }

    // Hands out the next slot, reset to a default OrderRecord, or
    // kInvalidSlot once the pool is full. Never allocates.
    std::uint32_t try_allocate() noexcept {
        if (size_ >= capacity_) return kInvalidSlot;
        std::uint32_t idx = size_++;
        (*this)[idx] = OrderRecord{};
        return idx;
    }

    OrderRecord& operator[](std::uint32_t idx) noexcept {
        return chunks_[idx / kChunkSize][idx % kChunkSize];
    }
    const OrderRecord& operator[](std::uint32_t idx) const noexcept {
        return chunks_[idx / kChunkSize][idx % kChunkSize];
    }

    std::uint32_t size() const noexcept { return size_; }
    std::uint32_t capacity() const noexcept { return capacity_; }
    bool full() const noexcept { return size_ >= capacity_; }

private:
    std::vector<std::unique_ptr<OrderRecord[]>> chunks_;
    std::uint32_t size_ = 0;
    std::uint32_t capacity_ = 0;
};

} // namespace xmatch::detail
