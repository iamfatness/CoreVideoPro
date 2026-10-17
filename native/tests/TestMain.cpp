#ifdef COREVIDEO_USE_SYSTEM_GTEST
#include <gtest/gtest.h>
#else
#include "gtest/gtest.h"
#endif
#include <cstdio>
#include <cstdlib>
#include <algorithm>

int main(int argc, char** argv) {
  const auto environment = [](const char* name) {
    const char* value = std::getenv(name);
    return value ? value : "<unset; factory default>";
  };
  std::fprintf(stderr, "[gpu-test-run] COREVIDEO_ISOLATE_MONITORS=%s COREVIDEO_CPU_SOURCE_PREPARATION=%s\n",
      environment("COREVIDEO_ISOLATE_MONITORS"), environment("COREVIDEO_CPU_SOURCE_PREPARATION"));
  testing::InitGoogleTest(&argc, argv);
#ifndef COREVIDEO_USE_SYSTEM_GTEST
  const auto& tests = testing::registry();
  if (std::none_of(tests.begin(), tests.end(), testing::shouldRunTest)) {
    std::fprintf(stderr, "No native tests matched the requested filter; validation is incomplete.\n");
    return 1;
  }
#endif
  const auto result = testing::RUN_ALL_TESTS();
#ifdef COREVIDEO_USE_SYSTEM_GTEST
  if (testing::UnitTest::GetInstance()->test_to_run_count() == 0) {
    std::fprintf(stderr, "No native tests matched the requested filter; validation is incomplete.\n");
    return 1;
  }
#endif
  return result;
}
