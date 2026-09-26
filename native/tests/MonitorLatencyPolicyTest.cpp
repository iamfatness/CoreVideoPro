#include "modules/MonitorLatencyPolicy.h"

#include <gtest/gtest.h>

TEST(MonitorLatencyPolicy, KeepsTheMonitorInsideTheMeasuredLiveSyncBudget) {
  using corevideo::modules::MonitorLatencyPolicy;
  EXPECT_EQ(MonitorLatencyPolicy::endpointBuffer100ns(), 0);
  EXPECT_EQ(MonitorLatencyPolicy::ringTargetFrames(48000), 960u);
  EXPECT_EQ(MonitorLatencyPolicy::ringTargetFrames(44100), 882u);
  EXPECT_LE(MonitorLatencyPolicy::endpointBuffer100ns() / 10'000 +
                1000 * MonitorLatencyPolicy::ringTargetFrames(48000) / 48000,
            20);
  EXPECT_EQ(MonitorLatencyPolicy::sharedPeriodFrames(48000, 480, 480, 480), 480u);
  EXPECT_EQ(MonitorLatencyPolicy::sharedPeriodFrames(48000, 48, 96, 960), 480u);
  EXPECT_EQ(MonitorLatencyPolicy::sharedPeriodFrames(48000, 480, 960, 1920), 960u);
  EXPECT_EQ(MonitorLatencyPolicy::sharedPeriodFrames(48000, 0, 0, 0), 0u);
}
