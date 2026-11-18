<!--
Short is fine. A failing test first, then the fix that makes it pass, is the ideal PR here.
-->

## What changes

## Why it is not expressible today

## Verification

- [ ] `ctest --test-dir build --output-on-failure` passes (including the new case)
- [ ] the build is warning-free with the project flags (`/W4` or `-Wall -Wextra -Wpedantic -Wshadow -Wconversion -Wsign-conversion`)
- [ ] each header still compiles on its own (`#include <bitlane/X.hpp>` alone in an empty TU)
- [ ] `./build/example_posting_index` and `./build/example_tag_query` still run
- [ ] if the codec changed: `cmake --install` then a `find_package(bitlane CONFIG)` consumer still builds and round-trips

## If this touches performance

Paste `./build/bitlane_bench` before and after, same machine, same build type,
and say which path was active (`sse2/4-lane` or `scalar/gallop`). The harness
also prints `(agreement: yes)` rows — if they ever say `NO`, that is the bug
report, not the numbers. No numbers, no merge.

## Notes

- Library code stays header-only with zero dependencies; a `.cpp` under
  `include/` or a third-party include will be closed rather than argued.
- Nothing in the library throws. If your change needs an error, return `bool`,
  `std::optional` or `std::nullopt`.
