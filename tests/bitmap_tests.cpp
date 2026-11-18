// bitlane test suite. Build and run:
// SPDX-License-Identifier: MIT OR Apache-2.0
//
//     cmake -S . -B build -G Ninja && cmake --build build && ./build/bitlane_tests
//
// Exit status is 0 only when every case passes.

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <random>
#include <set>
#include <string>
#include <vector>

#include <bitlane/bitlane.hpp>

#include "testkit.hpp"

using bitlane::bitmap;

namespace {

/// Ground truth for the algebra tests: sorted, unique, boring.
std::vector<std::uint32_t> sorted_unique(std::vector<std::uint32_t> v) {
    std::sort(v.begin(), v.end());
    v.erase(std::unique(v.begin(), v.end()), v.end());
    return v;
}

std::vector<std::uint32_t> reference_union(const std::vector<std::uint32_t>& a,
                                          const std::vector<std::uint32_t>& b) {
    std::vector<std::uint32_t> out;
    std::set_union(a.begin(), a.end(), b.begin(), b.end(), std::back_inserter(out));
    return out;
}

std::vector<std::uint32_t> reference_intersect(const std::vector<std::uint32_t>& a,
                                              const std::vector<std::uint32_t>& b) {
    std::vector<std::uint32_t> out;
    std::set_intersection(a.begin(), a.end(), b.begin(), b.end(), std::back_inserter(out));
    return out;
}

std::vector<std::uint32_t> reference_difference(const std::vector<std::uint32_t>& a,
                                               const std::vector<std::uint32_t>& b) {
    std::vector<std::uint32_t> out;
    std::set_difference(a.begin(), a.end(), b.begin(), b.end(), std::back_inserter(out));
    return out;
}

std::vector<std::uint32_t> reference_xor(const std::vector<std::uint32_t>& a,
                                        const std::vector<std::uint32_t>& b) {
    std::vector<std::uint32_t> out;
    std::set_symmetric_difference(a.begin(), a.end(), b.begin(), b.end(), std::back_inserter(out));
    return out;
}

bitmap of(std::vector<std::uint32_t> values) {
    std::sort(values.begin(), values.end());
    values.erase(std::unique(values.begin(), values.end()), values.end());
    return bitmap(values.begin(), values.end());
}

/// Random ascending unique uint16 run, sized to exercise the intersection paths.
std::vector<std::uint16_t> random_run(std::mt19937_64& g, std::size_t n, std::uint32_t span) {
    std::vector<std::uint16_t> out;
    out.reserve(n);
    std::uniform_int_distribution<std::uint32_t> pick(0, span - 1);
    while (out.size() < n) {
        const std::uint16_t v = static_cast<std::uint16_t>(pick(g));
        if (out.empty() || out.back() != v) {
            out.push_back(v);
        }
    }
    std::sort(out.begin(), out.end());
    out.erase(std::unique(out.begin(), out.end()), out.end());
    return out;
}

} // namespace

// --- membership ------------------------------------------------------------

TEST(empty_bitmap_is_empty) {
    bitmap b;
    CHECK(b.empty());
    CHECK_EQ(b.cardinality(), 0u);
    CHECK(!b.contains(0));
    CHECK(!b.contains(4294967295u));
    CHECK(!b.min().has_value());
    CHECK(!b.max().has_value());
    CHECK_EQ(b.slot_count(), 0u);
    CHECK(b.begin() == b.end());
}

TEST(insert_contains_erase) {
    bitmap b;
    CHECK(b.insert(7));
    CHECK(!b.insert(7));
    CHECK(b.contains(7));
    CHECK(b.erase(7));
    CHECK(!b.erase(7));
    CHECK(!b.contains(7));
    CHECK(b.empty());
}

TEST(boundary_values_of_the_universe) {
    bitmap b;
    b.insert(0);
    b.insert(1);
    b.insert(65535);
    b.insert(65536);
    b.insert(4294967294u);
    b.insert(4294967295u);
    CHECK_EQ(b.cardinality(), 6u);
    CHECK_EQ(*b.min(), 0u);
    CHECK_EQ(*b.max(), 4294967295u);
    CHECK_EQ(b.to_vector(), (std::vector<std::uint32_t>{0, 1, 65535, 65536, 4294967294u, 4294967295u}));
    CHECK(b.erase(4294967295u));
    CHECK_EQ(*b.max(), 4294967294u);
}

TEST(representation_flips_at_the_documented_threshold) {
    bitmap sparse;
    for (std::uint32_t i = 0; i < bitlane::kArrayCardinalityCap; ++i) {
        sparse.insert(i * 2); // stays inside container 0, all even values
    }
    CHECK_EQ(sparse.array_slots(), 1u);
    CHECK_EQ(sparse.bitmap_slots(), 0u);

    // One more distinct member pushes past 4096 and the container widens.
    sparse.insert(8193u); // odd, so it is new; still inside container 0
    CHECK_EQ(sparse.bitmap_slots(), 1u);
    CHECK_EQ(sparse.array_slots(), 0u);

    // 8 193 is the odd member that forced the widen; dropping to 4 096 compacts.
    CHECK(sparse.erase(8193u));
    CHECK_EQ(sparse.cardinality(), bitlane::kArrayCardinalityCap);
    CHECK_EQ(sparse.array_slots(), 1u);
    CHECK_EQ(sparse.bitmap_slots(), 0u);
    CHECK(sparse.erase(8190u));
    CHECK_EQ(sparse.array_slots(), 1u);
}

TEST(erasing_every_member_empties_the_slot_list) {
    bitmap b = of({1, 2, 3, 70000, 70001});
    for (std::uint32_t v : std::vector<std::uint32_t>{1, 2, 3, 70000, 70001}) {
        CHECK(b.erase(v));
    }
    CHECK(b.empty());
    CHECK_EQ(b.slot_count(), 0u);
}

// --- ranges --------------------------------------------------------------

TEST(insert_range_within_one_container) {
    bitmap b;
    b.insert_range(10, 20);
    CHECK_EQ(b.cardinality(), 11u);
    CHECK(b.contains(10));
    CHECK(b.contains(20));
    CHECK(!b.contains(21));
}

TEST(insert_range_spans_container_boundaries) {
    bitmap b;
    b.insert_range(65530, 131080);
    CHECK_EQ(b.cardinality(), 131080u - 65530u + 1u);
    CHECK(b.contains(65529) == false);
    CHECK(b.contains(65530));
    CHECK(b.contains(65535));
    CHECK(b.contains(65536));
    CHECK(b.contains(131080));
    CHECK(!b.contains(131081));
    CHECK_EQ(*b.min(), 65530u);
    CHECK_EQ(*b.max(), 131080u);
}

TEST(insert_range_of_a_full_container_is_bitmap_shaped) {
    bitmap b;
    b.insert_range(0, 65535);
    CHECK_EQ(b.cardinality(), 65536u);
    CHECK_EQ(b.bitmap_slots(), 1u);
    CHECK_EQ(b.slot_count(), 1u);
}

TEST(insert_range_rejects_an_inverted_span) {
    bitmap b;
    b.insert_range(100, 99);
    CHECK(b.empty());
}

TEST(insert_range_to_the_top_of_the_universe) {
    bitmap b;
    b.insert_range(4294967290u, 4294967295u);
    CHECK_EQ(b.cardinality(), 6u);
    CHECK(b.contains(4294967295u));
}

// --- ordering and queries ------------------------------------------------

TEST(iteration_is_ascending_and_complete) {
    std::mt19937_64 g(1234);
    std::uniform_int_distribution<std::uint32_t> pick(0, 300000);
    std::vector<std::uint32_t> values;
    for (int i = 0; i < 5000; ++i) {
        values.push_back(pick(g));
    }
    const auto want = sorted_unique(values);
    const bitmap b = of(values);
    CHECK_EQ(b.to_vector().size(), want.size());
    CHECK_EQ(b.to_vector(), want);
    CHECK_EQ(std::distance(b.begin(), b.end()), static_cast<std::ptrdiff_t>(want.size()));
}

TEST(count_below_matches_iteration) {
    bitmap b = of({0, 1, 5, 65535, 65536, 100000, 4294967295u});
    std::uint32_t seen = 0;
    for (std::uint32_t v : b) {
        CHECK_EQ(b.count_below(v), seen);
        ++seen;
    }
    CHECK_EQ(b.count_below(0), 0u);
    CHECK_EQ(b.count_below(1), 1u);
    CHECK_EQ(b.count_below(5), 2u);
    CHECK_EQ(b.count_below(4294967295u), 6u);
    CHECK_EQ(b.count_below(200000), 6u);
}

TEST(select_round_trips_with_count_below) {
    bitmap b = of({7, 8, 65540, 1000000, 4294967290u});
    std::uint32_t rank = 0;
    for (std::uint32_t v : b) {
        CHECK_EQ(*b.select(rank), v);
        ++rank;
    }
    CHECK(!b.select(rank).has_value());
    CHECK(!b.select(9999).has_value());
}

TEST(select_works_inside_a_bitmap_container) {
    bitmap b;
    b.insert_range(3 * 65536u, 3 * 65536u + 60000u);
    CHECK_EQ(b.bitmap_slots(), 1u);
    CHECK_EQ(*b.select(0), 3 * 65536u);
    CHECK_EQ(*b.select(1), 3 * 65536u + 1u);
    CHECK_EQ(*b.select(59999), 3 * 65536u + 59999u);
    CHECK(!b.select(60001).has_value());
}

// --- set algebra --------------------------------------------------------

TEST(algebra_against_stl_on_random_sets) {
    std::mt19937_64 g(20260718);
    for (int trial = 0; trial < 200; ++trial) {
        // Vary density so both container forms appear on both sides.
        const std::uint32_t span = (trial % 3 == 0) ? 2000u : 200000u;
        const std::size_t n = static_cast<std::size_t>(1 + (g() % 900));
        const std::size_t m = static_cast<std::size_t>(1 + (g() % 900));
        std::uniform_int_distribution<std::uint32_t> pick(0, span);
        std::vector<std::uint32_t> va, vb;
        for (std::size_t i = 0; i < n; ++i) {
            va.push_back(pick(g));
        }
        for (std::size_t i = 0; i < m; ++i) {
            vb.push_back(pick(g));
        }
        const auto a = sorted_unique(va);
        const auto b = sorted_unique(vb);
        const bitmap ba = of(a);
        const bitmap bb = of(b);

        CHECK_EQ((ba | bb).to_vector(), reference_union(a, b));
        CHECK_EQ((ba & bb).to_vector(), reference_intersect(a, b));
        CHECK_EQ((ba - bb).to_vector(), reference_difference(a, b));
        CHECK_EQ((ba ^ bb).to_vector(), reference_xor(a, b));
        CHECK_EQ(ba.intersection_cardinality(bb), reference_intersect(a, b).size());
    }
}

TEST(algebra_identities) {
    const bitmap a = of({1, 2, 3, 70000, 200000, 4294967295u});
    const bitmap b = of({3, 4, 70000, 4294967295u});
    const bitmap empty;

    CHECK((a | a) == a);
    CHECK((a & a) == a);
    CHECK((a ^ a) == empty);
    CHECK((a - a) == empty);
    CHECK((a | empty) == a);
    CHECK((a & empty) == empty);
    CHECK((a - empty) == a);
    CHECK((empty - a) == empty);
    CHECK((a | b) == (b | a));
    CHECK((a & b) == (b & a));
    CHECK((a ^ b) == (b ^ a));
    // De Morgan over the intersection, relative to the union of both sides.
    CHECK_EQ(((a | b) - (a & b)).to_vector(), (a ^ b).to_vector());
    CHECK_EQ((a - b).to_vector(), (a & (a ^ b)).to_vector());
}

TEST(compound_assignment_operators) {
    bitmap a = of({1, 2, 3, 70000});
    const bitmap b = of({3, 4, 70000});
    bitmap u = a;
    u |= b;
    CHECK(u == (a | b));
    a &= b;
    CHECK(a == (of({1, 2, 3, 70000}) & b));
    bitmap x = of({1, 2, 3});
    x ^= x;
    CHECK(x.empty());
    bitmap d = of({1, 2, 3});
    d -= of({2});
    CHECK_EQ(d.to_vector(), (std::vector<std::uint32_t>{1, 3}));
}

TEST(similarity_metrics) {
    const bitmap a = of({1, 2, 3, 4});
    const bitmap b = of({3, 4, 5, 6});
    CHECK_EQ(a.intersection_cardinality(b), 2u);
    CHECK(a.jaccard_index(b) > 0.33 && a.jaccard_index(b) < 0.34);
    CHECK_EQ(a.jaccard_index(a), 1.0);
    const bitmap empty;
    CHECK_EQ(empty.jaccard_index(empty), 1.0);
    CHECK_EQ(a.jaccard_index(empty), 0.0);
}

// --- low-level intersection paths ---------------------------------------

TEST(intersection_helpers_match_the_stl) {
    std::mt19937_64 g(777);
    for (int trial = 0; trial < 400; ++trial) {
        const std::size_t na = static_cast<std::size_t>(g() % 40);
        const std::size_t nb = static_cast<std::size_t>(g() % 40);
        const auto a = random_run(g, na, 65536);
        const auto b = random_run(g, nb, 65536);
        std::vector<std::uint16_t> want;
        std::set_intersection(a.begin(), a.end(), b.begin(), b.end(), std::back_inserter(want));
        const auto got = bitlane::detail::intersect_arrays(a, b);
        if (got != want) {
            char note[128];
            std::snprintf(note, sizeof note,
                          "trial %d: sizes %zu/%zu differ (got %zu, want %zu)", trial, a.size(),
                          b.size(), got.size(), want.size());
            ::testkit::fail(__FILE__, __LINE__, note);
        }
        CHECK(std::is_sorted(got.begin(), got.end()));
    }
}

TEST(gallop_finds_the_lower_bound) {
    std::vector<std::uint16_t> v;
    for (std::uint32_t i = 0; i < 6000; ++i) {
        v.push_back(static_cast<std::uint16_t>(i * 5u % 65536u));
    }
    std::sort(v.begin(), v.end());
    v.erase(std::unique(v.begin(), v.end()), v.end());
    for (std::uint32_t probe = 0; probe < 65536u; probe += 977u) {
        const std::uint16_t target = static_cast<std::uint16_t>(probe);
        const auto want = std::lower_bound(v.begin(), v.end(), target) - v.begin();
        CHECK_EQ(bitlane::detail::gallop(v, 0, target), static_cast<std::size_t>(want));
    }
}

// --- serialisation -------------------------------------------------------

TEST(encode_decode_round_trip) {
    std::mt19937_64 g(31337);
    for (int trial = 0; trial < 40; ++trial) {
        std::uniform_int_distribution<std::uint32_t> pick(0, 400000);
        std::vector<std::uint32_t> values;
        const std::size_t n = static_cast<std::size_t>(g() % 9000);
        for (std::size_t i = 0; i < n; ++i) {
            values.push_back(pick(g));
        }
        const bitmap b = of(values);
        const auto bytes = b.encode();
        CHECK(bytes.size() >= 28u);
        const auto back = bitmap::decode(bytes);
        CHECK(back.has_value());
        CHECK(*back == b);
        CHECK_EQ(back->to_vector(), b.to_vector());
        CHECK_EQ(back->fingerprint(), b.fingerprint());
    }
}

TEST(empty_bitmap_round_trips) {
    const bitmap b;
    const auto bytes = b.encode();
    const auto back = bitmap::decode(bytes);
    CHECK(back.has_value());
    CHECK(back->empty());
}

TEST(both_container_forms_survive_the_wire) {
    bitmap b;
    b.insert_range(0, 60000);                 // container 0: bitmap form
    for (std::uint32_t i = 0; i < 50; ++i) {  // container 1: array form
        b.insert(65536u + i * 997u);
    }
    CHECK_EQ(b.bitmap_slots(), 1u);
    CHECK_EQ(b.array_slots(), 1u);
    const auto back = bitmap::decode(b.encode());
    CHECK(back.has_value());
    CHECK(*back == b);
    CHECK_EQ(back->bitmap_slots(), 1u);
    CHECK_EQ(back->array_slots(), 1u);
}

TEST(decode_rejects_malformed_input) {
    const bitmap b = of({1, 2, 3, 70000, 70001});
    std::vector<std::uint8_t> good = b.encode();

    CHECK(!bitmap::decode({}).has_value());
    CHECK(!bitmap::decode(std::span<const std::uint8_t>{good.data(), good.size() - 1}).has_value());
    CHECK(!bitmap::decode(std::vector<std::uint8_t>{1, 2, 3}).has_value());

    auto corrupt = good;
    corrupt[0] = 'X'; // magic
    CHECK(!bitmap::decode(corrupt).has_value());

    corrupt = good;
    corrupt[4] = 9; // unknown version
    CHECK(!bitmap::decode(corrupt).has_value());

    corrupt = good;
    corrupt[6] = 1; // reserved flags must be zero
    CHECK(!bitmap::decode(corrupt).has_value());

    corrupt = good;
    corrupt[corrupt.size() - 1] ^= 0xFF; // checksum
    CHECK(!bitmap::decode(corrupt).has_value());

    corrupt = good;
    corrupt[20] ^= 0x01; // first key
    CHECK(!bitmap::decode(corrupt).has_value());

    corrupt = good;
    corrupt[8] = 0x7F; // absurd slot count
    corrupt[9] = 0xFF;
    CHECK(!bitmap::decode(corrupt).has_value());
}

TEST(decode_is_reproducible_on_trailing_garbage) {
    const bitmap b = of({5, 6, 7});
    std::vector<std::uint8_t> bytes = b.encode();
    bytes.push_back(0);
    bytes.push_back(0);
    CHECK(!bitmap::decode(bytes).has_value()); // reader must end exactly at the buffer
}

// --- shape and memory ----------------------------------------------------

TEST(sparse_set_beats_a_naive_bitset) {
    bitmap b;
    b.insert(0);
    b.insert(4294967295u);
    CHECK_EQ(b.naive_bytes(), 536870912u); // half a gigabyte for the same universe
    CHECK(b.memory_usage() < 1024u);
    CHECK(b.memory_usage() * 1000 < b.naive_bytes());
}

TEST(container_count_tracks_key_spread) {
    bitmap b;
    for (std::uint32_t i = 0; i < 100; ++i) {
        b.insert(i * 1000003u);
    }
    CHECK_EQ(b.slot_count(), b.to_vector().size()); // all distinct keys, one member each
    CHECK_EQ(b.bitmap_slots(), 0u);
}

TEST(copy_and_move_keep_the_contents) {
    bitmap a = of({1, 2, 70000, 4000000000u});
    const bitmap b = a;
    CHECK(b == a);
    CHECK_EQ(b.cardinality(), 4u);
    bitmap c = std::move(a);
    CHECK(c == b);
    CHECK_EQ(c.cardinality(), 4u);
    a = b; // copy-assign onto a moved-from object
    CHECK(a == c);
}

TEST(initializer_list_and_span_inserts) {
    bitmap a{1, 2, 3, 2, 1};
    CHECK_EQ(a.cardinality(), 3u);
    std::vector<std::uint32_t> more{50, 60, 70};
    a.insert(std::span<const std::uint32_t>{more});
    CHECK_EQ(a.cardinality(), 6u);
    a.insert(more.begin(), more.end());
    CHECK_EQ(a.cardinality(), 6u); // idempotent
    a.insert({80, 90, 90});        // a braced list needs the initializer_list overload
    CHECK_EQ(a.cardinality(), 8u);
    CHECK(a.contains(80));
    CHECK(a.contains(90));
    a.insert({70000, 140000});
    CHECK_EQ(a.slot_count(), 3u); // keys 0, 1 and 2: 70000 and 140000 open containers
}

TEST(fingerprint_is_order_independent_and_value_sensitive) {
    const bitmap a = of({1, 2, 3, 65536, 65537});
    bitmap b;
    b.insert(65537);
    b.insert(1);
    b.insert(65536);
    b.insert(3);
    b.insert(2);
    CHECK_EQ(a.fingerprint(), b.fingerprint());
    b.insert(4);
    CHECK(a.fingerprint() != b.fingerprint());
}

TEST(dense_universe_stays_queryable) {
    bitmap b;
    b.insert_range(0, 1000000);
    CHECK_EQ(b.cardinality(), 1000001u);
    CHECK(b.contains(999999));
    CHECK(!b.contains(1000001));
    CHECK_EQ(b.slot_count(), 16u); // ceil(1000001 / 65536)
    // The 16th container holds 16 961 members, still past the 4 096 threshold.
    CHECK_EQ(b.bitmap_slots(), 16u);
    CHECK_EQ(b.array_slots(), 0u);
    const bitmap holes = of({5, 65536, 131072, 999999});
    const bitmap punched = b - holes;
    CHECK_EQ(punched.cardinality(), 1000001u - 4u);
    CHECK(!punched.contains(5));
    CHECK(punched.contains(6));
}

int main() {
    std::printf("bitlane %u · intersection: %s\n", bitlane::version(),
                bitlane::detail::intersection_path());
    return testkit::run_all("bitmap tests");
}
