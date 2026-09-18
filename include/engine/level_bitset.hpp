#pragma once

#include <cstdint>
#include <vector>

namespace xmatch::detail {

// Fixed-size occupancy bitmap over price-level indices, with word-skipping
// search for the nearest set bit. Used to relocate "best bid/ask" in O(1)
// amortized time when the cached best level empties out, instead of
// scanning the level array one price at a time.
class LevelBitset {
public:
    void resize(std::size_t n_bits) {
        words_.assign((n_bits + 63) / 64, 0ULL);
    }

    void set(std::uint32_t bit) { words_[bit >> 6] |= (1ULL << (bit & 63)); }
    void clear(std::uint32_t bit) { words_[bit >> 6] &= ~(1ULL << (bit & 63)); }
    bool test(std::uint32_t bit) const { return (words_[bit >> 6] >> (bit & 63)) & 1ULL; }

    // Highest set bit at index <= start; -1 if none.
    std::int64_t find_highest_le(std::int64_t start) const {
        if (start < 0) return -1;
        std::int64_t word_idx = start >> 6;
        int bit_idx = static_cast<int>(start & 63);
        std::uint64_t mask = (bit_idx == 63) ? ~0ULL : ((1ULL << (bit_idx + 1)) - 1);
        std::uint64_t w = words_[static_cast<std::size_t>(word_idx)] & mask;
        for (;;) {
            if (w) return word_idx * 64 + (63 - __builtin_clzll(w));
            --word_idx;
            if (word_idx < 0) return -1;
            w = words_[static_cast<std::size_t>(word_idx)];
        }
    }

    // Lowest set bit at index >= start; -1 if none.
    std::int64_t find_lowest_ge(std::int64_t start) const {
        std::int64_t n_words = static_cast<std::int64_t>(words_.size());
        if (start < 0) start = 0;
        std::int64_t word_idx = start >> 6;
        if (word_idx >= n_words) return -1;
        int bit_idx = static_cast<int>(start & 63);
        std::uint64_t mask = (bit_idx == 0) ? ~0ULL : (~0ULL << bit_idx);
        std::uint64_t w = words_[static_cast<std::size_t>(word_idx)] & mask;
        for (;;) {
            if (w) return word_idx * 64 + __builtin_ctzll(w);
            ++word_idx;
            if (word_idx >= n_words) return -1;
            w = words_[static_cast<std::size_t>(word_idx)];
        }
    }

private:
    std::vector<std::uint64_t> words_;
};

} // namespace xmatch::detail
