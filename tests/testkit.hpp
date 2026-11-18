// Minimal test harness: no dependencies, exit code 0 only when everything passed.
// SPDX-License-Identifier: MIT OR Apache-2.0
#ifndef BITLANE_TESTKIT_HPP
#define BITLANE_TESTKIT_HPP

#include <cstddef>
#include <cstdio>
#include <random>
#include <string>
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

#define CHECK_EQ(a, b)                                                           \
    do {                                                                         \
        const auto& bitlane_lhs = (a);                                                    \
        const auto& bitlane_rhs = (b);                                                    \
        if (!(bitlane_lhs == bitlane_rhs)) {                                                       \
            ::testkit::fail(__FILE__, __LINE__, std::string(#a " == " #b) +      \
                                                  " failed (values differ)");    \
        }                                                                        \
    } while (0)

#endif // BITLANE_TESTKIT_HPP
