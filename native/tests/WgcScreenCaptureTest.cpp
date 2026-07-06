#include "modules/Interfaces.h"

#include <gtest/gtest.h>

using corevideo::modules::createWgcScreenCaptureDevice;

// Dev builds compile the WGC adapter in (COREVIDEO_WITH_WGC=ON); these run on
// the Windows rig where at least one display exists.
TEST(WgcScreenCapture, EnumeratesAtLeastOneScreen) {
  auto device = createWgcScreenCaptureDevice();
#if defined(COREVIDEO_WITH_WGC) && COREVIDEO_WITH_WGC
  ASSERT_NE(device, nullptr);
  const auto infos = device->enumerate();
  ASSERT_FALSE(infos.empty());
  EXPECT_EQ(infos[0].id.rfind("screen:", 0), 0u);
  EXPECT_EQ(infos[0].kind, "screen");
  EXPECT_GT(infos[0].width, 0);
  EXPECT_GT(infos[0].height, 0);
#else
  EXPECT_EQ(device, nullptr);
#endif
}
