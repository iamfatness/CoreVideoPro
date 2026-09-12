// #440 / T3.2. A browser source whose host died reported "browser host exited
// (code 3)" and showed a stalled tile. Exit 3 has one common cause on a machine
// we have never seen — no WebView2 Runtime — and that is the one thing a tester
// can actually act on.

#include "modules/BrowserHostExitMessage.h"

#include <gtest/gtest.h>

using corevideo::modules::browserHostExitMessage;

TEST(BrowserHostExitMessage, TheMissingRuntimeSaysWhatToInstall) {
  const auto message = browserHostExitMessage(3);
  EXPECT_NE(message.find("WebView2"), std::string::npos) << message;
  // It must carry the action, not just the diagnosis.
  EXPECT_NE(message.find("Install"), std::string::npos) << message;
  EXPECT_NE(message.find("developer.microsoft.com"), std::string::npos) << message;
  // And must not leak the raw code, which is what it replaces.
  EXPECT_EQ(message.find("code 3"), std::string::npos) << message;
}

// An unknown code keeps its number. Flattening every other death into a generic
// sentence would recreate the silence this replaces — the same rule the Zoom
// join reasons follow.
TEST(BrowserHostExitMessage, AnUnknownExitKeepsItsNumber) {
  for (const int code : {0, 1, 70, -1073741819}) {
    const auto message = browserHostExitMessage(code);
    EXPECT_NE(message.find(std::to_string(code)), std::string::npos) << message;
    EXPECT_EQ(message.find("WebView2"), std::string::npos) << message;
  }
}
