#ifndef MINIDB_TESTS_TEST_FRAMEWORK_HPP
#define MINIDB_TESTS_TEST_FRAMEWORK_HPP

// A minimal test framework.
//
// Why not GoogleTest or Catch2? MiniDB has no third-party dependencies, and
// keeping it that way means the project configures and builds offline on any
// machine with a compiler and CMake. The features a project this size needs
// -- registration, assertions, a pass/fail summary -- fit in this header, and
// CTest already provides discovery, parallelism and reporting on top.
//
// Usage:
//
//     TEST(name_of_the_case) {
//         EXPECT_EQ(2 + 2, 4);
//         ASSERT_TRUE(pointer != nullptr);   // aborts this case on failure
//     }
//
//     MINIDB_TEST_MAIN()

#include <cstddef>
#include <exception>
#include <functional>
#include <iostream>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace minidb::testing {

/// Thrown by ASSERT_* to abandon the current test case. Caught by the runner,
/// never by test code.
class AssertionFailure : public std::exception {
public:
    explicit AssertionFailure(std::string what) : what_(std::move(what)) {}
    [[nodiscard]] const char* what() const noexcept override { return what_.c_str(); }

private:
    std::string what_;
};

struct TestCase {
    std::string_view name;
    std::function<void()> body;
};

/// The registry of test cases.
///
/// A function-local static rather than a namespace-scope global: it is
/// constructed on first use, which sidesteps the static initialisation order
/// problem when self-registering tests run before main().
inline std::vector<TestCase>& registry() {
    static std::vector<TestCase> cases;
    return cases;
}

/// Registers one test case as a side effect of its own construction.
/// One instance is created per TEST() macro at namespace scope.
struct Registrar {
    Registrar(std::string_view name, std::function<void()> body) {
        registry().push_back(TestCase{name, std::move(body)});
    }
};

/// Number of non-fatal (EXPECT_*) failures in the case currently running.
inline int& current_case_failures() {
    static int failures = 0;
    return failures;
}

inline void report_failure(std::string_view file, int line, const std::string& detail) {
    std::cout << "    FAILED " << file << ":" << line << "\n"
              << "      " << detail << "\n";
}

/// Renders a value for a failure message, falling back to a placeholder for
/// types that have no operator<<. The `requires` expression is a C++20
/// concept check evaluated at compile time.
template<typename T>
std::string describe(const T& value) {
    if constexpr (requires(std::ostream& os) { os << value; }) {
        std::ostringstream stream;
        stream << value;
        return stream.str();
    } else {
        return "<not printable>";
    }
}

/// Runs every registered case. Returns a process exit code.
inline int run_all() {
    std::size_t passed = 0;
    std::vector<std::string_view> failed;

    for (const TestCase& test : registry()) {
        std::cout << "[ RUN      ] " << test.name << "\n";
        current_case_failures() = 0;

        try {
            test.body();
        } catch (const AssertionFailure&) {
            // Already reported at the assertion site; counted below.
        } catch (const std::exception& error) {
            ++current_case_failures();
            std::cout << "    FAILED unexpected exception: " << error.what() << "\n";
        } catch (...) {
            ++current_case_failures();
            std::cout << "    FAILED unexpected non-standard exception\n";
        }

        if (current_case_failures() == 0) {
            std::cout << "[       OK ] " << test.name << "\n";
            ++passed;
        } else {
            std::cout << "[  FAILED  ] " << test.name << "\n";
            failed.push_back(test.name);
        }
    }

    std::cout << "\n" << passed << " passed, " << failed.size() << " failed\n";
    for (std::string_view name : failed) {
        std::cout << "  failed: " << name << "\n";
    }
    return failed.empty() ? 0 : 1;
}

}  // namespace minidb::testing

// --------------------------------------------------------------------------
// Macros
//
// Macros are avoided elsewhere in MiniDB, but a test framework genuinely needs
// them: only a macro can capture __FILE__, __LINE__ and the source text of the
// expression being checked.
// --------------------------------------------------------------------------

#define MINIDB_TEST_CONCAT_INNER(a, b) a##b
#define MINIDB_TEST_CONCAT(a, b) MINIDB_TEST_CONCAT_INNER(a, b)

/// Defines and registers a test case.
#define TEST(name)                                                                            \
    static void name();                                                                       \
    static const ::minidb::testing::Registrar MINIDB_TEST_CONCAT(registrar_, __LINE__)(#name, \
                                                                                       name); \
    static void name()

/// Records a failure and continues the case.
#define EXPECT_TRUE(expression)                                                                   \
    do {                                                                                          \
        if (!(expression)) {                                                                      \
            ++::minidb::testing::current_case_failures();                                         \
            ::minidb::testing::report_failure(__FILE__, __LINE__, "expected true: " #expression); \
        }                                                                                         \
    } while (false)

#define EXPECT_FALSE(expression) EXPECT_TRUE(!(expression))

#define EXPECT_EQ(actual, expected)                                                         \
    do {                                                                                    \
        const auto& minidb_actual = (actual);                                               \
        const auto& minidb_expected = (expected);                                           \
        if (!(minidb_actual == minidb_expected)) {                                          \
            ++::minidb::testing::current_case_failures();                                   \
            ::minidb::testing::report_failure(                                              \
                __FILE__, __LINE__,                                                         \
                std::string(#actual " == " #expected "\n        actual:   ") +              \
                    ::minidb::testing::describe(minidb_actual) +                            \
                    "\n        expected: " + ::minidb::testing::describe(minidb_expected)); \
        }                                                                                   \
    } while (false)

#define EXPECT_NE(actual, unexpected) EXPECT_TRUE(!((actual) == (unexpected)))

/// Records a failure and abandons the case. Use when continuing would crash,
/// for example after a null check.
#define ASSERT_TRUE(expression)                                                                   \
    do {                                                                                          \
        if (!(expression)) {                                                                      \
            ++::minidb::testing::current_case_failures();                                         \
            ::minidb::testing::report_failure(__FILE__, __LINE__, "required true: " #expression); \
            throw ::minidb::testing::AssertionFailure(#expression);                               \
        }                                                                                         \
    } while (false)

/// Fails unless the statement throws the named exception type.
#define EXPECT_THROWS_AS(statement, exception_type)                                          \
    do {                                                                                     \
        bool minidb_threw_expected = false;                                                  \
        try {                                                                                \
            statement;                                                                       \
        } catch (const exception_type&) {                                                    \
            minidb_threw_expected = true;                                                    \
        } catch (...) {                                                                      \
            /* wrong exception type; reported below */                                       \
        }                                                                                    \
        if (!minidb_threw_expected) {                                                        \
            ++::minidb::testing::current_case_failures();                                    \
            ::minidb::testing::report_failure(__FILE__, __LINE__,                            \
                                              #statement " did not throw " #exception_type); \
        }                                                                                    \
    } while (false)

/// Defines main() for a test binary.
#define MINIDB_TEST_MAIN()                   \
    int main() {                             \
        return ::minidb::testing::run_all(); \
    }

#endif  // MINIDB_TESTS_TEST_FRAMEWORK_HPP
