// bitlane — compressed bitmap index for 32-bit integer sets.
// SPDX-License-Identifier: MIT OR Apache-2.0
//
// A container holds the low 16 bits of every member sharing one high 16-bit
// key. It is either a sorted array of uint16_t (cheap when sparse) or a
// 1024-word bitset (cheap when dense). The form flips at 4096 members, which is
// exactly where the two need the same 8 KiB.

#ifndef BITLANE_CONTAINER_HPP
#define BITLANE_CONTAINER_HPP

#include <algorithm>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <utility>
#include <vector>

#include "simd.hpp"

namespace bitlane {
namespace detail {

inline constexpr std::uint32_t kContainerSpan = 1u << 16;
inline constexpr std::size_t kWordsPerBitmap = 1u << 10;   // 65536 / 64
inline constexpr std::size_t kBytesPerBitmap = kWordsPerBitmap * 8u;
inline constexpr std::uint32_t kArrayCardinalityCap = 1u << 12;  // 4096 * 2B == 8 KiB

/// Bitmap words with bits [lo, hi] set, both ends inside one word.
inline std::uint64_t span_mask(std::uint32_t lo, std::uint32_t hi) noexcept {
    const std::uint64_t low = ~std::uint64_t{0} << lo;
    const std::uint64_t high = (hi == 63) ? ~std::uint64_t{0}
                                          : ((std::uint64_t{1} << (hi + 1)) - 1);
    return low & high;
}

class container {
  public:
    container() = default;

    /// Takes any order, any duplicates.
    explicit container(std::vector<std::uint16_t> array) {
        std::sort(array.begin(), array.end());
        array.erase(std::unique(array.begin(), array.end()), array.end());
        if (array.size() > kArrayCardinalityCap) {
            std::vector<std::uint64_t> bits(kWordsPerBitmap, 0);
            for (const std::uint16_t v : array) {
                bits[v >> 6] |= std::uint64_t{1} << (v & 63);
            }
            card_ = static_cast<std::uint32_t>(array.size());
            bits_ = std::move(bits);
        } else {
            card_ = static_cast<std::uint32_t>(array.size());
            arr_ = std::move(array);
        }
    }

    /// Wrap an array the caller has already proved ascending, duplicate-free and
    /// within the cardinality cap. The decoder validates before it builds, so it
    /// must not pay for a second sort.
    [[nodiscard]] static container from_sorted_array(std::vector<std::uint16_t> array) {
        container out;
        out.card_ = static_cast<std::uint32_t>(array.size());
        out.arr_ = std::move(array);
        return out;
    }

    [[nodiscard]] bool is_bitmap() const noexcept { return !bits_.empty(); }
    [[nodiscard]] std::uint32_t cardinality() const noexcept { return card_; }
    [[nodiscard]] bool empty() const noexcept { return card_ == 0; }

    [[nodiscard]] const std::vector<std::uint16_t>& array_view() const noexcept { return arr_; }
    [[nodiscard]] const std::uint64_t* words() const noexcept { return bits_.data(); }

    /// Heap bytes held by this container, for `bitmap::memory_usage()`.
    [[nodiscard]] std::size_t bytes() const noexcept {
        if (is_bitmap()) {
            return bits_.capacity() * sizeof(std::uint64_t) + arr_.capacity() * 2 + 3 * sizeof(void*);
        }
        return arr_.capacity() * 2 + 3 * sizeof(void*);
    }

    void clear() noexcept {
        arr_.clear();
        bits_.clear();
        card_ = 0;
    }

    /// True when the value was not already a member.
    bool add(std::uint16_t low) {
        if (is_bitmap()) {
            std::uint64_t& word = bits_[low >> 6];
            const std::uint64_t bit = std::uint64_t{1} << (low & 63);
            if ((word & bit) != 0) {
                return false;
            }
            word |= bit;
            ++card_;
            return true;
        }
        const auto at = std::lower_bound(arr_.begin(), arr_.end(), low);
        if (at != arr_.end() && *at == low) {
            return false;
        }
        arr_.insert(at, low);
        ++card_;
        if (card_ > kArrayCardinalityCap) {
            to_bitmap();
        }
        return true;
    }

    /// True when the value was a member and is now gone.
    bool remove(std::uint16_t low) {
        if (is_bitmap()) {
            std::uint64_t& word = bits_[low >> 6];
            const std::uint64_t bit = std::uint64_t{1} << (low & 63);
            if ((word & bit) == 0) {
                return false;
            }
            word &= ~bit;
            --card_;
            if (card_ <= kArrayCardinalityCap) {
                to_array();
            }
            return true;
        }
        const auto at = std::lower_bound(arr_.begin(), arr_.end(), low);
        if (at == arr_.end() || *at != low) {
            return false;
        }
        arr_.erase(at);
        --card_;
        return true;
    }

    [[nodiscard]] bool contains(std::uint16_t low) const noexcept {
        if (is_bitmap()) {
            return ((bits_[low >> 6] >> (low & 63)) & 1u) != 0;
        }
        const auto at = std::lower_bound(arr_.begin(), arr_.end(), low);
        return at != arr_.end() && *at == low;
    }

    /// Sets every bit in [first, last], both inclusive.
    void add_range(std::uint32_t first, std::uint32_t last) {
        if (last < first) {
            return;
        }
        // A single-word-scale span is cheaper as a bitmap fill than as thousands
        // of sorted inserts, which each move the tail of the array.
        if (last - first > kArrayCardinalityCap && !is_bitmap()) {
            to_bitmap();
        }
        if (is_bitmap()) {
            const std::size_t w0 = first >> 6;
            const std::size_t w1 = last >> 6;
            for (std::size_t w = w0; w <= w1 && w < bits_.size(); ++w) {
                const std::uint32_t lo = (w == w0) ? (first & 63) : 0u;
                const std::uint32_t hi = (w == w1) ? (last & 63) : 63u;
                bits_[w] |= span_mask(lo, hi);
            }
            card_ = popcount_words(bits_.data(), bits_.size());
            return;
        }
        for (std::uint32_t v = first; v <= last; ++v) {
            add(static_cast<std::uint16_t>(v));
        }
    }

    /// Invokes `f(low)` for every member in ascending order.
    template <class F> void for_each(F&& f) const {
        if (is_bitmap()) {
            for (std::size_t w = 0; w < bits_.size(); ++w) {
                std::uint64_t word = bits_[w];
                while (word != 0) {
                    f(static_cast<std::uint16_t>(w * 64 + std::countr_zero(word)));
                    word &= word - 1;
                }
            }
            return;
        }
        for (const std::uint16_t v : arr_) {
            f(v);
        }
    }

    [[nodiscard]] std::vector<std::uint16_t> values() const {
        std::vector<std::uint16_t> out;
        out.reserve(card_);
        for_each([&out](std::uint16_t v) { out.push_back(v); });
        return out;
    }

    /// Number of members strictly below `low`.
    [[nodiscard]] std::uint32_t count_below(std::uint16_t low) const noexcept {
        if (is_bitmap()) {
            const std::size_t stop = low >> 6;
            std::uint32_t total = 0;
            for (std::size_t w = 0; w < stop && w < bits_.size(); ++w) {
                total += std::popcount(bits_[w]);
            }
            if (stop < bits_.size()) {
                const std::uint32_t bit = low & 63;
                const std::uint64_t mask =
                    (bit == 0) ? 0u : span_mask(0, bit - 1);
                total += std::popcount(bits_[stop] & mask);
            }
            return total;
        }
        return static_cast<std::uint32_t>(
            std::lower_bound(arr_.begin(), arr_.end(), low) - arr_.begin());
    }

    /// The `rank`-th member, or `false` when `rank >= cardinality()`.
    [[nodiscard]] bool select(std::uint32_t rank, std::uint16_t& out) const {
        if (rank >= card_) {
            return false;
        }
        if (!is_bitmap()) {
            out = arr_[rank];
            return true;
        }
        std::uint32_t seen = 0;
        for (std::size_t w = 0; w < bits_.size(); ++w) {
            const std::uint32_t pc = std::popcount(bits_[w]);
            if (seen + pc > rank) {
                std::uint64_t word = bits_[w];
                for (std::uint32_t k = seen; k < rank; ++k) {
                    word &= word - 1;
                }
                out = static_cast<std::uint16_t>(w * 64 + std::countr_zero(word));
                return true;
            }
            seen += pc;
        }
        return false;
    }

    [[nodiscard]] std::uint16_t min() const noexcept {
        if (!is_bitmap()) {
            return arr_.front();
        }
        for (std::size_t w = 0; w < bits_.size(); ++w) {
            if (bits_[w] != 0) {
                return static_cast<std::uint16_t>(w * 64 + std::countr_zero(bits_[w]));
            }
        }
        return 0;
    }

    [[nodiscard]] std::uint16_t max() const noexcept {
        if (!is_bitmap()) {
            return arr_.back();
        }
        for (std::size_t w = bits_.size(); w-- > 0;) {
            if (bits_[w] != 0) {
                return static_cast<std::uint16_t>(w * 64 + 63 - std::countl_zero(bits_[w]));
            }
        }
        return 0;
    }

    void to_bitmap() {
        std::vector<std::uint64_t> bits(kWordsPerBitmap, 0);
        for (const std::uint16_t v : arr_) {
            bits[v >> 6] |= std::uint64_t{1} << (v & 63);
        }
        arr_.clear();
        arr_.shrink_to_fit();
        bits_ = std::move(bits);
    }

    void to_array() {
        std::vector<std::uint16_t> out;
        out.reserve(card_);
        for (std::size_t w = 0; w < bits_.size(); ++w) {
            std::uint64_t word = bits_[w];
            while (word != 0) {
                out.push_back(static_cast<std::uint16_t>(w * 64 + std::countr_zero(word)));
                word &= word - 1;
            }
        }
        card_ = static_cast<std::uint32_t>(out.size());
        bits_.clear();
        bits_.shrink_to_fit();
        arr_ = std::move(out);
    }

    /// Take ownership of a finished bitmap body; may compact to array form.
    void adopt_bits(std::vector<std::uint64_t> bits) {
        const std::uint32_t card = popcount_words(bits.data(), bits.size());
        arr_.clear();
        bits_ = std::move(bits);
        card_ = card;
        if (card_ <= kArrayCardinalityCap) {
            to_array();
        }
    }

    [[nodiscard]] container and_with(const container& other) const {
        if (is_bitmap() && other.is_bitmap()) {
            std::vector<std::uint64_t> out(kWordsPerBitmap);
            for (std::size_t w = 0; w < kWordsPerBitmap; ++w) {
                out[w] = bits_[w] & other.bits_[w];
            }
            return from_bits(std::move(out));
        }
        if (is_bitmap()) {
            return container(filter_by_bits(other.arr_));
        }
        if (other.is_bitmap()) {
            return container(other.filter_by_bits(arr_));
        }
        return container(intersect_arrays(arr_, other.arr_));
    }

    [[nodiscard]] container or_with(const container& other) const {
        if (is_bitmap() || other.is_bitmap()) {
            const std::vector<std::uint64_t>& base = is_bitmap() ? bits_ : other.bits_;
            std::vector<std::uint64_t> out(base);
            if (is_bitmap() && other.is_bitmap()) {
                for (std::size_t w = 0; w < kWordsPerBitmap; ++w) {
                    out[w] |= other.bits_[w];
                }
            } else {
                const std::vector<std::uint16_t>& src = is_bitmap() ? other.arr_ : arr_;
                for (const std::uint16_t v : src) {
                    out[v >> 6] |= std::uint64_t{1} << (v & 63);
                }
            }
            return from_bits(std::move(out));
        }
        return container(merge_arrays(arr_, other.arr_));
    }

    [[nodiscard]] container xor_with(const container& other) const {
        if (is_bitmap() && other.is_bitmap()) {
            std::vector<std::uint64_t> out(kWordsPerBitmap);
            for (std::size_t w = 0; w < kWordsPerBitmap; ++w) {
                out[w] = bits_[w] ^ other.bits_[w];
            }
            return from_bits(std::move(out));
        }
        if (is_bitmap()) {
            std::vector<std::uint64_t> out(bits_);
            for (const std::uint16_t v : other.arr_) {
                out[v >> 6] ^= std::uint64_t{1} << (v & 63);
            }
            return from_bits(std::move(out));
        }
        if (other.is_bitmap()) {
            std::vector<std::uint64_t> out(other.bits_);
            for (const std::uint16_t v : arr_) {
                out[v >> 6] ^= std::uint64_t{1} << (v & 63);
            }
            return from_bits(std::move(out));
        }
        return container(symmetric_difference_arrays(arr_, other.arr_));
    }

    /// Members of `*this` that are absent from `other`.
    [[nodiscard]] container and_not_with(const container& other) const {
        if (is_bitmap() && other.is_bitmap()) {
            std::vector<std::uint64_t> out(kWordsPerBitmap);
            for (std::size_t w = 0; w < kWordsPerBitmap; ++w) {
                out[w] = bits_[w] & ~other.bits_[w];
            }
            return from_bits(std::move(out));
        }
        if (is_bitmap()) {
            std::vector<std::uint64_t> out(bits_);
            for (const std::uint16_t v : other.arr_) {
                out[v >> 6] &= ~(std::uint64_t{1} << (v & 63));
            }
            return from_bits(std::move(out));
        }
        if (other.is_bitmap()) {
            std::vector<std::uint16_t> out;
            out.reserve(arr_.size());
            for (const std::uint16_t v : arr_) {
                if (((other.bits_[v >> 6] >> (v & 63)) & 1u) == 0) {
                    out.push_back(v);
                }
            }
            return container(std::move(out));
        }
        return container(difference_arrays(arr_, other.arr_));
    }

    [[nodiscard]] bool operator==(const container& other) const {
        return card_ == other.card_ && and_with(other).card_ == card_;
    }

  private:
    /// Members of `other` that are also set here; `other` must be ascending.
    [[nodiscard]] std::vector<std::uint16_t> filter_by_bits(
        const std::vector<std::uint16_t>& other) const {
        std::vector<std::uint16_t> out;
        out.reserve(other.size());
        for (const std::uint16_t v : other) {
            if (((bits_[v >> 6] >> (v & 63)) & 1u) != 0) {
                out.push_back(v);
            }
        }
        return out;
    }

    [[nodiscard]] static container from_bits(std::vector<std::uint64_t> bits) {
        container c;
        c.adopt_bits(std::move(bits));
        return c;
    }

    std::vector<std::uint16_t> arr_;
    std::vector<std::uint64_t> bits_;
    std::uint32_t card_ = 0;
};

static_assert(kBytesPerBitmap == 8192, "array and bitmap forms must meet at 8 KiB");

} // namespace detail

// Re-exported so `bitlane::container` and the tuning constants are nameable
// without reaching into `detail`.
using detail::container;
using detail::kArrayCardinalityCap;
using detail::kBytesPerBitmap;
using detail::kContainerSpan;
using detail::kWordsPerBitmap;

} // namespace bitlane

#endif // BITLANE_CONTAINER_HPP
