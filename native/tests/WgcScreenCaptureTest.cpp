#include "modules/Interfaces.h"

#include <gtest/gtest.h>

#if defined(COREVIDEO_WITH_WGC) && COREVIDEO_WITH_WGC
#include <windows.h>
#endif

using corevideo::modules::createWgcScreenCaptureDevice;

#if defined(COREVIDEO_WITH_WGC) && COREVIDEO_WITH_WGC
TEST(WgcScreenCapture, WindowTitlesAreUtf8RegardlessOfWindowsAnsiCodePage) {
  // A visible window outside the desktop keeps enumeration real without
  // opening an interactive test window over the operator's work.
  struct OwnedWindow {
    HWND value;
    ~OwnedWindow() { if (value) DestroyWindow(value); }
  } window{CreateWindowExW(0, L"STATIC", L"CoreVideo UTF-8 QA \u00E9 \u2019 \U0001F3A5",
      WS_POPUP | WS_VISIBLE, -32000, -32000, 200, 120, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr)};
  ASSERT_NE(window.value, nullptr);
  auto device = createWgcScreenCaptureDevice();
  ASSERT_NE(device, nullptr);
  bool found = false;
  for (const auto& info : device->enumerate()) {
    if (info.name == "CoreVideo UTF-8 QA \xC3\xA9 \xE2\x80\x99 \xF0\x9F\x8E\xA5") found = true;
  }
  EXPECT_TRUE(found);
}
#endif

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
