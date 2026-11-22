// R3 (Sources Phase 2 plan, Task 8; comms spec C2): the five subscription-evidence lines the
// Mac engine writes are pinned to the core's parser. A rename on either side used to go
// unnoticed: main-macos.mm wrote them inline and nothing fed them through the parser.
// Each builder's line goes through parseZoomEngineEvent and ZoomSubscriptionSdkEvidence::observe,
// and the retained row must carry the fixture's numbers.
#include "SubscriptionEvidenceLines.h"
#include "modules/ZoomEngineClient.h"
#include "modules/ZoomSubscriptionSdkEvidence.h"

#include <gtest/gtest.h>

#include <string>

namespace {
struct Retained {
  bool ok = false;
  corevideo::rpc::Json row;
};

Retained retain(const std::string& line) {
  corevideo::modules::ZoomSubscriptionSdkEvidence evidence;
  const auto event = corevideo::modules::parseZoomEngineEvent(line);
  if (!event) return {};
  evidence.observe(*event, 7);
  const auto json = evidence.json();
  const auto& events = json.get("events")->asArray();
  if (events.size() != 1) return {};
  return {true, events.front()};
}
}  // namespace

TEST(SubscriptionEvidenceLines, EveryMacLineIsRetained) {
  namespace lines = corevideo::engine::evidence;
  struct Case {
    const char* stage;
    std::string line;
    int code, resolution, requested, actual;  // -1 = field absent
  };
  const Case cases[] = {
      {"create_renderer_failed", lines::createRendererFailed("cam-42", 42, 7, 2), 7, 2, -1, -1},
      {"set_resolution", lines::setResolution("cam-42", 42, 0, 1), 0, 1, -1, -1},
      {"video_subscribe", lines::videoSubscribe("cam-42", 42, 3, 2), 3, 2, -1, -1},
      {"video_resolution_downgraded", lines::resolutionDowngraded("cam-42", 42, 2, 1), -1, -1, 2, 1},
      {"video_subscribe_failed_all", lines::subscribeFailedAll("cam-42", 42, 2), -1, -1, 2, -1},
  };
  for (const auto& c : cases) {
    const auto retained = retain(c.line);
    ASSERT_TRUE(retained.ok) << c.stage << " was not retained: " << c.line;
    EXPECT_EQ(retained.row.getString("stage"), c.stage) << c.line;
    EXPECT_EQ(retained.row.getString("participantId"), "42") << c.line;
    EXPECT_EQ(retained.row.getString("sourceUuid"), "cam-42") << c.line;
    const auto number = [&](const char* key) {
      return retained.row.get(key) ? static_cast<int>(retained.row.getNumber(key)) : -1;
    };
    EXPECT_EQ(number("sdkCode"), c.code) << c.stage;
    EXPECT_EQ(number("resolution"), c.resolution) << c.stage;
    EXPECT_EQ(number("requestedResolution"), c.requested) << c.stage;
    EXPECT_EQ(number("actualResolution"), c.actual) << c.stage;
  }
}

TEST(SubscriptionEvidenceLines, ThePlatformIsReported) {
  corevideo::modules::ZoomSubscriptionSdkEvidence evidence;
#if defined(__APPLE__)
  const char* expected = "macos";
#elif defined(_WIN32)
  const char* expected = "windows";
#else
  const char* expected = "linux";
#endif
  // sdkCode is a ZoomSDKError on macOS and an SDKError on Windows; the number spaces differ.
  EXPECT_EQ(evidence.json().getString("platform"), expected);
}
