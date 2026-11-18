// bitlane — compressed bitmap index for 32-bit integer sets.
// SPDX-License-Identifier: MIT OR Apache-2.0
//
// Serialisation. The format is little-endian, self-describing and validated on
// the way in: `decode` is written to be safe on hostile bytes and returns
// `std::nullopt` rather than reading past the buffer or trusting a length field.

#ifndef BITLANE_CODEC_HPP
#define BITLANE_CODEC_HPP

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

#include "bitmap.hpp"

namespace bitlane {

/// `BLT1` header, slot records, then a trailing FNV-1a-64 over everything before it.
namespace wire {
inline constexpr std::array<char, 4> kMagic{{'B', 'L', 'T', '1'}};
inline constexpr std::uint8_t kVersion = 1;
inline constexpr std::size_t kHeaderBytes = 20;
inline constexpr std::size_t kChecksumBytes = 8;
inline constexpr std::uint8_t kKindArray = 0;
inline constexpr std::uint8_t kKindBitmap = 1;
} // namespace wire

namespace detail {

inline std::uint64_t fnv1a(std::uint64_t hash, std::span<const std::uint8_t> bytes) noexcept {
    for (const std::uint8_t b : bytes) {
        hash ^= b;
        hash *= 1099511628211ull;
    }
    return hash;
}

inline std::uint64_t fnv1a(std::uint64_t hash, const void* data, std::size_t n) noexcept {
    return fnv1a(hash, std::span<const std::uint8_t>(static_cast<const std::uint8_t*>(data), n));
}

class writer {
  public:
    void u8(std::uint8_t v) { out_.push_back(v); }

    void u16(std::uint16_t v) {
        out_.push_back(static_cast<std::uint8_t>(v & 0xFFu));
        out_.push_back(static_cast<std::uint8_t>(v >> 8));
    }

    void u32(std::uint32_t v) {
        for (int i = 0; i < 4; ++i) {
            out_.push_back(static_cast<std::uint8_t>((v >> (8 * i)) & 0xFFu));
        }
    }

    void u64(std::uint64_t v) {
        for (int i = 0; i < 8; ++i) {
            out_.push_back(static_cast<std::uint8_t>((v >> (8 * i)) & 0xFFu));
        }
    }

    void raw(const void* data, std::size_t n) {
        const auto* p = static_cast<const std::uint8_t*>(data);
        out_.insert(out_.end(), p, p + n);
    }

    void magic(std::span<const char, 4> m) {
        for (const char c : m) {
            out_.push_back(static_cast<std::uint8_t>(c));
        }
    }

    [[nodiscard]] std::vector<std::uint8_t> finish() {
        const std::uint64_t sum = fnv1a(1469598103934665603ull, out_.data(), out_.size());
        u64(sum);
        return std::move(out_);
    }

    [[nodiscard]] std::size_t size() const noexcept { return out_.size(); }

  private:
    std::vector<std::uint8_t> out_;
};

/// Bounds-checked cursor; every read either succeeds or poisons the reader.
class reader {
  public:
    reader(const std::uint8_t* data, std::size_t n) noexcept : p_(data), n_(n) {}

    [[nodiscard]] bool ok() const noexcept { return ok_; }
    [[nodiscard]] std::size_t remaining() const noexcept { return ok_ ? n_ - i_ : 0; }

    bool skip(std::size_t n) noexcept {
        if (!ok_ || n > remaining()) {
            ok_ = false;
            return false;
        }
        i_ += n;
        return true;
    }

    std::uint8_t u8() noexcept {
        if (!ok_ || remaining() < 1) {
            ok_ = false;
            return 0;
        }
        return p_[i_++];
    }

    std::uint16_t u16() noexcept {
        const std::uint32_t lo = u8();
        const std::uint32_t hi = u8();
        return static_cast<std::uint16_t>(lo | (hi << 8));
    }

    std::uint32_t u32() noexcept {
        std::uint32_t v = 0;
        for (int i = 0; i < 4; ++i) {
            v |= static_cast<std::uint32_t>(u8()) << (8 * i);
        }
        return v;
    }

    std::uint64_t u64() noexcept {
        std::uint64_t v = 0;
        for (int i = 0; i < 8; ++i) {
            v |= static_cast<std::uint64_t>(u8()) << (8 * i);
        }
        return v;
    }

    /// Copies `n` bytes out; the caller must not trust `out` when `ok()` is false.
    bool bytes(std::vector<std::uint8_t>& out, std::size_t n) {
        if (!ok_ || n > remaining()) {
            ok_ = false;
            return false;
        }
        out.assign(p_ + i_, p_ + i_ + n);
        i_ += n;
        return true;
    }

    [[nodiscard]] const std::uint8_t* here() const noexcept { return p_ + i_; }

  private:
    const std::uint8_t* p_ = nullptr;
    std::size_t n_ = 0;
    std::size_t i_ = 0;
    bool ok_ = true;
};

} // namespace detail

inline std::vector<std::uint8_t> bitmap::encode() const {
    detail::writer w;
    w.magic(wire::kMagic);
    w.u8(wire::kVersion);
    w.u8(0); // flags: reserved, never non-zero until a consumer needs one
    w.u16(0);
    w.u32(static_cast<std::uint32_t>(slots_.size()));
    w.u64(static_cast<std::uint64_t>(cardinality()));

    for (const slot& s : slots_) {
        w.u16(s.key);
        if (s.data.is_bitmap()) {
            w.u8(wire::kKindBitmap);
            w.u8(0);
            w.u32(s.data.cardinality());
            w.raw(s.data.words(), kWordsPerBitmap * sizeof(std::uint64_t));
        } else {
            w.u8(wire::kKindArray);
            w.u8(0);
            w.u32(s.data.cardinality());
            for (const std::uint16_t v : s.data.array_view()) {
                w.u16(v);
            }
        }
    }
    return w.finish();
}

inline std::optional<bitmap> bitmap::decode(std::span<const std::uint8_t> bytes) {
    if (bytes.size() < wire::kHeaderBytes + wire::kChecksumBytes) {
        return std::nullopt;
    }
    detail::reader r(bytes.data(), bytes.size());

    for (const char c : wire::kMagic) {
        if (r.u8() != static_cast<std::uint8_t>(c)) {
            return std::nullopt;
        }
    }
    if (r.u8() != wire::kVersion || r.u8() != 0 || r.u16() != 0) {
        return std::nullopt;
    }
    const std::uint32_t slot_count = r.u32();
    const std::uint64_t declared_card = r.u64();
    if (!r.ok()) {
        return std::nullopt;
    }

    // Reject absurd counts before they become an allocation. The smallest legal
    // record is 8 bytes of header plus one 2-byte value.
    if (r.remaining() < wire::kChecksumBytes ||
        slot_count > (r.remaining() - wire::kChecksumBytes) / 10u) {
        return std::nullopt;
    }

    bitmap out;
    std::vector<std::uint8_t> chunk;
    std::uint64_t total = 0;
    std::uint16_t previous_key = 0;
    bool first = true;

    for (std::uint32_t i = 0; i < slot_count; ++i) {
        const std::uint16_t key = r.u16();
        const std::uint8_t kind = r.u8();
        const std::uint8_t reserved = r.u8();
        const std::uint32_t card = r.u32();
        if (!r.ok() || reserved != 0) {
            return std::nullopt;
        }
        if (!first && key <= previous_key) {
            return std::nullopt; // keys must be strictly ascending
        }
        previous_key = key;
        first = false;

        if (kind == wire::kKindArray) {
            if (card == 0 || card > kArrayCardinalityCap) {
                return std::nullopt;
            }
            std::vector<std::uint16_t> values;
            values.reserve(card);
            std::uint16_t last_value = 0;
            for (std::uint32_t k = 0; k < card; ++k) {
                const std::uint16_t v = r.u16();
                if (!r.ok() || (k != 0 && v <= last_value)) {
                    return std::nullopt; // arrays are ascending and duplicate-free
                }
                last_value = v;
                values.push_back(v);
            }
            // Values are container-local; the declared key puts them back in place.
            out.slots_.push_back(slot{key, container::from_sorted_array(std::move(values))});
            total += card;
        } else if (kind == wire::kKindBitmap) {
            if (card <= kArrayCardinalityCap) {
                return std::nullopt; // would have been stored as an array
            }
            if (!r.bytes(chunk, kBytesPerBitmap)) {
                return std::nullopt;
            }
            std::vector<std::uint64_t> words(kWordsPerBitmap);
            for (std::size_t w = 0; w < kWordsPerBitmap; ++w) {
                std::uint64_t v = 0;
                for (int b = 0; b < 8; ++b) {
                    v |= static_cast<std::uint64_t>(chunk[w * 8 + static_cast<std::size_t>(b)])
                         << (8 * b);
                }
                words[w] = v;
            }
            if (detail::popcount_words(words.data(), words.size()) != card) {
                return std::nullopt; // cardinality must agree with the payload
            }
            out.slots_.push_back(slot{key, container{}});
            out.slots_.back().data.adopt_bits(std::move(words));
            total += card;
        } else {
            return std::nullopt;
        }
    }

    const std::uint64_t checksum = r.u64();
    if (!r.ok() || r.remaining() != 0 || total != declared_card) {
        return std::nullopt;
    }
    const std::size_t body = bytes.size() - wire::kChecksumBytes;
    if (detail::fnv1a(1469598103934665603ull, bytes.data(), body) != checksum) {
        return std::nullopt;
    }
    return out;
}

} // namespace bitlane

#endif // BITLANE_CODEC_HPP
