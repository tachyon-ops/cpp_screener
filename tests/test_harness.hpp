#pragma once
#include <iostream>
#include <vector>
#include <string>
#include <functional>
#include <cmath>
#include <sstream>

namespace test {

struct TestInfo {
    std::string name;
    std::function<void()> func;
};

inline std::vector<TestInfo>& get_tests() {
    static std::vector<TestInfo> tests;
    return tests;
}

struct TestRegistrar {
    TestRegistrar(const std::string& name, std::function<void()> func) {
        get_tests().push_back({name, func});
    }
};

#define TEST_CASE(name) \
    static void name##_func(); \
    static test::TestRegistrar name##_registrar(#name, name##_func); \
    static void name##_func()

struct TestContext {
    static inline int assertion_count = 0;
    static inline int failure_count = 0;
    static inline std::vector<std::string> failures;
};

#define ASSERT_TRUE(cond) \
    do { \
        test::TestContext::assertion_count++; \
        if (!(cond)) { \
            test::TestContext::failure_count++; \
            std::stringstream ss; \
            ss << __FILE__ << ":" << __LINE__ << ": Assertion failed: " << #cond; \
            std::string fail_msg = ss.str(); \
            test::TestContext::failures.push_back(fail_msg); \
            std::cerr << "  \033[1;31m[FAIL]\033[0m " << fail_msg << std::endl; \
            return; \
        } \
    } while(0)

#define ASSERT_FALSE(cond) \
    do { \
        test::TestContext::assertion_count++; \
        if ((cond)) { \
            test::TestContext::failure_count++; \
            std::stringstream ss; \
            ss << __FILE__ << ":" << __LINE__ << ": Assertion failed: !" << #cond; \
            std::string fail_msg = ss.str(); \
            test::TestContext::failures.push_back(fail_msg); \
            std::cerr << "  \033[1;31m[FAIL]\033[0m " << fail_msg << std::endl; \
            return; \
        } \
    } while(0)

#define ASSERT_EQ(val1, val2) \
    do { \
        test::TestContext::assertion_count++; \
        auto v1 = (val1); \
        auto v2 = (val2); \
        if (!(v1 == v2)) { \
            test::TestContext::failure_count++; \
            std::stringstream ss; \
            ss << __FILE__ << ":" << __LINE__ << ": Assertion failed: " << #val1 << " == " << #val2 << " (Actual: " << v1 << " vs Expected: " << v2 << ")"; \
            std::string fail_msg = ss.str(); \
            test::TestContext::failures.push_back(fail_msg); \
            std::cerr << "  \033[1;31m[FAIL]\033[0m " << fail_msg << std::endl; \
            return; \
        } \
    } while(0)

#define ASSERT_NEAR(val1, val2, tol) \
    do { \
        test::TestContext::assertion_count++; \
        double v1 = static_cast<double>(val1); \
        double v2 = static_cast<double>(val2); \
        double t = static_cast<double>(tol); \
        if (std::abs(v1 - v2) > t) { \
            test::TestContext::failure_count++; \
            std::stringstream ss; \
            ss << __FILE__ << ":" << __LINE__ << ": Assertion failed: |" << #val1 << " - " << #val2 << "| <= " << #tol << " (Actual: " << v1 << " vs Expected: " << v2 << ", diff: " << std::abs(v1 - v2) << ")"; \
            std::string fail_msg = ss.str(); \
            test::TestContext::failures.push_back(fail_msg); \
            std::cerr << "  \033[1;31m[FAIL]\033[0m " << fail_msg << std::endl; \
            return; \
        } \
    } while(0)

} // namespace test
