#ifdef COREVIDEO_USE_SYSTEM_GTEST
#include <gtest/gtest.h>
#else
#include "gtest/gtest.h"
#endif
#include <cstdio>
#include <cstdlib>
#include <algorithm>
#include <string_view>
#include "core/ComApartmentLifetime.h"

int main(int argc, char** argv) {
#ifndef COREVIDEO_USE_SYSTEM_GTEST
  bool listTests = false;
  for (int index = 1; index < argc; ++index) {
    const std::string_view option = argv[index];
    if (option == "--gtest_list_tests") {
      listTests = true;
    } else if (!option.starts_with("--gtest_filter=")) {
      std::fprintf(stderr, "Unsupported native test argument: %s\n", argv[index]);
      return 2;
    }
  }
  testing::InitGoogleTest(&argc, argv);
  const auto& tests = testing::registry();
  if (std::none_of(tests.begin(), tests.end(), testing::shouldRunTest)) {
    std::fprintf(stderr, "No native tests matched the requested filter; validation is incomplete.\n");
    return 1;
  }
  if (listTests) {
    for (const auto& test : tests) {
      if (testing::shouldRunTest(test)) {
        std::printf("%s.%s\n", test.suite.c_str(), test.name.c_str());
      }
    }
    return 0;
  }
#endif
  // Match production main: COM/WinRT activation caches and worker-owned media
  // must not outlive the final apartment between successive capture fixtures.
  corevideo::core::ComApartmentLifetime apartment;
  if (!apartment.initialized()) {
    std::fprintf(stderr, "Native test COM apartment initialization failed.\n");
    return 1;
  }
  const auto environment = [](const char* name) {
    const char* value = std::getenv(name);
    return value ? value : "<unset; factory default>";
  };
  std::fprintf(stderr, "[gpu-test-run] COREVIDEO_ISOLATE_MONITORS=%s COREVIDEO_CPU_SOURCE_PREPARATION=%s\n",
      environment("COREVIDEO_ISOLATE_MONITORS"), environment("COREVIDEO_CPU_SOURCE_PREPARATION"));
#ifdef COREVIDEO_USE_SYSTEM_GTEST
  testing::InitGoogleTest(&argc, argv);
  const auto result = testing::UnitTest::GetInstance()->Run();
  if (testing::UnitTest::GetInstance()->test_to_run_count() == 0) {
    std::fprintf(stderr, "No native tests matched the requested filter; validation is incomplete.\n");
    return 1;
  }
#else
  const auto result = testing::RUN_ALL_TESTS();
#endif
  return result;
}
