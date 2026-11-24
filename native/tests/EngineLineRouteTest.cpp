// The Mac engine's reader loop routes comms_* lines before engine dispatch,
// matching engine-comms.cpp's order on Windows (engine-line-route.h).
#include <gtest/gtest.h>

#include "engine-line-route.h"

TEST(EngineLineRoute, CommsLinesRouteFirstEvenWhenTheyNameAnEngineVerb) {
  const std::string comms = R"({"cmd":"comms_mic","device":"join quit leave"})";
  EXPECT_EQ(routeEngineLine(comms), EngineLineRoute::Comms);
  EXPECT_EQ(routeEngineLine(R"({"cmd":"join","meeting_id":"1"})"), EngineLineRoute::Engine);
  EXPECT_EQ(routeEngineLine(R"({"cmd":"comms"})"), EngineLineRoute::Engine);  // no comms_ prefix
  EXPECT_EQ(routeEngineLine(R"({"cmd":"quit"})"), EngineLineRoute::Engine);
}

// Confirmatory (rule 8): Mac dispatch is by exact cmd, so a comms line naming
// an engine verb in a value never parses as that verb. The route order above
// is kept for parity with Windows, not because Mac needs it.
TEST(EngineLineRoute, MacEngineDispatchIsByExactCommandName) {
  const std::string comms = R"({"cmd":"comms_mic","device":"join quit leave"})";
  EXPECT_EQ(engine_parse_command(comms), EngineCommand::Unknown);
}
