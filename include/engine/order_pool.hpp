#pragma once

#include <cstdint>
#include <memory>
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

// Chunked, append-only pool of OrderRecord. Growth allocates a whole new
// chunk (never moves/copies existing elements), so slot indices and
// references remain valid for the engine's lifetime and heap allocation
// only happens once per kChunkSize new orders -- effectively never during
// the timed hot path once the first chunk(s) are pre-reserved in
// configure().
class OrderPool {
public:
    static constexpr std::size_t kChunkSize = 1u << 20; // ~1,048,576 slots/chunk

    OrderPool() { chunks_.reserve(64); }

    void reserve(std::size_t n_slots) {
        std::size_t needed_chunks = (n_slots + kChunkSize - 1) / kChunkSize;
        while (chunks_.size() < needed_chunks) add_chunk();
    }

    std::uint32_t allocate() {
        std::size_t chunk_idx = size_ / kChunkSize;
        std::size_t offset = size_ % kChunkSize;
        if (chunk_idx >= chunks_.size()) add_chunk();
        std::uint32_t idx = static_cast<std::uint32_t>(size_);
        ++size_;
        chunks_[chunk_idx][offset] = OrderRecord{};
        return idx;
    }

    OrderRecord& operator[](std::uint32_t idx) {
        return chunks_[idx / kChunkSize][idx % kChunkSize];
    }
    const OrderRecord& operator[](std::uint32_t idx) const {
        return chunks_[idx / kChunkSize][idx % kChunkSize];
    }

    std::size_t size() const { return size_; }

private:
    void add_chunk() {
        chunks_.push_back(std::make_unique<OrderRecord[]>(kChunkSize));
    }

    std::vector<std::unique_ptr<OrderRecord[]>> chunks_;
    std::size_t size_ = 0;
};

} // namespace xmatch::detail
