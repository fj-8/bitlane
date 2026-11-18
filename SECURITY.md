# Security policy

## What counts as a vulnerability here

`bitlane::bitmap::decode()` exists to be handed bytes from somewhere you do not
control — a cache, a file another process wrote, a request body. Everything
below is in scope:

* Any crash, hang or unbounded allocation reachable from `decode()` on an input
  of a few kilobytes. The decoder must bound its work before it allocates: if a
  patch makes a slot count or cardinality turn into an allocation without first
  checking it against the bytes actually present, that is a vulnerability.
* Any input that decodes into a `bitmap` whose invariants are broken —
  unsorted slots, a container whose cardinality disagrees with its contents, an
  array container above 4 096 members. Such a bitmap silently corrupts every
  later query, which is worse than a rejection.
* Undefined behaviour anywhere: the library contains no `unsafe`-equivalent
  constructs (no aliasing casts, no manual memory, no signed-overflow arithmetic)
  and a patch introducing any of them is a bug even if the tests pass.
* A container-form path that reads past the 1 024 words it owns, or a SIMD merge
  that reads past the end of an input array.
* Memory growth that is documented as bounded but is not: `memory_usage()` and
  the README tables make size claims, and a workload that breaks them
  substantially (for example a bitmap of 1 member over a huge universe holding
  megabytes) is a defect.

Out of scope: making a bitmap that a caller has already decided to build
smaller, the choice of hash in `fingerprint()` (it is an identity helper, not a
MAC — never use it as one), and anything about how you store the encoded bytes.

## Reporting

Open a [private security advisory](https://github.com/fj-8/bitlane/security/advisories/new)
rather than a public issue. Include:

1. the exact byte sequence, ideally as a base64 string or a `.bin` file;
2. the platform, compiler and build type you reproduced it on;
3. whether it was found by fuzzing, and if so the seed or corpus.

A reproducing test case is enough to start; you do not need a fix. The
maintainer acknowledges within 72 hours, and the expectation is that any
`decode()` failure mode gets a case in `decode_rejects_malformed_input` — a
public regression test is added once a patch is released, not before.

## Hardening already present

* `decode()` never reads a byte it has not checked the length for, rejects the
  header before computing any size, and refuses a slot count that could not fit
  in the remaining buffer.
* It validates container shape both ways: an array above the cardinality cap, a
  bitmap at or below it, non-ascending keys or values, a popcount that disagrees
  with the declared cardinality, a total that disagrees with the header, trailing
  or missing bytes, and an FNV-1a checksum over the payload.
* Nothing in the library throws, so there is no exception path to unwind through
  on hostile input; failure is reported as `std::nullopt`.
* No environment variables, no filesystem access, no subprocesses, no
  third-party dependency to inherit a CVE from.
