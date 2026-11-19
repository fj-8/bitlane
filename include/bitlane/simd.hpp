// bitlane — compressed bitmap index for 32-bit integer sets.
// SPDX-License-Identifier: MIT OR Apache-2.0
//
// Sorted-run primitives. The array/array intersection has two paths: a
// four-lane-at-a-time merge for x86-64 (SSE2 is baseline there, so no compiler
// flags are needed) and a galloping scalar path everywhere else. The test suite
// cross-checks both against `std::set_intersection` on random inputs.

#ifndef BITLANE_SIMD_HPP
#define BITLANE_SIMD_HPP

#include <algorithm>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <iterator>
#include <vector>

#if defined(__SSE2__) || defined(_M_X64)
#define BITLANE_SSE2 1
#include <emmintrin.h>
#else
#define BITLANE_SSE2 0
#endif

namespace bitlane {
namespace detail {

/// First index at or after `from` whose element is >= `target`.
inline std::size_t gallop(const std::vector<std::uint16_t>& v, std::size_t from,
                          std::uint16_t target) {
    std::size_t step = 1;
    std::size_t lo = from;
    while (lo + step < v.size() && v[lo + step] < target) {
        lo += step;
        step *= 2;
    }
    const std::size_t hi = std::min(v.size(), lo + step + 1);
    const auto at = std::lower_bound(v.begin() + static_cast<std::ptrdiff_t>(lo),
                                     v.begin() + static_cast<std::ptrdiff_t>(hi), target);
    return static_cast<std::size_t>(at - v.begin());
}

/// Ascending, duplicate-free intersection of two ascending unique runs.
inline std::vector<std::uint16_t> intersect_arrays(const std::vector<std::uint16_t>& a,
                                                   const std::vector<std::uint16_t>& b) {
    std::vector<std::uint16_t> out;
    if (a.empty() || b.empty()) {
        return out;
    }
    out.reserve(std::min(a.size(), b.size()));

#if BITLANE_SSE2
    if (a.size() >= 4 && b.size() >= 4) {
        std::size_t i = 0;
        std::size_t j = 0;
        const std::size_t na = a.size() - 3;
        const std::size_t nb = b.size() - 3;
        while (i < na && j < nb) {
            // Four uint16 widened into four int32 lanes: values stay below 2^16,
            // so lane-wise equality is exact and needs no masking.
            const std::uint16_t* pa = a.data() + i;
            const std::uint16_t* pb = b.data() + j;
            const __m128i va =
                _mm_setr_epi32(pa[0], pa[1], pa[2], pa[3]);
            __m128i found = _mm_setzero_si128();
            for (int k = 0; k < 4; ++k) {
                const __m128i probe = _mm_set1_epi32(pb[k]);
                found = _mm_or_si128(found, _mm_cmpeq_epi32(va, probe));
            }
            const int mask = _mm_movemask_ps(_mm_castsi128_ps(found));
            if (mask != 0) {
                alignas(16) std::int32_t lanes[4];
                _mm_store_si128(reinterpret_cast<__m128i*>(lanes), va);
                for (int k = 0; k < 4; ++k) {
                    if ((mask & (1 << k)) != 0) {
                        out.push_back(static_cast<std::uint16_t>(lanes[k]));
                    }
                }
            }
            // A value no larger than max(pa[3], pb[3]) cannot match anything past
            // the two blocks in play, so retire the block with the smaller top.
            const std::uint16_t top_a = a[i + 3];
            const std::uint16_t top_b = b[j + 3];
            if (top_a <= top_b) {
                i += 4;
            }
            if (top_b <= top_a) {
                j += 4;
            }
        }
        // Invariant: nothing below a[i] or b[j] was discarded while still able to
        // match, so finishing the suffixes with a plain scan is complete.
        for (; i < a.size(); ++i) {
            const auto begin = b.begin() + static_cast<std::ptrdiff_t>(j);
            const auto at = std::lower_bound(begin, b.end(), a[i]);
            if (at != b.end() && *at == a[i]) {
                out.push_back(a[i]);
            }
        }
        return out;
    }
#endif

    std::size_t i = 0;
    std::size_t j = 0;
    while (i < a.size() && j < b.size()) {
        if (a[i] < b[j]) {
            i = gallop(a, i, b[j]);
        } else if (b[j] < a[i]) {
            j = gallop(b, j, a[i]);
        } else {
            out.push_back(a[i]);
            ++i;
            ++j;
        }
    }
    return out;
}

/// Ascending union of two ascending unique runs.
inline std::vector<std::uint16_t> merge_arrays(const std::vector<std::uint16_t>& a,
                                               const std::vector<std::uint16_t>& b) {
    std::vector<std::uint16_t> out;
    out.reserve(a.size() + b.size());
    std::set_union(a.begin(), a.end(), b.begin(), b.end(), std::back_inserter(out));
    return out;
}

/// Elements of `a` that are not in `b`.
inline std::vector<std::uint16_t> difference_arrays(const std::vector<std::uint16_t>& a,
                                                    const std::vector<std::uint16_t>& b) {
    std::vector<std::uint16_t> out;
    out.reserve(a.size());
    std::set_difference(a.begin(), a.end(), b.begin(), b.end(), std::back_inserter(out));
    return out;
}

/// Elements in exactly one of the two runs.
inline std::vector<std::uint16_t> symmetric_difference_arrays(
    const std::vector<std::uint16_t>& a, const std::vector<std::uint16_t>& b) {
    std::vector<std::uint16_t> out;
    out.reserve(a.size() + b.size());
    std::set_symmetric_difference(a.begin(), a.end(), b.begin(), b.end(),
                                  std::back_inserter(out));
    return out;
}

inline std::uint32_t popcount_words(const std::uint64_t* words, std::size_t n) noexcept {
    std::uint64_t total = 0;
    for (std::size_t i = 0; i < n; ++i) {
        total += static_cast<std::uint64_t>(std::popcount(words[i]));
    }
    return static_cast<std::uint32_t>(total);
}

/// Which intersection path this translation unit compiled to.
constexpr const char* intersection_path() noexcept {
#if BITLANE_SSE2
    return "sse2/4-lane";
#else
    return "scalar/gallop";
#endif
}

} // namespace detail
} // namespace bitlane

#endif // BITLANE_SIMD_HPP
