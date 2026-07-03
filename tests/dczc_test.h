// SPDX-License-Identifier: Apache-2.0
// dczc — minimal dependency-free test harness
//
// No GTest dependency (keeps the tree buildable on a bare board). Each test file
// defines cases with DCZC_TEST and ends with DCZC_TEST_MAIN(). A non-zero exit
// code marks failure so ctest picks it up.

#pragma once

#include <cstdio>
#include <cstring>
#include <functional>
#include <string>
#include <vector>

namespace dczc_test {

struct Case {
    std::string name;
    std::function<void(bool&)> fn;
};

inline std::vector<Case>& registry() {
    static std::vector<Case> r;
    return r;
}

struct Registrar {
    Registrar(const char* name, std::function<void(bool&)> fn) {
        registry().push_back({name, std::move(fn)});
    }
};

inline int run_all(const char* suite) {
    int failed = 0;
    for (auto& c : registry()) {
        bool ok = true;
        std::printf("[ RUN      ] %s.%s\n", suite, c.name.c_str());
        try {
            c.fn(ok);
        } catch (const std::exception& e) {
            std::printf("[   THREW  ] %s\n", e.what());
            ok = false;
        }
        if (ok) {
            std::printf("[     PASS ] %s.%s\n", suite, c.name.c_str());
        } else {
            std::printf("[     FAIL ] %s.%s\n", suite, c.name.c_str());
            ++failed;
        }
    }
    std::printf("[==========] %zu cases, %d failed\n", registry().size(), failed);
    return failed == 0 ? 0 : 1;
}

}  // namespace dczc_test

#define DCZC_TEST(name)                                                       \
    static void dczc_test_##name(bool& _dczc_ok);                             \
    static ::dczc_test::Registrar dczc_reg_##name(#name, dczc_test_##name);   \
    static void dczc_test_##name([[maybe_unused]] bool& _dczc_ok)

// Soft check: records failure but keeps running the case.
#define CHECK(cond)                                                           \
    do {                                                                      \
        if (!(cond)) {                                                        \
            std::printf("    CHECK failed: %s (%s:%d)\n", #cond,              \
                        __FILE__, __LINE__);                                  \
            _dczc_ok = false;                                                 \
        }                                                                     \
    } while (0)

// Hard check: records failure and returns from the case.
#define REQUIRE(cond)                                                         \
    do {                                                                      \
        if (!(cond)) {                                                        \
            std::printf("    REQUIRE failed: %s (%s:%d)\n", #cond,            \
                        __FILE__, __LINE__);                                  \
            _dczc_ok = false;                                                 \
            return;                                                           \
        }                                                                     \
    } while (0)

#define DCZC_TEST_MAIN(suite)                                                 \
    int main() { return ::dczc_test::run_all(suite); }
