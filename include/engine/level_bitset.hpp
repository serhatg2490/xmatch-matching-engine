#pragma once

#include <bit>
#include <cstdint>
#include <vector>

namespace xmatch::detail {

// Fixed-size occupancy bitmap over price-level indices, with word-skipping
// search for the nearest set bit. Used to relocate "best bid/ask" in O(1)
// amortized time when the cached best level empties out, instead of
// scanning the level array one price at a time.
//
// The scans below use std::countl_zero / std::countr_zero (<bit>, C++20)
// rather than __builtin_clzll / __builtin_ctzll, so nothing here depends on
// a GCC/Clang extension. Note that the two are not interchangeable at zero:
// the builtins are undefined on a zero word, while the std functions are
// defined and return 64. Both loops therefore keep the `if (w)` guard
// *before* the call -- GCC proves w != 0 from it and emits the same bare
// bsr/bsf it emitted for the builtins (verified on GCC 13, -O3). Drop the
// guard and the compiler has to add a zero test, putting a branch back into
// best-price relocation.
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
            if (w) return word_idx * 64 + (63 - std::countl_zero(w));
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
            if (w) return word_idx * 64 + std::countr_zero(w);
            ++word_idx;
            if (word_idx >= n_words) return -1;
            w = words_[static_cast<std::size_t>(word_idx)];
        }
    }

private:
    std::vector<std::uint64_t> words_;
};

} // namespace xmatch::detail
