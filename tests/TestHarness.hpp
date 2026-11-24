#pragma once

// Minimal self-contained test runner (no third-party framework).
//   CADENCE_TEST(Name) { ... }              registers a named test case
//   CADENCE_CHECK(cond, msg)                records a failure and continues
//   CADENCE_REQUIRE(cond, msg)              records a failure and leaves the current test
// The executable runs every test, or only the ones named on the command line; see RunTests().

#include <algorithm>
#include <iostream>
#include <set>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

namespace cadence::test {

struct TestCase {
    const char* name;
    void (*fn)();
};

inline std::vector<TestCase>& Registry() {
    static std::vector<TestCase> tests;
    return tests;
}

inline int& CurrentFailures() {
    static int failures = 0;
    return failures;
}

struct Registrar {
    Registrar(const char* name, void (*fn)()) { Registry().push_back({name, fn}); }
};

inline void ReportFailure(const char* expr, std::string_view msg, const char* file, int line) {
    ++CurrentFailures();
    std::cerr << "    [FAIL] " << msg << "\n           " << expr << " (" << file << ":" << line << ")\n";
}

// Usage: test_core                      -> run all tests
//        test_core NameA NameB          -> run only these tests (unknown names fail)
//        test_core --list               -> print all test names
//        test_core --expect-tests=A,B   -> fail unless exactly these tests exist (keeps CMake in sync)
inline int RunTests(int argc, char** argv) {
    auto& tests = Registry();
    std::vector<std::string> selected;
    for (int i = 1; i < argc; ++i) {
        const std::string_view arg = argv[i];
        if (arg == "--list") {
            for (const auto& t : tests) std::cout << t.name << "\n";
            return 0;
        }
        if (arg.starts_with("--expect-tests=")) {
            std::set<std::string> expected;
            std::stringstream list{std::string(arg.substr(15))};
            for (std::string name; std::getline(list, name, ',');) {
                if (!name.empty()) expected.insert(name);
            }
            std::set<std::string> actual;
            for (const auto& t : tests) actual.insert(t.name);
            if (expected == actual) {
                std::cout << "Test list in sync (" << actual.size() << " tests)\n";
                return 0;
            }
            for (const auto& n : actual)   if (!expected.contains(n)) std::cerr << "Not registered in CMake: " << n << "\n";
            for (const auto& n : expected) if (!actual.contains(n))   std::cerr << "Listed in CMake but missing: " << n << "\n";
            return 1;
        }
        selected.emplace_back(arg);
    }

    int failedTests = 0;
    int ran         = 0;
    for (const auto& name : selected) {
        const bool known = std::any_of(tests.begin(), tests.end(), [&](const TestCase& t) { return name == t.name; });
        if (!known) {
            std::cerr << "[FAIL] Unknown test: " << name << "\n";
            ++failedTests;
        }
    }
    for (const auto& t : tests) {
        if (!selected.empty() && std::find(selected.begin(), selected.end(), t.name) == selected.end()) continue;
        CurrentFailures() = 0;
        std::cout << "[RUN ] " << t.name << "\n";
        t.fn();
        ++ran;
        if (CurrentFailures() == 0) {
            std::cout << "[PASS] " << t.name << "\n";
        } else {
            std::cout << "[FAIL] " << t.name << " (" << CurrentFailures() << " check(s) failed)\n";
            ++failedTests;
        }
    }
    std::cout << "\n" << ran << " test(s) run, " << failedTests << " failed\n";
    return failedTests == 0 ? 0 : 1;
}

} // namespace cadence::test

#define CADENCE_TEST(name)                                                              \
    static void name();                                                                 \
    static const ::cadence::test::Registrar name##_registrar_(#name, &name);            \
    static void name()

#define CADENCE_CHECK(cond, msg)                                                        \
    do {                                                                                \
        if (!(cond)) ::cadence::test::ReportFailure(#cond, (msg), __FILE__, __LINE__);  \
    } while (0)

#define CADENCE_REQUIRE(cond, msg)                                                      \
    do {                                                                                \
        if (!(cond)) {                                                                  \
            ::cadence::test::ReportFailure(#cond, (msg), __FILE__, __LINE__);           \
            return;                                                                     \
        }                                                                               \
    } while (0)
