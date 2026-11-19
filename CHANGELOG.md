# Changelog

All notable changes to `bitlane` are documented here. The format follows
[Keep a Changelog](https://keepachangelog.com/en/1.1.0/) and the project
adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [Unreleased]

### Added
- `every_single_byte_edit_of_a_frame_is_rejected` in `tests/bitmap_tests.cpp`:
  flip the high bit at every offset of an encoded frame and require `decode` to
  reject each one. `decode_rejects_malformed_input` samples chosen fields; this
  walks the whole buffer, which is the exhaustive form of the claim the docs
  make. The suite is now 32 cases (`ctest`, 1.1 s on MSVC 19.51).
- `testkit::describe`, so a failed `CHECK_EQ` prints both operands —
  `lhs: 43 / rhs: 42`, `5 elements: 7 8 9 10 ...` — instead of "values differ".

### Changed
- CI compiles the suite with `-Wall -Wextra -Werror` under GCC, Clang and Apple
  Clang, not only `/W4 /WX` under MSVC, and runs it once more under
  `-fsanitize=address,undefined` in a Debug build. The stricter legs found real
  diagnostics: `/W4` emits no conversion warnings at all, so `std::popcount`,
  `std::countr_zero` and `std::countl_zero` — all three of which answer in `int`
  — had been fed into unsigned counters and bit positions unnoticed.
  `container.hpp` funnels them through `popcount_of`, `lowest_bit_at` and
  `highest_bit_at` now, and the bench and example cast their printf arguments
  explicitly. No behaviour change: every value was already in range.

### Fixed
- `CHECK_EQ` bound its operands by reference. An operand such as `*b.min()`
  names a byte inside the `std::optional` temporary the call returned, and that
  temporary dies at the end of the initialising expression: lifetime extension
  does not reach a subobject obtained through a function call. The suite was
  reading destroyed storage. MSVC in both Debug and Release kept the stale bytes
  and every check passed. GCC in the CI Release build said so out loud —
  `warning: using a dangling pointer to an unnamed temporary
  [-Wdangling-pointer=]`, pointing at `tests/testkit.hpp` and at the unnamed
  temporary in each `CHECK_EQ(*b.select(...), ...)` — and then reported four
  wrong values (`tests/bitmap_tests.cpp:117`, `:176`, `:235`, `:246`). The
  library was not implicated: `cardinality`, `max` and `to_vector` in those same
  cases were correct, as was every other check in the suite. Operands are
  copies now.
- The header-isolation probe in CI wrote `#include <include/bitlane/x.hpp>`,
  which no translation unit compiled with `-Iinclude` can resolve; it includes
  `<bitlane/x.hpp>` now.
- The Windows CI legs named `Visual Studio 17 2022`, a generator the runner
  image does not necessarily ship, and asked a multi-config generator for a
  `CMAKE_BUILD_TYPE` it ignores. CMake now picks the installed generator and the
  configuration is selected with `--config`/`-C Release`.
- The CI example step looked for `example_*` with a wildcard that also matched
  the Visual Studio project files, then tried to run the XML it found. It now
  matches the binary by name or `.exe` and fails loudly when neither exists.

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
