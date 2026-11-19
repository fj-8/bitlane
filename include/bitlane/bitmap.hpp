// bitlane — compressed bitmap index for 32-bit integer sets.
// SPDX-License-Identifier: MIT OR Apache-2.0
//
// Public container type: a sorted list of (high-16 key -> container) slots.
// Keeping the slots sorted is what lets the set operations be linear merges
// instead of hash lookups, and lets `contains` be one binary search.

#ifndef BITLANE_BITMAP_HPP
#define BITLANE_BITMAP_HPP

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <iterator>
#include <optional>
#include <span>
#include <utility>
#include <vector>

#include "container.hpp"

namespace bitlane {

/// A set of `std::uint32_t` that stays compact for sparse data and fast for dense.
class bitmap {
  public:
    using value_type = std::uint32_t;
    using size_type = std::size_t;

    bitmap() = default;

    bitmap(std::initializer_list<std::uint32_t> values) { insert(values.begin(), values.end()); }

    template <class It> bitmap(It first, It last) { insert(first, last); }

    // --- membership -------------------------------------------------------

    /// True when the value was not already present.
    bool insert(std::uint32_t value) {
        const std::uint16_t key = high(value);
        const std::size_t at = locate_or_prepare(key);
        return slots_[at].data.add(low(value));
    }

    /// Inserts every value in [first, last); no return value because it would lie.
    template <class It> void insert(It first, It last) {
        for (; first != last; ++first) {
            insert(static_cast<std::uint32_t>(*first));
        }
    }

    void insert(std::span<const std::uint32_t> values) { insert(values.begin(), values.end()); }

    /// `insert({1, 2, 3})`. A braced list prefers this overload over the `span`
    /// one, which cannot be deduced from a temporary.
    void insert(std::initializer_list<std::uint32_t> values) { insert(values.begin(), values.end()); }

    /// Adds every value in [lo, hi], inclusive. Runs are the common case in
    /// posting lists, and filling a bitmap container beats 65 536 inserts.
    void insert_range(std::uint32_t lo, std::uint32_t hi) {
        if (hi < lo) {
            return;
        }
        std::uint32_t cursor = lo;
        while (cursor <= hi) {
            const std::uint16_t key = high(cursor);
            const std::uint32_t slot_end =
                join(key, 0xFFFFu);  // the last value in container `key`
            const std::uint32_t stop = (hi < slot_end) ? hi : slot_end;
            const std::size_t at = locate_or_prepare(key);
            slots_[at].data.add_range(low(cursor), low(stop));
            if (stop == hi) {
                break;
            }
            cursor = stop + 1;
        }
    }

    /// True when the value was present and is now gone.
    bool erase(std::uint32_t value) {
        const std::size_t at = locate(high(value));
        if (at == slots_.size()) {
            return false;
        }
        const bool removed = slots_[at].data.remove(low(value));
        if (slots_[at].data.empty()) {
            slots_.erase(slots_.begin() + static_cast<std::ptrdiff_t>(at));
        }
        return removed;
    }

    [[nodiscard]] bool contains(std::uint32_t value) const noexcept {
        const std::size_t at = locate(high(value));
        return at != slots_.size() && slots_[at].data.contains(low(value));
    }

    [[nodiscard]] size_type cardinality() const noexcept {
        std::size_t total = 0;
        for (const slot& s : slots_) {
            total += s.data.cardinality();
        }
        return total;
    }

    [[nodiscard]] bool empty() const noexcept { return slots_.empty(); }

    void clear() noexcept { slots_.clear(); }

    [[nodiscard]] std::optional<std::uint32_t> min() const noexcept {
        if (slots_.empty()) {
            return std::nullopt;
        }
        return join(slots_.front().key, slots_.front().data.min());
    }

    [[nodiscard]] std::optional<std::uint32_t> max() const noexcept {
        if (slots_.empty()) {
            return std::nullopt;
        }
        return join(slots_.back().key, slots_.back().data.max());
    }

    /// Members below `value`. Linear in the number of slots.
    [[nodiscard]] size_type count_below(std::uint32_t value) const noexcept {
        std::size_t total = 0;
        for (const slot& s : slots_) {
            if (s.key < high(value)) {
                total += s.data.cardinality();
            } else if (s.key == high(value)) {
                total += s.data.count_below(low(value));
                break;
            } else {
                break;
            }
        }
        return total;
    }

    /// The `rank`-th member in ascending order.
    [[nodiscard]] std::optional<std::uint32_t> select(std::size_t rank) const {
        for (const slot& s : slots_) {
            if (rank < s.data.cardinality()) {
                std::uint16_t low_value = 0;
                if (!s.data.select(static_cast<std::uint32_t>(rank), low_value)) {
                    return std::nullopt;
                }
                return join(s.key, low_value);
            }
            rank -= s.data.cardinality();
        }
        return std::nullopt;
    }

    // --- iteration --------------------------------------------------------

    class iterator {
      public:
        using iterator_category = std::input_iterator_tag;
        using value_type = std::uint32_t;
        using difference_type = std::ptrdiff_t;
        using pointer = const std::uint32_t*;
        using reference = std::uint32_t;

        iterator() = default;

        [[nodiscard]] std::uint32_t operator*() const noexcept { return value_; }

        iterator& operator++() {
            step();
            return *this;
        }

        iterator operator++(int) {
            iterator copy = *this;
            step();
            return copy;
        }

        friend bool operator==(const iterator& a, const iterator& b) noexcept {
            return a.done_ == b.done_ && (a.done_ || (a.slot_ == b.slot_ && a.value_ == b.value_));
        }

        friend bool operator!=(const iterator& a, const iterator& b) noexcept { return !(a == b); }

      private:
        friend class bitmap;

        explicit iterator(const bitmap* owner) : owner_(owner) {
            if (owner_ == nullptr || owner_->slots_.empty()) {
                return; // end sentinel: `done_` is already true
            }
            load_slot(0);
            advance();
        }

        void load_slot(std::size_t index) {
            slot_ = index;
            const container& c = owner_->slots_[index].data;
            in_bitmap_ = c.is_bitmap();
            array_index_ = 0;
            word_index_ = 0;
            word_ = 0;
        }

        void advance() {
            for (;;) {
                if (in_bitmap_) {
                    while (word_ == 0) {
                        if (word_index_ >= kWordsPerBitmap) {
                            break;
                        }
                        word_ = owner_->slots_[slot_].data.words()[word_index_++];
                    }
                    if (word_ != 0) {
                        const std::uint32_t bit = static_cast<std::uint32_t>(
                            (word_index_ - 1) * 64 +
                            static_cast<std::size_t>(std::countr_zero(word_)));
                        word_ &= word_ - 1;
                        value_ = join(owner_->slots_[slot_].key, bit);
                        done_ = false;
                        return;
                    }
                } else {
                    const auto& arr = owner_->slots_[slot_].data.array_view();
                    if (array_index_ < arr.size()) {
                        value_ = join(owner_->slots_[slot_].key, arr[array_index_++]);
                        done_ = false;
                        return;
                    }
                }
                if (slot_ + 1 >= owner_->slots_.size()) {
                    done_ = true;
                    return;
                }
                load_slot(slot_ + 1);
            }
        }

        void step() {
            if (done_) {
                return;
            }
            advance();
        }

        const bitmap* owner_ = nullptr;
        std::size_t slot_ = 0;
        std::size_t array_index_ = 0;
        std::size_t word_index_ = 0;
        std::uint64_t word_ = 0;
        std::uint32_t value_ = 0;
        bool in_bitmap_ = false;
        bool done_ = true;
    };

    [[nodiscard]] iterator begin() const { return iterator(this); }
    [[nodiscard]] iterator end() const { return iterator(); }

    [[nodiscard]] std::vector<std::uint32_t> to_vector() const {
        std::vector<std::uint32_t> out;
        out.reserve(cardinality());
        for (std::uint32_t v : *this) {
            out.push_back(v);
        }
        return out;
    }

    // --- set algebra ------------------------------------------------------

    friend bitmap operator|(const bitmap& a, const bitmap& b) { return combine(a, b, op::kOr); }
    friend bitmap operator&(const bitmap& a, const bitmap& b) { return combine(a, b, op::kAnd); }
    friend bitmap operator^(const bitmap& a, const bitmap& b) { return combine(a, b, op::kXor); }
    friend bitmap operator-(const bitmap& a, const bitmap& b) { return combine(a, b, op::kAndNot); }

    bitmap& operator|=(const bitmap& other) { return *this = *this | other; }
    bitmap& operator&=(const bitmap& other) { return *this = *this & other; }
    bitmap& operator^=(const bitmap& other) { return *this = *this ^ other; }
    bitmap& operator-=(const bitmap& other) { return *this = *this - other; }

    [[nodiscard]] bitmap unite_with(const bitmap& other) const { return *this | other; }
    [[nodiscard]] bitmap intersect_with(const bitmap& other) const { return *this & other; }

    /// |A ∩ B| without materialising the intersection.
    [[nodiscard]] std::size_t intersection_cardinality(const bitmap& other) const noexcept {
        std::size_t total = 0;
        std::size_t i = 0;
        std::size_t j = 0;
        while (i < slots_.size() && j < other.slots_.size()) {
            if (slots_[i].key < other.slots_[j].key) {
                ++i;
            } else if (other.slots_[j].key < slots_[i].key) {
                ++j;
            } else {
                total += slots_[i].data.and_with(other.slots_[j].data).cardinality();
                ++i;
                ++j;
            }
        }
        return total;
    }

    /// |A ∩ B| / |A ∪ B|, in [0, 1]; both empty yields 1.0 by convention.
    [[nodiscard]] double jaccard_index(const bitmap& other) const {
        const std::size_t both = intersection_cardinality(other);
        const std::size_t mine = cardinality();
        const std::size_t theirs = other.cardinality();
        const std::size_t either = mine + theirs - both;
        if (either == 0) {
            return 1.0;
        }
        return static_cast<double>(both) / static_cast<double>(either);
    }

    [[nodiscard]] bool operator==(const bitmap& other) const noexcept {
        if (slots_.size() != other.slots_.size()) {
            return false;
        }
        for (std::size_t i = 0; i < slots_.size(); ++i) {
            if (slots_[i].key != other.slots_[i].key || !(slots_[i].data == other.slots_[i].data)) {
                return false;
            }
        }
        return true;
    }

    [[nodiscard]] bool operator!=(const bitmap& other) const noexcept { return !(*this == other); }

    // --- introspection ----------------------------------------------------

    [[nodiscard]] size_type slot_count() const noexcept { return slots_.size(); }

    [[nodiscard]] size_type bitmap_slots() const noexcept {
        return static_cast<size_type>(
            std::count_if(slots_.begin(), slots_.end(),
                          [](const slot& s) { return s.data.is_bitmap(); }));
    }

    [[nodiscard]] size_type array_slots() const noexcept {
        return slot_count() - bitmap_slots();
    }

    /// Bytes reachable from this object, including container payloads.
    [[nodiscard]] std::size_t memory_usage() const noexcept {
        std::size_t total = slots_.capacity() * sizeof(slot) + 2 * sizeof(void*);
        for (const slot& s : slots_) {
            total += s.data.bytes();
        }
        return total;
    }

    /// Roughly how many bytes an uncompressed bitset over the same universe needs.
    [[nodiscard]] std::size_t naive_bytes() const noexcept {
        if (slots_.empty()) {
            return 0;
        }
        const std::uint32_t top = *max();
        return (static_cast<std::size_t>(top) >> 3) + 1;
    }

    [[nodiscard]] std::uint64_t fingerprint() const noexcept {
        // FNV-1a over the encoded form: cheap identity for caches and tests.
        std::uint64_t hash = 1469598103934665603ull;
        for (const slot& s : slots_) {
            hash = (hash ^ s.key) * 1099511628211ull;
            hash = (hash ^ s.data.cardinality()) * 1099511628211ull;
        }
        return hash;
    }

    // --- serialisation ----------------------------------------------------

    [[nodiscard]] std::vector<std::uint8_t> encode() const;
    [[nodiscard]] static std::optional<bitmap> decode(std::span<const std::uint8_t> bytes);

  private:
    enum op : std::uint8_t { kAnd, kOr, kXor, kAndNot };

    /// One high-16 key and the members that share it.
    struct slot {
        std::uint16_t key = 0;
        container data;
    };

    static std::uint16_t high(std::uint32_t v) noexcept {
        return static_cast<std::uint16_t>(v >> 16);
    }

    static std::uint16_t low(std::uint32_t v) noexcept {
        return static_cast<std::uint16_t>(v & 0xFFFFu);
    }

    static std::uint32_t join(std::uint16_t key, std::uint32_t low_value) noexcept {
        return (static_cast<std::uint32_t>(key) << 16) | low_value;
    }

    /// Index of the slot with `key`, or `slots_.size()` when absent.
    [[nodiscard]] std::size_t locate(std::uint16_t key) const noexcept {
        const auto at = std::lower_bound(
            slots_.begin(), slots_.end(), key,
            [](const slot& s, std::uint16_t k) { return s.key < k; });
        if (at == slots_.end() || at->key != key) {
            return slots_.size();
        }
        return static_cast<std::size_t>(at - slots_.begin());
    }

    /// Index of the slot for `key`, inserting an empty one when needed.
    std::size_t locate_or_prepare(std::uint16_t key) {
        const auto at = std::lower_bound(
            slots_.begin(), slots_.end(), key,
            [](const slot& s, std::uint16_t k) { return s.key < k; });
        if (at != slots_.end() && at->key == key) {
            return static_cast<std::size_t>(at - slots_.begin());
        }
        const auto inserted = slots_.insert(at, slot{key, container{}});
        return static_cast<std::size_t>(inserted - slots_.begin());
    }

    static bitmap combine(const bitmap& a, const bitmap& b, op kind);

    std::vector<slot> slots_;
};

/// Merge two sorted slot lists, dispatching to the cheaper container pair.
inline bitmap bitmap::combine(const bitmap& a, const bitmap& b, op kind) {
    bitmap out;
    out.slots_.reserve(std::max(a.slots_.size(), b.slots_.size()));
    std::size_t i = 0;
    std::size_t j = 0;
    auto push = [&](std::uint16_t key, container c) {
        if (!c.empty()) {
            out.slots_.push_back(slot{key, std::move(c)});
        }
    };
    while (i < a.slots_.size() && j < b.slots_.size()) {
        const std::uint16_t ka = a.slots_[i].key;
        const std::uint16_t kb = b.slots_[j].key;
        if (ka < kb) {
            switch (kind) {
            case kOr:
            case kXor:
            case kAndNot: // nothing on the right to subtract
                push(ka, a.slots_[i].data);
                break;
            default:
                break; // AND with no partner slot produces nothing.
            }
            ++i;
        } else if (kb < ka) {
            switch (kind) {
            case kOr:
                push(kb, b.slots_[j].data);
                break;
            case kXor:
                push(kb, b.slots_[j].data);
                break;
            default:
                break;
            }
            ++j;
        } else {
            const container& ca = a.slots_[i].data;
            const container& cb = b.slots_[j].data;
            switch (kind) {
            case kAnd:
                push(ka, ca.and_with(cb));
                break;
            case kOr:
                push(ka, ca.or_with(cb));
                break;
            case kXor:
                push(ka, ca.xor_with(cb));
                break;
            case kAndNot:
                push(ka, ca.and_not_with(cb));
                break;
            }
            ++i;
            ++j;
        }
    }
    for (; i < a.slots_.size(); ++i) {
        if (kind == kOr || kind == kXor || kind == kAndNot) {
            push(a.slots_[i].key, a.slots_[i].data);
        }
    }
    for (; j < b.slots_.size(); ++j) {
        if (kind == kOr || kind == kXor) {
            push(b.slots_[j].key, b.slots_[j].data);
        }
    }
    return out;
}

} // namespace bitlane

#include "codec.hpp"

#endif // BITLANE_BITMAP_HPP
