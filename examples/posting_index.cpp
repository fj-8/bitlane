// A miniature search index: term -> posting list, combined with boolean logic.
// SPDX-License-Identifier: MIT OR Apache-2.0
//
//     ./build/example_posting_index
//
// Captured output (2026-11-18, MSVC 19.51, Release):
//   +-------------------------------------------------------------------------
//   | indexed 8 documents, 37 distinct terms
//   | 
//   | "bitmap"                            2 hits  docs[1, 8]
//   | "bitmap" OR "index"                 3 hits  docs[1, 4, 8]
//   | "rust" AND "index"                  1 hits  docs[1]
//   | "rust" AND NOT "bitmap"             1 hits  docs[3]
//   | "simd" XOR "galloping"              1 hits  docs[4]
//   | 
//   | broad match: cardinality 4, 1 container(s), 114 bytes held
//   | shared with "rust": 1 document(s), jaccard 0.200
//   | encoded to 44 bytes, decoded successfully and equal: yes



#include <cctype>
#include <cstdint>
#include <cstdio>
#include <string>
#include <unordered_map>
#include <vector>

#include <bitlane/bitlane.hpp>

namespace {

struct document {
    std::uint32_t id;
    std::string text;
};

std::vector<std::string> tokens(const std::string& text) {
    std::vector<std::string> out;
    std::string current;
    for (const char c : text) {
        if (std::isalnum(static_cast<unsigned char>(c)) != 0) {
            current += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        } else if (!current.empty()) {
            out.push_back(current);
            current.clear();
        }
    }
    if (!current.empty()) {
        out.push_back(current);
    }
    return out;
}

} // namespace

int main() {
    const std::vector<document> corpus = {
        {1, "rust bitmap index for sparse sets"},
        {2, "compressed posting lists in c++"},
        {3, "rust argument parsing without dependencies"},
        {4, "inverted index internals and simd intersections"},
        {5, "java garbage collection notes"},
        {6, "simd galloping intersect on roaring bitmaps"},
        {7, "python streaming quantiles"},
        {8, "bitmap memory layouts compared: naive vs roaring"},
    };

    std::unordered_map<std::string, bitlane::bitmap> index;
    for (const document& doc : corpus) {
        for (const std::string& token : tokens(doc.text)) {
            index[token].insert(doc.id);
        }
    }

    std::printf("indexed %zu documents, %zu distinct terms\n\n", corpus.size(), index.size());

    auto show = [&](const char* label, const bitlane::bitmap& result) {
        std::printf("%-34s %2zu hits  docs[", label, result.cardinality());
        std::uint32_t shown = 0;
        for (const std::uint32_t id : result) {
            std::printf("%s%u", shown == 0 ? "" : ", ", id);
            ++shown;
        }
        std::printf("]\n");
    };

    show("\"bitmap\"", index["bitmap"]);
    show("\"bitmap\" OR \"index\"", index["bitmap"] | index["index"]);
    show("\"rust\" AND \"index\"", index["rust"] & index["index"]);
    show("\"rust\" AND NOT \"bitmap\"", index["rust"] - index["bitmap"]);
    show("\"simd\" XOR \"galloping\"", index["simd"] ^ index["galloping"]);

    // The interesting property: the query result is a bitmap, so it can be handed
    // to another stage as a filter without decompressing it into a vector first.
    const bitlane::bitmap broad = index["bitmap"] | index["index"] | index["simd"];
    std::printf("\nbroad match: cardinality %zu, %zu container(s), %zu bytes held\n",
                broad.cardinality(), broad.slot_count(), broad.memory_usage());
    std::printf("shared with \"rust\": %zu document(s), jaccard %.3f\n",
                broad.intersection_cardinality(index["rust"]), broad.jaccard_index(index["rust"]));

    const auto wire = broad.encode();
    const auto restored = bitlane::bitmap::decode(wire);
    std::printf("encoded to %zu bytes, decoded %s and equal: %s\n", wire.size(),
                restored.has_value() ? "successfully" : "FAILED",
                (restored.has_value() && *restored == broad) ? "yes" : "no");
    return 0;
}
