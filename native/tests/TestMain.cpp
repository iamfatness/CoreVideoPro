#ifdef COREVIDEO_USE_SYSTEM_GTEST
#include <gtest/gtest.h>
#else
#include "gtest/gtest.h"
#endif
#include <cstdio>
#include <cstdlib>

int main(int argc, char** argv) {
  const auto environment = [](const char* name) {
    const char* value = std::getenv(name);
    return value ? value : "<unset; factory default>";
  };
  std::fprintf(stderr, "[gpu-test-run] COREVIDEO_ISOLATE_MONITORS=%s COREVIDEO_CPU_SOURCE_PREPARATION=%s\n",
      environment("COREVIDEO_ISOLATE_MONITORS"), environment("COREVIDEO_CPU_SOURCE_PREPARATION"));
  testing::InitGoogleTest(&argc, argv);
  return testing::RUN_ALL_TESTS();
}
