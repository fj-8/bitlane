// Minimal test harness: no dependencies, exit code 0 only when everything passed.
// SPDX-License-Identifier: MIT OR Apache-2.0
#ifndef BITLANE_TESTKIT_HPP
#define BITLANE_TESTKIT_HPP

#include <concepts>
#include <cstddef>
#include <cstdio>
#include <random>
#include <string>
#include <type_traits>
#include <vector>

namespace testkit {

struct case_t {
    const char* name;
    void (*fn)();
};

inline std::vector<case_t>& registry() {
    static std::vector<case_t> cases;
    return cases;
}

struct registrar {
    registrar(const char* name, void (*fn)()) { registry().push_back(case_t{name, fn}); }
};

/// Thrown by CHECK so a failing case stops without killing the whole run.
struct failure {
    std::string what;
};

[[noreturn]] inline void fail(const char* file, int line, const std::string& message) {
    char buffer[512];
    std::snprintf(buffer, sizeof buffer, "%s:%d: %s", file, line, message.c_str());
    throw failure{buffer};
}

// Rendering the two operands turns "values differ" into something a maintainer
// can act on. Anything the suite does not know how to print falls back to a
// label rather than a compile error, so adding a check on a new type is safe.
inline std::string describe(bool v) { return v ? "true" : "false"; }
inline std::string describe(std::string v) { return "\"" + v + "\""; }
inline std::string describe(const char* v) {
    return v ? "\"" + std::string(v) + "\"" : "nullptr";
}

template <class T>
    requires std::integral<T> || std::floating_point<T>
inline std::string describe(T v) {
    char buffer[64];
    if constexpr (std::integral<T>) {
        if constexpr (std::is_signed_v<T>) {
            std::snprintf(buffer, sizeof buffer, "%lld", static_cast<long long>(v));
        } else {
            std::snprintf(buffer, sizeof buffer, "%llu", static_cast<unsigned long long>(v));
        }
    } else {
        std::snprintf(buffer, sizeof buffer, "%.6g", static_cast<double>(v));
    }
    return buffer;
}

template <class T>
    requires requires(const T& v) { v.size(); v[0]; }
inline std::string describe(const T& v) {
    std::string out = std::to_string(v.size()) + " elements:";
    for (std::size_t i = 0; i < v.size() && i < 4; ++i) {
        out += " " + describe(v[i]);
    }
    if (v.size() > 4) {
        out += " ...";
    }
    return out;
}

template <class T>
    requires(!std::integral<T> && !std::floating_point<T> &&
             !requires(const T& v) { v.size(); v[0]; })
inline std::string describe(const T&) {
    return "(not printable)";
}

inline int run_all(const char* suite) {
    std::size_t passed = 0;
    std::vector<std::string> errors;
    for (const case_t& c : registry()) {
        try {
            c.fn();
            ++passed;
            std::printf("  ok   %s\n", c.name);
        } catch (const failure& f) {
            std::printf("  FAIL %s\n       %s\n", c.name, f.what.c_str());
            errors.push_back(f.what);
        } catch (const std::exception& e) {
            std::printf("  FAIL %s\n       unexpected exception: %s\n", c.name, e.what());
            errors.push_back(e.what());
        }
    }
    std::printf("\n%s: %zu/%zu passed\n", suite, passed, registry().size());
    return errors.empty() ? 0 : 1;
}

// A fixed seed keeps every "random" test reproducible; pass a counter to vary.
inline std::mt19937_64 rng(std::uint64_t seed = 0xB17A2Eull) { return std::mt19937_64(seed); }

} // namespace testkit

#define TEST(name)                                                               \
    static void name();                                                          \
    static ::testkit::registrar registrar_##name(#name, &name);                  \
    static void name()

#define CHECK(cond)                                                              \
    do {                                                                         \
        if (!(cond)) {                                                           \
            ::testkit::fail(__FILE__, __LINE__, "CHECK(" #cond ") failed");       \
        }                                                                        \
    } while (0)

#define CHECK_EQ(a, b)                                                         \
    do {                                                                       \
        /* By value on purpose: an operand such as *b.min() names a byte       \
         * inside the optional temporary, and that temporary dies at the end   \
         * of the initialiser. A reference bound to it reads destroyed         \
         * storage: MSVC kept the stale bytes, GCC -O2 reused the slot and     \
         * returned four wrong values. */                                      \
        const auto bitlane_lhs = (a);                                          \
        const auto bitlane_rhs = (b);                                          \
        if (!(bitlane_lhs == bitlane_rhs)) {                                   \
            ::testkit::fail(__FILE__, __LINE__,                                \
                            std::string(#a " == " #b) + " failed\n  lhs: " +   \
                            ::testkit::describe(bitlane_lhs) + "\n  rhs: " +   \
                            ::testkit::describe(bitlane_rhs));                 \
        }                                                                      \
    } while (0)

#endif // BITLANE_TESTKIT_HPP
