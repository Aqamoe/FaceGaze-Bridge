// 极简测试框架。刻意不引 Catch2：M1 只需要断言 + 用例注册，
// 引第三方前先证明真的需要它。
#pragma once

#include <cmath>
#include <cstdio>
#include <vector>

namespace fgb::test {

struct TestCase {
    const char* name;
    void (*fn)();
};

inline std::vector<TestCase>& registry() {
    static std::vector<TestCase> instances;
    return instances;
}

inline int& failure_count() {
    static int count = 0;
    return count;
}

struct Registrar {
    Registrar(const char* name, void (*fn)()) { registry().push_back({name, fn}); }
};

inline int run_all() {
    int failed_cases = 0;
    for (const auto& tc : registry()) {
        const int before = failure_count();
        std::fprintf(stderr, "[ RUN  ] %s\n", tc.name);
        tc.fn();
        if (failure_count() != before) {
            std::fprintf(stderr, "[ FAIL ] %s\n", tc.name);
            ++failed_cases;
        } else {
            std::fprintf(stderr, "[  OK  ] %s\n", tc.name);
        }
    }
    std::fprintf(stderr, "\n%zu 个用例，%d 个失败用例，%d 处断言失败\n", registry().size(),
                 failed_cases, failure_count());
    return failed_cases == 0 ? 0 : 1;
}

} // namespace fgb::test

#define FGB_TEST(name)                                                       \
    static void name();                                                      \
    static ::fgb::test::Registrar fgb_reg_##name{#name, name};               \
    static void name()

#define FGB_CHECK(cond)                                                                     \
    do {                                                                                    \
        if (!(cond)) {                                                                      \
            std::fprintf(stderr, "    %s:%d  断言失败: %s\n", __FILE__, __LINE__, #cond);   \
            ++::fgb::test::failure_count();                                                  \
        }                                                                                    \
    } while (0)

#define FGB_CHECK_NEAR(actual, expected, eps)                                                    \
    do {                                                                                         \
        const double a_ = static_cast<double>(actual);                                           \
        const double e_ = static_cast<double>(expected);                                         \
        if (std::fabs(a_ - e_) > (eps)) {                                                        \
            std::fprintf(stderr, "    %s:%d  期望 %.6f ± %.6f，实际 %.6f\n", __FILE__, __LINE__, \
                         e_, static_cast<double>(eps), a_);                                      \
            ++::fgb::test::failure_count();                                                      \
        }                                                                                        \
    } while (0)
