#include "modules/ZoomEngineRuntime.h"

#include <gtest/gtest.h>

#include <cstdlib>

namespace {

void setEnv(const char* name, const char* value) {
#if defined(_WIN32)
  _putenv_s(name, value);
#else
  setenv(name, value, 1);
#endif
}

void unsetEnv(const char* name) {
#if defined(_WIN32)
  _putenv_s(name, "");
#else
  unsetenv(name);
#endif
}

}  // namespace

TEST(ZoomEngineRuntime, IsDisabledWhenNoEnginePathIsConfigured) {
  unsetEnv("COREVIDEO_ZOOM_ENGINE_PATH");

  corevideo::modules::ZoomEngineRuntime runtime;

  EXPECT_FALSE(runtime.configured());
  EXPECT_TRUE(runtime.join(corevideo::rpc::Json::Object{}).isNull());
  EXPECT_TRUE(runtime.leave().isNull());
  EXPECT_TRUE(runtime.snapshot().isNull());
}

TEST(ZoomEngineRuntime, RejectsJoinWithoutNumericMeetingIdBeforeLaunch) {
  setEnv("COREVIDEO_ZOOM_ENGINE_PATH", "C:/missing/corevideo-zoom-engine.exe");

  corevideo::modules::ZoomEngineRuntime runtime;
  const auto snapshot = runtime.join(corevideo::rpc::Json::Object{
      {"meetingUrl", "https://zoom.us/j/not-a-meeting"},
      {"displayName", "Operator"},
      {"webinar", false},
  });

  EXPECT_TRUE(runtime.configured());
  EXPECT_EQ(snapshot.getString("meetingState"), "error");
  ASSERT_NE(snapshot.get("warnings"), nullptr);
  EXPECT_FALSE(snapshot.get("warnings")->asArray().empty());

  unsetEnv("COREVIDEO_ZOOM_ENGINE_PATH");
}
