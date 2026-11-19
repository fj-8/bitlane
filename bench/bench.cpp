// bitlane benchmark harness.
// SPDX-License-Identifier: MIT OR Apache-2.0
//
//     cmake --build build --target bitlane_bench && ./build/bitlane_bench
//
// Compares the compressed bitmap against the two structures people reach for
// first: a flat `std::vector<bool>` bitset and a `std::set<std::uint32_t>`. Every
// number printed here is measured on the machine you are running it on, and the
// loser of each row is the honest one, not the preselected one.

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <iterator>
#include <random>
#include <set>
#include <string>
#include <vector>

#include <bitlane/bitlane.hpp>

namespace {

using clock_type = std::chrono::steady_clock;

double nanoseconds_since(clock_type::time_point start) noexcept {
    return std::chrono::duration<double, std::nano>(clock_type::now() - start).count();
}

/// Every timed body reports something; this keeps the optimiser from deleting it.
std::size_t sink = 0;

constexpr std::size_t kRepeats = 5;

/// Median of `kRepeats` runs of `body`, so a page fault cannot write a headline.
template <class F>
double median_ns(F&& body) {
    double runs[kRepeats] = {};
    for (std::size_t r = 0; r < kRepeats; ++r) {
        const auto t0 = clock_type::now();
        sink += body();
        runs[r] = nanoseconds_since(t0);
    }
    std::sort(runs, runs + kRepeats);
    return runs[kRepeats / 2];
}

struct workload {
    const char* label;
    std::uint32_t universe; // values are drawn from [0, universe)
    std::size_t members;
};

std::vector<std::uint32_t> sample(const workload& w, std::uint64_t seed) {
    std::mt19937_64 g(seed);
    std::uniform_int_distribution<std::uint32_t> pick(0, w.universe - 1);
    std::vector<std::uint32_t> out;
    out.reserve(w.members);
    for (std::size_t i = 0; i < w.members; ++i) {
        out.push_back(pick(g));
    }
    std::sort(out.begin(), out.end());
    out.erase(std::unique(out.begin(), out.end()), out.end());
    return out;
}

void rule(const char* name) {
    std::printf("\n%-28s %14s %14s %14s\n", name, "bitlane", "vector<bool>", "std::set");
    std::printf("%s\n", std::string(72, '-').c_str());
}

void row(const char* name, double a, double b, double c) {
    std::printf("%-28s %11.2f ns %11.2f ns %11.2f ns\n", name, a, b, c);
}

void bytes_row(const char* name, std::size_t a, std::size_t b, std::size_t c) {
    std::printf("%-28s %11.1f KiB %11.1f KiB %11.1f KiB\n",
                name, static_cast<double>(a) / 1024.0, static_cast<double>(b) / 1024.0,
                static_cast<double>(c) / 1024.0);
}

void run(const workload& w, std::uint64_t seed) {
    const auto values = sample(w, seed);
    const auto probes = sample({w.label, w.universe, 100000}, seed + 1);

    std::printf("\n=== %s: %zu members over a %u-value universe ===\n", w.label, values.size(),
                w.universe);

    // --- construction ---------------------------------------------------
    const double per_member = static_cast<double>(values.size());
    const double build_bm = median_ns([&] {
        bitlane::bitmap local;
        for (const std::uint32_t v : values) {
            local.insert(v);
        }
        return local.cardinality();
    }) / per_member;

    bitlane::bitmap bm;
    for (const std::uint32_t v : values) {
        bm.insert(v);
    }

    const double build_flat = median_ns([&] {
        std::vector<bool> local(w.universe, false);
        for (const std::uint32_t v : values) {
            local[v] = true;
        }
        return static_cast<std::size_t>(local[w.universe - 1]);
    }) / per_member;

    std::vector<bool> flat(w.universe, false);
    for (const std::uint32_t v : values) {
        flat[v] = true;
    }

    const double build_set = median_ns([&] {
        std::set<std::uint32_t> local;
        for (const std::uint32_t v : values) {
            local.insert(v);
        }
        return local.size();
    }) / per_member;

    std::set<std::uint32_t> set_;
    for (const std::uint32_t v : values) {
        set_.insert(v);
    }

    rule("insert per value");
    row("build", build_bm, build_flat, build_set);

    // --- membership -----------------------------------------------------
    const double probe_scale = 1.0 / static_cast<double>(probes.size());
    const auto count_bm = [&] {
        std::size_t hits = 0;
        for (const std::uint32_t v : probes) {
            hits += bm.contains(v) ? 1u : 0u;
        }
        return hits;
    };
    const auto count_flat = [&] {
        std::size_t hits = 0;
        for (const std::uint32_t v : probes) {
            hits += flat[v] ? 1u : 0u;
        }
        return hits;
    };
    const auto count_set = [&] {
        std::size_t hits = 0;
        for (const std::uint32_t v : probes) {
            hits += set_.count(v) ? 1u : 0u;
        }
        return hits;
    };

    const double probe_bm = median_ns(count_bm) * probe_scale;
    const double probe_flat = median_ns(count_flat) * probe_scale;
    const double probe_set = median_ns(count_set) * probe_scale;
    const std::size_t hits_bm = count_bm();
    const std::size_t hits_flat = count_flat();
    const std::size_t hits_set = count_set();

    rule("contains per probe");
    row("membership", probe_bm, probe_flat, probe_set);
    std::printf("   (agreement: %s)\n", (hits_bm == hits_flat && hits_flat == hits_set) ? "yes" : "NO");

    // --- set algebra ----------------------------------------------------
    const auto other_values = sample(w, seed + 7);
    bitlane::bitmap bm2;
    for (const std::uint32_t v : other_values) {
        bm2.insert(v);
    }
    std::set<std::uint32_t> set2;
    for (const std::uint32_t v : other_values) {
        set2.insert(v);
    }
    std::vector<bool> flat2(w.universe, false);
    for (const std::uint32_t v : other_values) {
        flat2[v] = true;
    }

    const auto united_bm = bm | bm2;
    const double union_bm = median_ns([&] { return (bm | bm2).cardinality(); });

    std::vector<bool> united_flat(w.universe, false);
    for (std::size_t i = 0; i < flat.size(); ++i) {
        united_flat[i] = flat[i] || flat2[i];
    }
    const double union_flat = median_ns([&] {
        std::vector<bool> local(w.universe, false);
        for (std::size_t i = 0; i < flat.size(); ++i) {
            local[i] = flat[i] || flat2[i];
        }
        return static_cast<std::size_t>(local[w.universe - 1]);
    });

    std::set<std::uint32_t> united_set;
    std::set_union(set_.begin(), set_.end(), set2.begin(), set2.end(),
                   std::inserter(united_set, united_set.end()));
    const double union_set = median_ns([&] {
        std::set<std::uint32_t> local;
        std::set_union(set_.begin(), set_.end(), set2.begin(), set2.end(),
                       std::inserter(local, local.end()));
        return local.size();
    });

    // The clamped divisor keeps the cost-per-member row defined if a workload
    // ever yields an empty result; the measured totals are unaffected.
    const double union_members =
        static_cast<double>(std::max<std::size_t>(united_bm.cardinality(), 1u));
    rule("union of two sets");
    row("total", union_bm, union_flat, union_set);
    row("per resulting member", union_bm / union_members, union_flat / union_members,
        union_set / union_members);

    const auto both_bm = bm & bm2;
    const double inter_bm = median_ns([&] { return (bm & bm2).cardinality(); });

    std::size_t inter_count_flat = 0;
    for (std::size_t i = 0; i < flat.size(); ++i) {
        inter_count_flat += (flat[i] && flat2[i]) ? 1u : 0u;
    }
    const double inter_flat = median_ns([&] {
        std::size_t hits = 0;
        for (std::size_t i = 0; i < flat.size(); ++i) {
            hits += (flat[i] && flat2[i]) ? 1u : 0u;
        }
        return hits;
    });

    std::set<std::uint32_t> both_set;
    std::set_intersection(set_.begin(), set_.end(), set2.begin(), set2.end(),
                          std::inserter(both_set, both_set.end()));
    const double inter_set = median_ns([&] {
        std::set<std::uint32_t> local;
        std::set_intersection(set_.begin(), set_.end(), set2.begin(), set2.end(),
                              std::inserter(local, local.end()));
        return local.size();
    });

    rule("intersection of two sets");
    row("total", inter_bm, inter_flat, inter_set);
    std::printf("   (sizes agree: %s)\n",
                (both_bm.cardinality() == both_set.size() &&
                 both_set.size() == inter_count_flat)
                    ? "yes"
                    : "NO");

    // --- memory ---------------------------------------------------------
    bytes_row("heap held", bm.memory_usage(),
              static_cast<std::size_t>(w.universe + 7) / 8 + sizeof(std::vector<bool>),
              set_.size() * 40u + set_.size() * 2 * 8u);
    std::printf("   (flat bitset covering the same members, i.e. up to max(): %.1f MiB)\n",
                static_cast<double>(bm.naive_bytes()) / (1024.0 * 1024.0));
    std::printf("   (memory rows: the bitlane column counts live heap bytes, the flat column is\n");
    std::printf("    (universe+7)/8 plus the vector object, and the std::set column is a 56-byte-per-node estimate,\n");
    std::printf("    not a measurement.)\n");
    std::printf("   (slots: %zu array + %zu bitmap, path: %s)\n", bm.array_slots(),
                bm.bitmap_slots(), bitlane::detail::intersection_path());
}

} // namespace

int main() {
    std::printf("bitlane %d.%d.%d benchmark\n", BITLANE_VERSION_MAJOR, BITLANE_VERSION_MINOR,
                BITLANE_VERSION_PATCH);
    std::printf("time rows are medians of %zu runs on this machine; lower is better.\n", kRepeats);
    std::printf("memory rows report what each structure holds, not what it took to build.\n");

    run({"posting list (sparse)", 100'000'000u, 1'000'000}, 11);
    run({"dense ids", 2'000'000u, 1'000'000}, 22);
    run({"tiny universe", 100'000u, 60'000}, 33);
    return 0;
}
