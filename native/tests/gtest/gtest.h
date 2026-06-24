#pragma once

#include <exception>
#include <functional>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace testing {

struct TestInfo {
  std::string suite;
  std::string name;
  std::function<void()> run;
};

inline std::vector<TestInfo>& registry() {
  static std::vector<TestInfo> tests;
  return tests;
}

inline std::string& activeFilter() {
  static std::string filter = "*";
  return filter;
}

inline bool wildcardMatch(const std::string& pattern, const std::string& value) {
  size_t patternIndex = 0;
  size_t valueIndex = 0;
  size_t starIndex = std::string::npos;
  size_t valueAfterStar = 0;
  while (valueIndex < value.size()) {
    if (patternIndex < pattern.size() &&
        (pattern[patternIndex] == '?' || pattern[patternIndex] == value[valueIndex])) {
      ++patternIndex;
      ++valueIndex;
    } else if (patternIndex < pattern.size() && pattern[patternIndex] == '*') {
      starIndex = patternIndex++;
      valueAfterStar = valueIndex;
    } else if (starIndex != std::string::npos) {
      patternIndex = starIndex + 1;
      valueIndex = ++valueAfterStar;
    } else {
      return false;
    }
  }
  while (patternIndex < pattern.size() && pattern[patternIndex] == '*') {
    ++patternIndex;
  }
  return patternIndex == pattern.size();
}

inline bool shouldRunTest(const TestInfo& test) {
  const std::string fullName = test.suite + "." + test.name;
  return wildcardMatch(activeFilter(), fullName);
}

struct Registrar {
  Registrar(std::string suite, std::string name, std::function<void()> run) {
    registry().push_back({std::move(suite), std::move(name), std::move(run)});
  }
};

class AssertionStream {
 public:
  AssertionStream(bool ok, const char* file, int line) : ok_(ok), file_(file), line_(line) {}
  ~AssertionStream() noexcept(false) {
    if (!ok_) {
      throw std::runtime_error(file_ + ":" + std::to_string(line_) + " assertion failed: " + message_.str());
    }
  }

  template <typename T>
  AssertionStream& operator<<(const T& value) {
    if (!ok_) {
      message_ << value;
    }
    return *this;
  }

 private:
  bool ok_;
  std::string file_;
  int line_;
  std::ostringstream message_;
};

inline int InitGoogleTest(int* argc, char** argv) {
  if (!argc || !argv) {
    return 0;
  }
  constexpr const char* prefix = "--gtest_filter=";
  constexpr size_t prefixLength = 15;
  for (int i = 1; i < *argc; ++i) {
    const std::string arg = argv[i] ? argv[i] : "";
    if (arg.rfind(prefix, 0) == 0) {
      activeFilter() = arg.substr(prefixLength);
    }
  }
  return 0;
}

inline int RUN_ALL_TESTS() {
  int failed = 0;
  int executed = 0;
  for (const auto& test : registry()) {
    if (!shouldRunTest(test)) {
      continue;
    }
    ++executed;
    try {
      test.run();
      std::cout << "[  PASSED  ] " << test.suite << "." << test.name << '\n';
    } catch (const std::exception& ex) {
      ++failed;
      std::cerr << "[  FAILED  ] " << test.suite << "." << test.name << ": " << ex.what() << '\n';
    }
  }
  std::cout << executed - failed << " tests passed, " << failed << " failed.\n";
  return failed == 0 ? 0 : 1;
}

}  // namespace testing

#define TEST(SuiteName, TestName)                                                                                \
  static void SuiteName##_##TestName##_impl();                                                                   \
  static ::testing::Registrar SuiteName##_##TestName##_registrar(#SuiteName, #TestName, SuiteName##_##TestName##_impl); \
  static void SuiteName##_##TestName##_impl()

#define EXPECT_TRUE(condition) ::testing::AssertionStream(static_cast<bool>(condition), __FILE__, __LINE__)
#define EXPECT_FALSE(condition) ::testing::AssertionStream(!static_cast<bool>(condition), __FILE__, __LINE__)
#define ASSERT_TRUE(condition) ::testing::AssertionStream(static_cast<bool>(condition), __FILE__, __LINE__)
#define ASSERT_FALSE(condition) ::testing::AssertionStream(!static_cast<bool>(condition), __FILE__, __LINE__)
#define EXPECT_EQ(left, right) ::testing::AssertionStream(((left) == (right)), __FILE__, __LINE__)
#define EXPECT_NE(left, right) ::testing::AssertionStream(((left) != (right)), __FILE__, __LINE__)
#define ASSERT_NE(left, right) ::testing::AssertionStream(((left) != (right)), __FILE__, __LINE__)
#define EXPECT_GE(left, right) ::testing::AssertionStream(((left) >= (right)), __FILE__, __LINE__)
