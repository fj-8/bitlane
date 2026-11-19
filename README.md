# bitlane

[![license: MIT OR Apache-2.0](https://img.shields.io/badge/license-MIT%20OR%20Apache--2.0-blue)](#license)
[![CI](https://github.com/fj-8/bitlane/actions/workflows/ci.yml/badge.svg)](https://github.com/fj-8/bitlane/actions/workflows/ci.yml)
[![C++20](https://img.shields.io/badge/C%2B%2B-20-informational)](#development)
[![dependencies: 0](https://img.shields.io/badge/dependencies-0-brightgreen)](#development)
[![header-only](https://img.shields.io/badge/header--only-5%20files-yellow)](include/bitlane)
[![tests: 32 passing](https://img.shields.io/badge/tests-32%20passing-success)](#correctness)
[![built with MSVC 19.51](https://img.shields.io/badge/built%20with-MSVC%2019.51-6C3AC4)](#development)

A header-only compressed bitmap index over sets of `std::uint32_t`: the data
structure a search engine uses for posting lists, a column store uses for
nullable-value scans, and an analytics layer uses for tag intersections —
reduced to five headers with no third-party dependency anywhere.

`bitlane` stores a set of integers as sorted 65 536-value **containers**, each
of which is either a compact array of `uint16_t` or an 8 KiB bitmap, whichever
is smaller for how many members that chunk actually holds. Set algebra runs
container-by-container, so `A & B` costs nothing at all for the 65 536-value
ranges where only one side has members.

It is not a port of Roaring Bitmap, but the container idea is the same one, and
the file format is documented and versioned here as well (see
[Wire format](#wire-format)) so a `bitlane::bitmap` survives a process boundary.

## Contents

- [Why this exists](#why-this-exists)
- [What it deliberately is not](#what-it-deliberately-is-not)
- [Install](#install)
- [Quick start](#quick-start)
- [How the representation works](#how-the-representation-works)
- [API at a glance](#api-at-a-glance)
- [Wire format](#wire-format)
- [Measurements](#measurements)
- [Correctness](#correctness)
- [Concurrency](#concurrency)
- [Development](#development)
- [License](#license)

## Why this exists

The two ways people usually reach for first are both bad at 10 million members
over a 4-billion-value key space:

* a flat bitset costs `universe / 8` bytes whether the set is full or nearly
  empty — 11.9 MiB to hold ~1 M sparse ids in the benchmark below;
* `std::set<std::uint32_t>` costs ~56 bytes of pointer-chasing per member and
  cannot be intersected without walking both trees node by node.

A container-form bitmap is ~2.2 MiB for that same sparse set and intersects in
`O(matched containers)` with SIMD-friendly inner loops. Concretely, from the
median of six runs recorded below: **12.1 ns per member to union two ~1 M-member
sparse sets against 259 ns for `std::set`**, and **3.9 ms for an intersection
that took `std::set` 47 ms**.

## What it deliberately is not

| Not here | Why |
| --- | --- |
| 64-bit keys | The container key is 16 bits and the slot list is a `std::vector`; a 64-bit domain means a two-level index and doubles the pointer chasing. Split the set into shards instead. |
| Exceptions | Nothing in the library throws. `decode()` reports a bad buffer with `std::nullopt`; every other failure mode is a `bool` or an `std::optional`. |
| Runtime SIMD dispatch | The wide path is selected at compile time (`#if defined(__SSE2__) \|\| defined(_M_X64)`) and `intersection_path()` tells you which one you got. `bm::Bit`-style runtime probing needs per-architecture `.cpp` files, which a header-only library cannot have. |
| `std::vector<bool>` replacement | For a dense universe with a small key space a flat bitset is faster and smaller; the [Measurements](#measurements) tables show both rows where bitlane loses. |
| Thread-safe mutation | Ordinary value semantics: share it behind your own lock. See [Concurrency](#concurrency). |

## Install

Five headers, nothing else. Three ways in, in order of how much you trust
your build system.

### 1. Install and `find_package` (verified)

```console
$ cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
$ cmake --build build
$ cmake --install build --prefix /opt/bitlane
```

```cmake
find_package(bitlane 0.4 CONFIG REQUIRED)
target_link_libraries(my_app PRIVATE bitlane::bitlane)
```

`bitlane::bitlane` is an `INTERFACE` target that only carries the include
directory and `cxx_std_20`. This exact path — install to a prefix, then build an
unrelated project against `find_package` — was exercised on the development machine before this release was tagged.

### 2. FetchContent

```cmake
include(FetchContent)
FetchContent_Declare(bitlane GIT_REPOSITORY https://github.com/fj-8/bitlane GIT_TAG v0.4.0)
FetchContent_MakeAvailable(bitlane)
target_link_libraries(my_app PRIVATE bitlane::bitlane)
```

The test, benchmark and example targets switch themselves off when the project
is not top level (`PROJECT_IS_TOP_LEVEL` guards each option), so a
`FetchContent` build compiles headers and nothing else.

### 3. Copy

Drop `include/bitlane/` next to your sources and add it to the include path.
`bitlane.hpp` is the only header you should include; the other four are pulled
in by it.

## Quick start

```cpp
#include <bitlane/bitlane.hpp>
#include <cstdio>
#include <vector>

int main() {
    bitlane::bitmap docs;
    docs.insert(1);
    docs.insert(4);
    docs.insert_range(70000, 78999);            // inclusive on both ends
    const std::vector<std::uint32_t> extra{200000, 200145};
    docs.insert(extra.begin(), extra.end());    // any order, duplicates allowed

    std::printf("card=%zu min=%u contains(70500)=%d\n",
                docs.cardinality(), *docs.min(), (int)docs.contains(70500));

    bitlane::bitmap other{1, 2, 3};
    const auto both = docs & other;             // also |, ^, - (difference)
    const auto rank = docs.select(1);           // 2nd member, ascending
    const auto nth  = docs.count_below(70500);  // members strictly below

    const std::vector<std::uint8_t> wire = docs.encode();
    const auto back = bitlane::bitmap::decode(wire);   // std::optional<bitmap>
    std::printf("both=%zu rank=%u below=%zu round-trip-equal=%d\n",
                both.cardinality(), *rank, nth,
                static_cast<int>(back.has_value() && *back == docs));
}
```

```console
$ cl /nologo /std:c++20 /EHsc /W4 /permissive- /utf-8 /Iinclude readme_snip.cpp && readme_snip.exe
card=9004 min=1 contains(70500)=1
both=1 rank=4 below=502 round-trip-equal=1
```

Iteration is a plain ascending walk over the members, and `to_vector()` is the
materialised form of the same thing:

```cpp
for (std::uint32_t id : docs) { /* ascending, no duplicates */ }
```

## How the representation works

A value is split into a **key** (high 16 bits) and a **low** value (low 16
bits). Each container owns one key, i.e. the 65 536 values that share it, and
the slot list keeps keys sorted so a lookup is one binary search plus one
cheap probe.

| Form | Layout | Used when | Size |
| --- | --- | --- | --- |
| array | sorted `std::vector<std::uint16_t>` | ≤ 4 096 members in the chunk | ~2 B per member |
| bitmap | `std::vector<std::uint64_t>`, 1 024 words | > 4 096 members in the chunk | 8 KiB, fixed |

A container flips form the moment an `insert` or `erase` crosses 4 096, so the
representation tracks the data instead of being chosen once at construction.
`add_range` / `insert_range` fill whole runs without one probe per value, and
`bitmap_slots()` / `array_slots()` let you assert which form you ended up with
(the examples do exactly that).

Two containers meet through whichever of these is cheapest:

* array ∩ array — a four-lane-at-a-time SSE2 merge that compares `uint32`
  values against each other's frontier and retires the block with the smaller
  top; on non-x86 it is the same merge written scalar with **galloping**
  (exponential probe, then a bounded `lower_bound`) so a skewed pair of runs
  still costs `O(n log m)` rather than `O(n·m)`;
* array ∩ bitmap — the array's values are read out of the bitmap's words
  directly, which is why the intersection of a 4 000-member array with a full
  bitmap never materialises the bitmap's 65 536 members;
* bitmap ∩ bitmap — word-wise `&` plus `popcount`.

`combine` merges two sorted slot lists in one pass, so a set operation over
disjoint key ranges touches only the containers that overlap.

## API at a glance

| | |
| --- | --- |
| `insert(v)` | add one; returns `true` when it was not already present |
| `insert({a, b, c})` / `insert(first, last)` / `insert(std::span<const uint32_t>)` | bulk add, any order, duplicates allowed |
| `insert_range(lo, hi)` | every value in `[lo, hi]`, inclusive, crossing container boundaries |
| `erase(v)` | returns `true` when the value was present |
| `contains(v) const` | membership |
| `cardinality()`, `empty()`, `min()`, `max()` | `min`/`max` return `std::optional` |
| `count_below(v)` | members strictly below `v` |
| `select(rank)` | `rank`-th member, `std::optional` |
| `begin()` / `end()`, `to_vector()` | ascending members |
| `a \| b`, `a & b`, `a ^ b`, `a - b` | union, intersection, symmetric difference, difference; `+=`-style compounds exist |
| `intersection_cardinality(b)` | size of `a & b` without building it |
| `jaccard_index(b)` | `|a ∩ b| / |a ∪ b|` as `double` |
| `a == b`, `a != b` | set equality |
| `encode()` / `bitmap::decode(bytes)` | versioned wire form, see below |
| `memory_usage()`, `naive_bytes()` | live heap bytes vs the flat-bitset size for the same range |
| `slot_count()`, `array_slots()`, `bitmap_slots()` | representation introspection |
| `fingerprint()` | order-independent 64-bit hash over the slots |
| `bitlane::version()` | packed `major*10000 + minor*100 + patch` |

Everything in the class is `[[nodiscard]]` where a discarded result would mean
a bug, and the library never throws.

## Wire format

`encode()` produces a self-describing buffer; `decode()` accepts nothing it
cannot prove correct.

```
offset  size  field
0       4     magic "BLT1"
4       1     version (1)
5       1     flags (must be 0)
6       2     reserved (must be 0)
8       4     slot count
12      8     total cardinality
20      ...   slot records
...     8     FNV-1a 64 over every preceding byte
```

Each slot record is `key: uint16`, `kind: uint8` (0 = array, 1 = bitmap),
`reserved: uint8` (must be 0), `cardinality: uint32`, then either
`cardinality × uint16` ascending values or 8 192 bytes of little-endian words.

`decode()` returns `std::nullopt` on: a short buffer; wrong magic or version;
non-zero flags/reserved; a slot count large enough to imply more bytes than
exist; non-ascending slot keys; an array whose cardinality is `0` or `> 4096`;
non-ascending array values; a bitmap whose cardinality is `≤ 4096`; a bitmap
whose `popcount` disagrees with its declared cardinality; a total that
disagrees with the header's declared cardinality; leftover or missing bytes; a
checksum mismatch. There is a test that mutates a valid buffer one field at a
time and asserts each mutation is caught.

## Measurements

Recorded on the machine this library was developed on, from
`./build/bitlane_bench` (Release, `/O2`, `LTCG` off, SSE2 path active). Each cell is
the median across six invocations of the benchmark; every invocation prints an
internal median of five timed iterations, so these are medians of medians:

> Windows 11 x64 · Intel Xeon E5-2666 v3 @ 2.90 GHz · 16 GB · MSVC 19.51

**995 104 members drawn over a 100 000 000-value universe** (a posting list):

| | bitlane | `vector<bool>` | `std::set<uint32_t>` |
| --- | --- | --- | --- |
| build, per value | 30.8 ns | 5.9 ns | 222.5 ns |
| `contains`, per probe | 59.8 ns | 7.4 ns | 192.6 ns |
| union of two sets | 24.0 ms (12.1 ns/member) | 279.5 ms (141.1 ns/member) | 512.3 ms (258.7 ns/member) |
| intersection | 3.89 ms | 109.8 ms | 47.1 ms |
| bytes held | 2 268 KiB | 12 207 KiB | 54 420 KiB (estimated) |

**786 969 members over a 2 000 000-value universe** (nearly dense):

| | bitlane | `vector<bool>` | `std::set<uint32_t>` |
| --- | --- | --- | --- |
| build, per value | 12.4 ns | 1.3 ns | 220.1 ns |
| `contains`, per probe | 11.0 ns | 1.0 ns | 168.8 ns |
| union of two sets | 63.1 µs | 15.4 ms | 319.5 ms |
| intersection | 63.9 µs | 10.3 ms | 99.9 ms |
| bytes held | 251.4 KiB | 244.2 KiB | 43 037 KiB (estimated) |

**44 969 members over a 100 000-value universe** (small keyspace):

| | bitlane | `vector<bool>` | `std::set<uint32_t>` |
| --- | --- | --- | --- |
| build, per value | 9.6 ns | 1.3 ns | 167.1 ns |
| `contains`, per probe | 7.9 ns | 0.9 ns | 58.0 ns |
| union of two sets | 3.9 µs | 830.6 µs | 14.1 ms |
| intersection | 3.5 µs | 558.6 µs | 5.08 ms |
| bytes held | 16.2 KiB | 12.2 KiB | 2 459 KiB (estimated) |

Read these honestly:

* **bitlane wins on set algebra and on sparse memory, and loses on random
  membership.** A flat bitset answers `contains` with one bit test; the closest
  thing here is a binary search over the slot list, so single-value probing is
  8-60 ns depending on how many containers the set spans. If your hot loop is
  "ask about one id at a time" over a small universe, use a bitset.
* **bitlane does not win on dense memory.** 251.4 KiB vs 244.2 KiB in the second
  table is the honest result for a universe that is ~40 % full; compression
  earns its keep as density falls, and the first table (2 268 KiB vs 12 207 KiB)
  is where it does.
* `std::set` memory is **an estimate of 56 bytes per node, not a measurement** —
  the harness cannot see the standard library's allocator. The time rows are all
  measured in the same process.
* Run-to-run spread is real and worst on the short rows. Over the six runs the
  bitlane millisecond rows moved ≤31 % (sparse intersection spanned 3.37-4.40 ms),
  while the microsecond rows moved up to 1.6× (dense `contains` spanned
  10.6-17.6 ns, tiny-universe build 9.3-14.3 ns). The competitors are noisier:
  the flat-bitset membership row on the sparse workload spanned 3.03-9.38 ns and
  `std::set`'s dense build row spanned 215-424 ns. **Differences under a factor
  of ~1.5 are not findings here**; the headline gaps — 24 ms against 280 and
  512 ms, 2.2 MiB against 12 MiB — are far outside that band.
* No comparison against other bitmap libraries is published here, because none
  was installed on this machine to compare against. If you want that row, run it
  yourself and open an issue with the numbers.

## Correctness

32 cases in `tests/bitmap_tests.cpp`, run by `ctest`:

* algebra against the STL: 200 random trials where `|`, `&`, `^`, `-` are each
  checked against `std::set_union` / `set_intersection` /
  `set_symmetric_difference` / `set_difference`, then a fixed pair walked
  through the identity and commutativity laws (`A|A = A`, `A&A = A`,
  `A^A = ∅`, `A-∅ = A`, `∅-A = ∅`, commutativity of `|`, `&`, `^`), a De Morgan
  identity, and `A-B = A & (A^B)`;
* the four compound assignment operators checked against the free-operator
  forms;
* array/array intersection checked against `std::set_intersection` over 400
  random pairs of runs drawn from 0..39 elements, which covers both the SSE2
  block (taken when both runs hold at least 4) and the galloping loop
  (everything else); `gallop` is separately pinned against
  `std::lower_bound`. A non-SSE2 target compiles the galloping loop in place of
  the SIMD block, so what the fallback does on long skewed runs is only
  exercised by the trials that reach it here;
* representation invariants: form flips at exactly 4 096 members and compacts
  again at 4 096, `insert_range` across container boundaries, a whole container
  taken as a run staying bitmap-shaped, an inverted span rejected, empty
  results erasing whole slots, and the boundary values
  `0 / 1 / 65535 / 65536 / 4294967294 / 4294967295`;
* `count_below` cross-checked against actually iterating, `select`
  round-tripping with `count_below` and working inside a bitmap container,
  `min`/`max`, copy and move, `initializer_list` and `span` construction plus braced-list `insert`,
  `fingerprint` order-independence and value sensitivity;
* codec: 40 random round trips plus explicit empty, single-value and
  mixed-form ones, then mutation of a valid buffer at chosen fields — magic,
  version, reserved flags, declared slot count, first key, trailer — asserting
  `decode` returns `std::nullopt` every time; a separate case flips the high bit
  at **every offset** of a frame and requires rejection at each one; and
  reproducibility on trailing garbage;
* a memory assertion for the case compression exists to serve: `{0,
  4294967295}` costs 536 870 912 bytes flat and under 1 KiB here, and the test
  requires the gap to be at least 1000×.

Every timing comparison in the bench is also an agreement check: it prints
`(agreement: yes)` / `(sizes agree: yes)` for the membership and intersection
rows and prints `NO` — visibly, in the output you are asked to paste — if
bitlane and the STL ever disagree.

## Concurrency

`bitlane::bitmap` has no `mutable` members, no lazy caches and no global state,
so:

* any number of threads may hold **read-only** references to the same bitmap
  (`contains`, `cardinality`, iteration, `encode`, the set-algebra operators)
  while no thread writes it;
* writers need their own synchronisation — the type is a value, so the
  cheapest correct pattern is copy-on-write under a mutex;
* iterators hold a pointer to their owner, so a bitmap that a range-for loop is
  walking must not be mutated during it. This is a normal C++ container rule and
  the reason there is no internal lock pretending otherwise.

## Development

Requirements: CMake ≥ 3.21, a C++20 compiler. Everything measured and asserted
in this README was built and run locally with MSVC 19.51 (`/W4 /permissive-
/utf-8`), warning-free, through the Visual Studio generator. The Ninja generator
and the GCC, Clang and Apple Clang builds are exercised in CI rather than on this
machine.
`.github/workflows/ci.yml` runs five jobs, which report as eight checks: each of
the five headers compiled as the only include in a translation unit, the suite
under GCC, Clang and Apple Clang (Ninja, `-Wall -Wextra -Werror`) and under MSVC
(Visual Studio, multi-config), the same suite again under ASan and UBSan in a
Debug build, an MSVC build with `/W4 /WX`, and an installed tree consumed by a
separate `find_package` project — all eight green on `5af4cdf` and `e8e7b0a`,
the two newest commits for which the macOS leg got a runner as this is written. The red runs before
that were worth having: they traced four wrong `min()`/`select()` values to the
test harness reading a byte of a destroyed `std::optional`, not to the library,
and `CHANGELOG.md` says so in those terms. So did enabling `-Werror` for GCC and
Clang: `/W4` emits no conversion diagnostics, so twenty-some of them were
waiting in code that had been called warning-free on the strength of MSVC alone.
The one thing that has not been reproducible is `Apple Clang` on `macos-latest`.
It has reported `cancelled` with no runner ever assigned to it — zero steps, an
empty runner field — on six of the nine runs on `main` between `b38ec65` and
`4d81a45` (`b38ec65`, `cb878c9`, `d617674`, `b270e95`, `399b447` and `4d81a45`),
and on `b270e95` it reached a third attempt with the same result after eighteen
more minutes of queue; on every one of those six commits all the other checks
went green. Two of the nine, `714c249` and `f2edfcb`, ended `cancelled` with no
job started at all, which is what a queued `main` run becomes when a newer push
arrives before it begins, and the ninth, `5af4cdf`, is green on all eight. Both
ends of that range are fixed, and
`gh api repos/fj-8/bitlane/actions/runs?branch=main&event=push` read against each
run's `jobs_url` is what it was written from: the nine runs after it will say
something else and this sentence stays true. A cancelled job with an empty runner
field is a starved macOS pool rather than a compile failure, and the badge cannot
tell them apart, so the distinction is written down instead of smoothed over. It
matters here more than it would elsewhere: `Apple Clang` is one of the three legs
that found the integer-conversion warnings this section opens with, so a pool
that will not schedule it is a real gap in the matrix, not a cosmetic one.

```console
$ cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
$ cmake --build build
$ ctest --test-dir build --output-on-failure
$ ./build/bitlane_bench
$ ./build/example_posting_index
$ ./build/example_tag_query
```

On Windows, with nothing on `PATH` and no developer prompt:

```console
C:\> scripts\build-msvc.bat            rem configure + build + ctest
C:\> scripts\build-msvc.bat install    rem the same, then cmake --install to %TEMP%\bitlane-prefix
C:\> scripts\build-msvc.bat bench      rem the same, then one benchmark run
```

It locates the toolset with `vswhere` and uses the Visual Studio generator, so
`vcvars` is never needed. It has been run in all three modes on the machine
above.

Options, all defaulting to on only when bitlane is the top-level project:
`BITLANE_BUILD_TESTS`, `BITLANE_BUILD_BENCH`, `BITLANE_BUILD_EXAMPLES`.

Warnings used on every project-owned target: MSVC `/W4 /permissive- /utf-8`;
otherwise `-Wall -Wextra -Wpedantic -Wshadow -Wconversion -Wsign-conversion`.
Consumers importing the interface target keep their own settings.

`example_tag_query` builds 8 tag posting lists over 2 000 000 product ids and
answers a compound query; `example_posting_index` indexes 8 documents and
prints the algebra over real terms. Both outputs shown in their headers are the
actual captured runs, and every CI job runs both binaries. `find_package` from
an installed prefix is verified locally and by the `package` job.

## Elsewhere in this account

The same rules applied in three other languages — zero dependencies, and every
number in the README measured in the same checkout:

- [`karg`](https://github.com/fj-8/karg) — dependency-free Rust argument parser
  with typed accessors and errors that name the nearest valid flag.
- [`sift`](https://github.com/fj-8/sift) — pure-stdlib Python 3.11+ streaming
  estimators (HyperLogLog, Count-Min, t-digest, Welford, reservoir) whose frame
  format carries a version and a CRC32 for the same reason this one does.
- [`turnstile`](https://github.com/fj-8/turnstile) — bounded MPMC queue over a
  fixed ring for the JVM, lock-free on `VarHandle` release/acquire, with the
  protocol's invariants proved in `docs/ALGORITHMS.md` rather than inferred from
  the source.

## License

Dual-licensed under [MIT](LICENSE-MIT) or [Apache-2.0](LICENSE-APACHE), at your
option. SPDX headers on every file read `MIT OR Apache-2.0`.
