// SPDX-License-Identifier: Apache-2.0
// axon — minimal dependency-free test harness
//
// No GTest dependency (keeps the tree buildable on a bare board). Each test file
// defines cases with AXON_TEST and ends with AXON_TEST_MAIN(). A non-zero exit
// code marks failure so ctest picks it up.

#pragma once

#include <cstdio>
#include <cstring>
#include <functional>
#include <string>
#include <vector>

namespace axon_test {

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

}  // namespace axon_test

#define AXON_TEST(name)                                                       \
    static void axon_test_##name(bool& _axon_ok);                             \
    static ::axon_test::Registrar axon_reg_##name(#name, axon_test_##name);   \
    static void axon_test_##name([[maybe_unused]] bool& _axon_ok)

// Soft check: records failure but keeps running the case.
#define CHECK(cond)                                                           \
    do {                                                                      \
        if (!(cond)) {                                                        \
            std::printf("    CHECK failed: %s (%s:%d)\n", #cond,              \
                        __FILE__, __LINE__);                                  \
            _axon_ok = false;                                                 \
        }                                                                     \
    } while (0)

// Hard check: records failure and returns from the case.
#define REQUIRE(cond)                                                         \
    do {                                                                      \
        if (!(cond)) {                                                        \
            std::printf("    REQUIRE failed: %s (%s:%d)\n", #cond,            \
                        __FILE__, __LINE__);                                  \
            _axon_ok = false;                                                 \
            return;                                                           \
        }                                                                     \
    } while (0)

#define AXON_TEST_MAIN(suite)                                                 \
    int main() { return ::axon_test::run_all(suite); }
