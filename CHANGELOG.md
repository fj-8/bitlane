# Changelog

All notable changes to `bitlane` are documented here. The format follows
[Keep a Changelog](https://keepachangelog.com/en/1.1.0/) and the project
adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [Unreleased]

### Added
- Nothing staged.

## [0.4.0] - 2026-11-18

Initial public release.

### Added
- `bitlane::bitmap` over `std::uint32_t` sets: `insert` (single value, braced
  list, iterator range, `std::span`), `insert_range` (inclusive,
  container-crossing), `erase`,
  `contains`, `cardinality`, `empty`, `clear`, `min`, `max`, `count_below`,
  `select`, ascending iteration, `to_vector`.
- Set algebra `|`, `&`, `^`, `-` and the compound forms, merged over sorted
  container keys so disjoint key ranges cost nothing;
  `intersection_cardinality` and `jaccard_index` without materialising the
  result; set equality.
- `bitlane::detail::container` — the array/bitmap pair that flips at 4 096
  members, with `add_range`, `for_each`, `values`, `count_below`, `select`,
  `min`, `max` and the four pairwise operations dispatched by form.
- Sorted-run helpers in `simd.hpp`: a four-lane SSE2 merge for array/array
  intersection, a scalar galloping fallback, and `merge_arrays`,
  `difference_arrays`, `symmetric_difference_arrays`, `popcount_words`,
  `intersection_path`.
- Versioned wire format (`BLT1`) with an FNV-1a trailer in `encode()` /
  `bitmap::decode()`. `decode` is a validating reader: it bounds-checks before
  every byte, rejects malformed headers, non-ascending keys and values,
  container forms on the wrong side of the cardinality cap, popcount/cardinality
  disagreements, leftover or missing bytes and a bad checksum, and returns
  `std::nullopt` rather than throwing.
- `memory_usage()`, `naive_bytes()`, `slot_count`, `array_slots`, `bitmap_slots`,
  `fingerprint()`, `bitlane::version()`.
- CMake package: `bitlane::bitlane` interface target, install/export rules and a
  generated `bitlaneConfig.cmake` so `find_package(bitlane CONFIG)` works from an
  installed prefix (verified against an unrelated consumer project).
- 31-case test suite with STL differential checks, an adversarial decoder suite
  and representation-invariant tests; a benchmark harness comparing
  `std::vector<bool>` and `std::set<std::uint32_t>`; two examples (a boolean
  search index and a tag-filter catalogue with paged retrieval).

### Known limits
- 32-bit keys only; `count_below`, `select` and algebra are linear in the number
  of containers for the slot pass, so a set spread over thousands of keys pays
  for the binary search (measured: ~60 ns per membership probe over a 100 M-value
  universe).
- No runtime SIMD dispatch, no 64-bit domain, no `std::vector<bool>`-style
  proxy iterators — see the README section "What it deliberately is not".
