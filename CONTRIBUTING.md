# Contributing to bitlane

Thanks for taking the look. This is a small library with four unusual
constraints, so please read them before writing code.

## The four rules

1. **Header-only, zero dependencies.** Everything stays inside
   `include/bitlane/*.hpp`. No third-party include, no `.cpp` files in the
   library itself, no generated code. A patch that adds a dependency will be
   closed with a discussion, not merged: the whole point is that importing
   bitlane costs an include path and nothing else.
2. **No exceptions thrown by the library.** Failure is `bool`,
   `std::optional` or `std::nullopt`. `decode()` must stay total: any byte
   sequence, of any length, either decodes to a bitmap whose invariants hold or
   returns `std::nullopt`. If a change to the codec makes a hostile input
   reachable, add the mutation case to the test suite first.
3. **No `unsafe` shortcuts.** No reinterpret-cast aliasing of the container
   words, no platform-specific allocation, no `#pragma` that silences a warning
   instead of fixing it. The SSE2 path is allowed because SSE2 is baseline on
   x86-64 and `intersection_path()` reports which path you compiled.
4. **No fabricated numbers.** If a change touches performance, paste the output
   of `./build/bitlane_bench` before and after on the same machine, same build
   type, and say which path (`sse2/4-lane` or `scalar/gallop`) was active. Do
   not quote other libraries' benchmarks — ours reports `std::vector<bool>` and
   `std::set` rows because both are in the standard library and reproducible.

## Getting set up

```console
$ git clone https://github.com/fj-8/bitlane && cd bitlane
$ cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
$ cmake --build build
$ ctest --test-dir build --output-on-failure
```

Any C++20 compiler works. The development settings are MSVC `/W4 /permissive-
/utf-8` or `-Wall -Wextra -Wpedantic -Wshadow -Wconversion -Wsign-conversion`,
and the project-owned targets must stay warning-free; a warning suppressed with
a pragma is not "fixed".

On Windows, `scripts\build-msvc.bat` is the path of least resistance: it
finds Visual Studio with `vswhere` and uses the Visual Studio generator, so
nothing has to be on `PATH` and no `vcvars64` environment is needed. Pass
`install` or `bench` as the first argument for those extra steps.

## The shape of a good change

* **Bug fix** — one test that fails before the patch and passes after it. Say
  which invariant it protects; `representation_flips_at_the_documented_threshold`
  exists because a compaction bug is invisible in the public results.
* **New query or algebra** — the STL differential test is the contract. Add the
  randomised comparison in the same PR, otherwise the feature is unproven.
* **Wire format** — magic, version and reserved fields are frozen for `0.x`.
  A change that makes `decode()` reject bytes `0.4` would have accepted needs a
  version bump, a documented reason, and the `flags`/reserved rules updated in
  the README.
* **Documentation** — a claim about performance needs the measured row, a claim
  about behaviour needs the test name, and a captured console block must be the
  real output.

## What the test suite already covers

Before adding a case, check whether the invariant is already asserted: 31 cases
cover membership, boundaries of the 32-bit universe, form flipping at 4 096,
`insert_range` (including inverted spans and container-crossing spans),
iteration order, `count_below`/`select` agreement, the four algebra operations
against the STL over 200 random trials, De Morgan and identity laws, compound
assignment, `jaccard_index`, the SIMD and gallop helpers against
`std::set_intersection`/`std::lower_bound`, codec round trips and adversarial
decoding, and the memory comparison against a naive bitset.

## Release checklist (maintainers)

1. `ctest` green, `bitlane_bench` run twice, README tables refreshed from a real
   run with the machine line updated.
2. `cmake --install` into a scratch prefix, then build an unrelated consumer
   project against `find_package(bitlane 0.x CONFIG REQUIRED)`.
3. Version bumped in `CMakeLists.txt`, `BITLANE_VERSION_*` and `CHANGELOG.md`,
   and the `wire::kVersion` question re-asked and answered.
4. Tag `vX.Y.Z` and publish the release notes; the tag is what `FetchContent`
   examples in the README point at.
