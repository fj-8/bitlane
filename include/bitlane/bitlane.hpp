// bitlane — compressed bitmap index for 32-bit integer sets.
// SPDX-License-Identifier: MIT OR Apache-2.0
//
// Header-only. Include this and nothing else:
//
//     #include <bitlane/bitlane.hpp>
//     bitlane::bitmap ids;
//     ids.insert_range(1000, 50000);
//
// Requirements: C++20. Developed and tested with MSVC 19.51 (/W4 /permissive-);
// every header is also compiled on its own, so none of them depends on include
// order. No exception is thrown by this library: invalid input to `decode`
// yields `std::nullopt`, and nothing else reports failure.

#ifndef BITLANE_HPP
#define BITLANE_HPP

#define BITLANE_VERSION_MAJOR 0
#define BITLANE_VERSION_MINOR 4
#define BITLANE_VERSION_PATCH 0

#include "bitmap.hpp"
#include "codec.hpp"
#include "container.hpp"
#include "simd.hpp"

namespace bitlane {

/// Library version as packed integers, for build-time assertions.
constexpr std::uint32_t version() noexcept {
    return BITLANE_VERSION_MAJOR * 10000 + BITLANE_VERSION_MINOR * 100 +
           BITLANE_VERSION_PATCH;
}

} // namespace bitlane

#endif // BITLANE_HPP
