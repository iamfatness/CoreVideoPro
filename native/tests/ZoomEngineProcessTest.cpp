#include "modules/ZoomEngineProcess.h"

#include <gtest/gtest.h>

TEST(ZoomEngineProcessClient, RejectsEmptyExecutablePath) {
  corevideo::modules::ZoomEngineProcessClient client;

  EXPECT_FALSE(client.start({}));
  EXPECT_FALSE(client.lastError().empty());
  EXPECT_FALSE(client.running());
}

TEST(ZoomEngineProcessClient, RejectsCommandsBeforeIpcConnects) {
  corevideo::modules::ZoomEngineProcessClient client;

  EXPECT_FALSE(client.sendLine(corevideo::modules::buildZoomEngineQuitCommand()));
  EXPECT_FALSE(client.lastError().empty());
}

TEST(ZoomEngineProcessClient, InstanceTokenIsUniquePerSpawnAndPidScoped) {
  // Distinct on every call within this process (a rapid engine restart must not
  // reuse a name), and each carries a "<pid>-<n>" shape so separate app
  // instances (different pids) and the OBS zoom plugin never collide.
  const std::string a = corevideo::modules::makeZoomEngineInstanceToken();
  const std::string b = corevideo::modules::makeZoomEngineInstanceToken();
  EXPECT_NE(a, b);
  EXPECT_NE(a.find('-'), std::string::npos);
  EXPECT_FALSE(a.empty());
}
