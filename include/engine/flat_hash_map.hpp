#pragma once

#include <bit>
#include <cstdint>
#include <vector>
#include <cassert>

namespace xmatch::detail {

// Open-addressing, linear-probing map from a 64-bit key to a 32-bit value.
//
// Design notes (see DESIGN.md for the full rationale):
//  - Insert/find only; entries are never erased. The engine never needs to
//    forget an order_id (ids are unique for the lifetime of the instance),
//    so we don't pay for tombstone handling or backward-shift deletion.
//  - reserve() up front sizes the backing array so the benchmark's ~10M
//    operations don't trigger a rehash on the hot path; growth still works
//    (doubling + full rehash) if the caller undershoots the reserve.
//  - Keys are run through a 64-bit avalanche mix (splitmix64 finalizer)
//    before probing, since client order ids are frequently small sequential
//    integers that would otherwise cluster in the low buckets.
class FlatHashMap {
public:
    static constexpr std::uint32_t kEmpty = 0xFFFFFFFFu;

    FlatHashMap() { rehash(1u << 4); }

    void reserve(std::size_t n) {
        std::size_t needed = static_cast<std::size_t>(static_cast<double>(n) / kMaxLoadFactor) + 1;
        // std::bit_ceil (<bit>, C++20) is the smallest power of two >= needed,
        // replacing a shift-until-big-enough loop. Powers of two are not
        // incidental here: probing masks with capacity_ - 1, so a capacity
        // that is not a power of two would silently corrupt the index.
        std::size_t cap = std::bit_ceil(needed);
        if (cap > capacity_) rehash(cap);
    }

    // Returns pointer to slot value if key present, else nullptr.
    std::uint32_t* find(std::uint64_t key) {
        std::size_t idx = mix(key) & mask_;
        for (;;) {
            Slot& s = slots_[idx];
            if (!s.occupied) return nullptr;
            if (s.key == key) return &s.value;
            idx = (idx + 1) & mask_;
        }
    }

    const std::uint32_t* find(std::uint64_t key) const {
        return const_cast<FlatHashMap*>(this)->find(key);
    }

    bool contains(std::uint64_t key) const { return find(key) != nullptr; }

    // Updates the value for a key that is already present. Used by
    // replace() when new_order_id == order_id (in-place id reuse); the
    // find+assign happens in one call so no rehash can occur in between.
    void assign_existing(std::uint64_t key, std::uint32_t value) {
        std::uint32_t* v = find(key);
        assert(v && "assign_existing called on absent key");
        *v = value;
    }

    // Inserts key->value. Caller must ensure key is not already present
    // (the engine always checks contains() first for duplicate-id logic).
    void insert(std::uint64_t key, std::uint32_t value) {
        if (size_ + 1 > static_cast<std::size_t>(static_cast<double>(capacity_) * kMaxLoadFactor)) {
            rehash(capacity_ << 1);
        }
        std::size_t idx = mix(key) & mask_;
        for (;;) {
            Slot& s = slots_[idx];
            if (!s.occupied) {
                s.occupied = true;
                s.key = key;
                s.value = value;
                ++size_;
                return;
            }
            assert(s.key != key && "duplicate insert into FlatHashMap");
            idx = (idx + 1) & mask_;
        }
    }

    std::size_t size() const { return size_; }

private:
    struct Slot {
        std::uint64_t key = 0;
        std::uint32_t value = kEmpty;
        bool occupied = false;
    };

    static constexpr double kMaxLoadFactor = 0.7;

    static std::uint64_t mix(std::uint64_t x) {
        // splitmix64 finalizer
        x ^= x >> 30;
        x *= 0xbf58476d1ce4e5b9ULL;
        x ^= x >> 27;
        x *= 0x94d049bb133111ebULL;
        x ^= x >> 31;
        return x;
    }

    void rehash(std::size_t new_capacity) {
        // find()/insert() index with `& mask_`, which is only a valid modulo
        // when the capacity is a power of two. Every caller happens to pass
        // one today; assert it rather than trust that it stays true.
        assert(std::has_single_bit(new_capacity) && "FlatHashMap capacity must be a power of two");
        std::vector<Slot> old = std::move(slots_);
        capacity_ = new_capacity;
        mask_ = capacity_ - 1;
        slots_.assign(capacity_, Slot{});
        size_ = 0;
        for (Slot& s : old) {
            if (s.occupied) insert(s.key, s.value);
        }
    }

    std::vector<Slot> slots_;
    std::size_t capacity_ = 0;
    std::size_t mask_ = 0;
    std::size_t size_ = 0;
};

} // namespace xmatch::detail
