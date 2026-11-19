// Tag filters over a synthetic catalogue, including paged retrieval via `select`.
// SPDX-License-Identifier: MIT OR Apache-2.0
//
//     ./build/example_tag_query
//
// Captured output (2026-11-18, MSVC 19.51, Release):
//   +-------------------------------------------------------------------------
//   | 2000000 products, 8 tags
//   | 
//   | tag             matches    array   bitmap        bytes
//   | sale            1190120        0       31       257400
//   | new               79048       31        0       222466
//   | clearance        493410        0       31       257400
//   | featured         237214        1       30       259998
//   | local            495076        0       31       257400
//   | ships-free       493970        0       31       257400
//   | returns-30d      495519        0       31       257400
//   | video             19821       31        0        47056
//   | 
//   | (sale AND new) AND NOT (video OR ships-free): 34921 matches
//   | page 1456 of 2911: 1000227, 1000288, 1000319, 1000345, 1000414, 1000461, 1000484, 1000670, 1000715, 1000723, 1000786, 1000947
//   | matches in the first quarter of the id space: 8728 of 34921 (25.0%)
//   | result ships in 70118 bytes (68.5 KiB) vs 244.1 KiB for a flat bitset



#include <cstdint>
#include <cstdio>
#include <random>
#include <string>
#include <unordered_map>
#include <vector>

#include <bitlane/bitlane.hpp>

namespace {

constexpr std::uint32_t kProducts = 2'000'000;
constexpr std::size_t kPageSize = 12;

const std::vector<std::string> kTags = {
    "sale", "new", "clearance", "featured", "local", "ships-free", "returns-30d", "video",
};

} // namespace

int main() {
    std::mt19937_64 g(20261118);
    std::uniform_int_distribution<std::uint32_t> coin(0, 100);

    std::unordered_map<std::string, bitlane::bitmap> by_tag;
    for (std::uint32_t id = 0; id < kProducts; ++id) {
        for (const std::string& tag : kTags) {
            // Skewed so some lists are dense (bitmap containers) and others sparse.
            const std::uint32_t chance = tag == "sale"      ? 60
                                         : tag == "new"     ? 4
                                         : tag == "video"   ? 1
                                         : tag == "featured"? 12
                                                             : 25;
            if (coin(g) < chance) {
                by_tag[tag].insert(id);
            }
        }
    }

    std::printf("%u products, %zu tags\n\n", kProducts, kTags.size());
    std::printf("%-12s %10s %8s %8s %12s\n", "tag", "matches", "array", "bitmap", "bytes");
    for (const std::string& tag : kTags) {
        const bitlane::bitmap& b = by_tag[tag];
        std::printf("%-12s %10zu %8zu %8zu %12zu\n", tag.c_str(), b.cardinality(), b.array_slots(),
                    b.bitmap_slots(), b.memory_usage());
    }

    const bitlane::bitmap sale = by_tag["sale"];
    const bitlane::bitmap fresh = by_tag["new"];
    const bitlane::bitmap cheap_video = by_tag["video"] | by_tag["ships-free"];
    const bitlane::bitmap wanted = (sale & fresh) - cheap_video;

    std::printf("\n(sale AND new) AND NOT (video OR ships-free): %zu matches\n",
                wanted.cardinality());

    // `select` gives O(matches-before-the-page) paging without materialising the
    // whole result: the page is a window over ids, not a copy of them.
    const std::size_t pages = (wanted.cardinality() + kPageSize - 1) / kPageSize;
    const std::size_t page = pages / 2;
    std::printf("page %zu of %zu: ", page + 1, pages);
    for (std::size_t k = page * kPageSize; k < (page + 1) * kPageSize && k < wanted.cardinality();
         ++k) {
        const auto id = wanted.select(k);
        if (!id.has_value()) {
            break;
        }
        std::printf("%s%u", k == page * kPageSize ? "" : ", ", *id);
    }
    std::printf("\n");

    // `count_below` is a rank query: how many matches are in the first quarter of
    // the catalogue.
    const std::size_t in_first_quarter = wanted.count_below(kProducts / 4);
    std::printf("matches in the first quarter of the id space: %zu of %zu (%.1f%%)\n",
                in_first_quarter, wanted.cardinality(),
                wanted.cardinality() == 0
                    ? 0.0
                    : 100.0 * static_cast<double>(in_first_quarter) /
                          static_cast<double>(wanted.cardinality()));

    const auto wire = wanted.encode();
    std::printf("result ships in %zu bytes (%.1f KiB) vs %.1f KiB for a flat bitset\n",
                wire.size(), static_cast<double>(wire.size()) / 1024.0,
                static_cast<double>(wanted.naive_bytes()) / 1024.0);
    return 0;
}
