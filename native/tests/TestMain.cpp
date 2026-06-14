#ifdef COREVIDEO_USE_SYSTEM_GTEST
#include <gtest/gtest.h>
#else
#include "gtest/gtest.h"
#endif

int main(int argc, char** argv) {
  testing::InitGoogleTest(&argc, argv);
  return testing::RUN_ALL_TESTS();
}
