#include "test_harness.hpp"
#include <iostream>

int main() {
    std::cout << "\033[1;34m[START]\033[0m Running trader_tests suite..." << std::endl;
    
    auto& tests = test::get_tests();
    std::cout << "Found " << tests.size() << " test cases." << std::endl;
    
    int passed = 0;
    int failed = 0;
    
    for (const auto& t : tests) {
        std::cout << "\033[1;33m[RUN]  \033[0m " << t.name << std::endl;
        int old_failures = test::TestContext::failure_count;
        try {
            t.func();
            if (test::TestContext::failure_count == old_failures) {
                std::cout << "\033[1;32m[PASS] \033[0m " << t.name << std::endl;
                passed++;
            } else {
                std::cout << "\033[1;31m[FAIL] \033[0m " << t.name << std::endl;
                failed++;
            }
        } catch (const std::exception& e) {
            test::TestContext::failure_count++;
            std::string fail_msg = std::string("Unhandled exception in ") + t.name + ": " + e.what();
            test::TestContext::failures.push_back(fail_msg);
            std::cerr << "  \033[1;31m[FAIL]\033[0m " << fail_msg << std::endl;
            failed++;
        } catch (...) {
            test::TestContext::failure_count++;
            std::string fail_msg = std::string("Unknown unhandled exception in ") + t.name;
            test::TestContext::failures.push_back(fail_msg);
            std::cerr << "  \033[1;31m[FAIL]\033[0m " << fail_msg << std::endl;
            failed++;
        }
    }
    
    std::cout << "\n========================================" << std::endl;
    std::cout << "Test Summary:" << std::endl;
    std::cout << "  Total tests:  " << tests.size() << std::endl;
    std::cout << "  Passed:       " << passed << std::endl;
    std::cout << "  Failed:       " << failed << std::endl;
    std::cout << "  Assertions:   " << test::TestContext::assertion_count << std::endl;
    std::cout << "========================================" << std::endl;
    
    if (failed > 0) {
        std::cout << "\033[1;31mSOME TESTS FAILED\033[0m" << std::endl;
        return 1;
    } else {
        std::cout << "\033[1;32mALL TESTS PASSED\033[0m" << std::endl;
        return 0;
    }
}
