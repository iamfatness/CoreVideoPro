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
