#pragma once

// Minimal test framework: no external dependency, works on any platform.

#include <cmath>
#include <functional>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

namespace test {

struct Case {
  std::string name;
  std::function<void()> body;
};

inline std::vector<Case>& registry() {
  static std::vector<Case> cases;
  return cases;
}

inline int& failures() {
  static int count = 0;
  return count;
}

inline std::string& current_case() {
  static std::string name;
  return name;
}

struct Registrar {
  Registrar(const std::string& name, std::function<void()> body) {
    registry().push_back(Case{name, std::move(body)});
  }
};

inline void report_failure(const std::string& expression, const std::string& file,
                           int line, const std::string& detail = {}) {
  failures()++;
  std::cout << "  FAIL " << file << ":" << line << "  " << expression;
  if (!detail.empty()) {
    std::cout << "  (" << detail << ")";
  }
  std::cout << std::endl;
}

inline bool nearly_equal(double a, double b, double tolerance) {
  if (std::isinf(a) && std::isinf(b)) {
    return (a > 0) == (b > 0);
  }
  return std::fabs(a - b) <= tolerance;
}

/** Human readable rendering of a value, used in failure messages. */
template <typename T>
inline std::string describe(const T& value) {
  std::ostringstream out;
  out << value;
  return out.str();
}

template <typename T>
inline std::string describe(const std::vector<T>& values) {
  std::ostringstream out;
  out << "[";
  for (size_t i = 0; i < values.size(); ++i) {
    if (i != 0) {
      out << ", ";
    }
    out << describe(values[i]);
  }
  out << "]";
  return out.str();
}

inline int run_all() {
  int passed = 0;
  for (auto& test_case : registry()) {
    current_case() = test_case.name;
    const int before = failures();
    std::cout << "[ RUN  ] " << test_case.name << std::endl;
    try {
      test_case.body();
    } catch (const std::exception& ex) {
      report_failure("unexpected exception", __FILE__, __LINE__, ex.what());
    } catch (...) {
      report_failure("unexpected exception", __FILE__, __LINE__);
    }
    if (failures() == before) {
      ++passed;
      std::cout << "[  OK  ] " << test_case.name << std::endl;
    }
  }
  // Counted per case, not per failure: one case can fail several checks, so
  // deriving "passed" from the failure count reports nonsense ("0 passed, 13
  // failed, 13 total" for nine cases).  NOTE: this file is shared with the
  // sibling aes67-sip repository, which still has the derived form.
  const size_t failed = registry().size() - static_cast<size_t>(passed);
  std::cout << std::endl
            << passed << " passed, " << failed << " failed, " << registry().size()
            << " total" << std::endl;
  return failures() == 0 ? 0 : 1;
}

}  // namespace test

#define TEST_CASE(name)                                 \
  static void name();                                   \
  static test::Registrar registrar_##name(#name, name); \
  static void name()

#define CHECK(expression)                                    \
  do {                                                       \
    if (!(expression)) {                                     \
      test::report_failure(#expression, __FILE__, __LINE__); \
    }                                                        \
  } while (0)

#define CHECK_EQ(actual, expected)                                           \
  do {                                                                       \
    const auto& a_ = (actual);                                               \
    const auto& e_ = (expected);                                             \
    if (!(a_ == e_)) {                                                       \
      test::report_failure(                                                  \
          #actual, __FILE__, __LINE__,                                       \
          "expected " + test::describe(e_) + ", got " + test::describe(a_)); \
    }                                                                        \
  } while (0)

#define CHECK_NEAR(actual, expected, tolerance)                           \
  do {                                                                    \
    const double a_ = static_cast<double>(actual);                        \
    const double e_ = static_cast<double>(expected);                      \
    if (!test::nearly_equal(a_, e_, tolerance)) {                         \
      std::ostringstream os_;                                             \
      os_ << "expected " << e_ << " +/- " << tolerance << ", got " << a_; \
      test::report_failure(#actual, __FILE__, __LINE__, os_.str());       \
    }                                                                     \
  } while (0)

#define CHECK_THROWS(expression)                                                \
  do {                                                                          \
    bool threw_ = false;                                                        \
    try {                                                                       \
      (void)(expression);                                                       \
    } catch (...) {                                                             \
      threw_ = true;                                                            \
    }                                                                           \
    if (!threw_) {                                                              \
      test::report_failure("expected an exception from " #expression, __FILE__, \
                           __LINE__);                                           \
    }                                                                           \
  } while (0)
