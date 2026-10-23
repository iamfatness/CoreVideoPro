#include "modules/EncoderCapacityProbe.h"

#include <gtest/gtest.h>

// The probe cache is keyed by codec. One canonical spelling per codec, or the
// same workload is probed twice under two names and the sender's "no free
// session" answer depends on which spelling asked.
TEST(EncoderCapacityProbePolicy, CanonicalProbeCodecCollapsesSpellings) {
  using corevideo::modules::canonicalProbeCodec;
  EXPECT_EQ(canonicalProbeCodec("h264"), "h264");
  EXPECT_EQ(canonicalProbeCodec("H264"), "h264");
  EXPECT_EQ(canonicalProbeCodec("h265"), "hevc");
  EXPECT_EQ(canonicalProbeCodec("hevc"), "hevc");
  EXPECT_EQ(canonicalProbeCodec("hvc1"), "hevc");
  EXPECT_EQ(canonicalProbeCodec("av1"), "av1");
  EXPECT_EQ(canonicalProbeCodec("av01"), "av1");
  EXPECT_EQ(canonicalProbeCodec(""), "h264");
  EXPECT_EQ(canonicalProbeCodec("bogus"), "h264");
}

TEST(EncoderCapacityProbePolicy, ProbeKeyOrdersByCodecFirst) {
  corevideo::modules::EncoderProbeKey a{"av1", 1920, 1080, 60};
  corevideo::modules::EncoderProbeKey b{"h264", 1920, 1080, 60};
  EXPECT_TRUE(a < b);
  EXPECT_FALSE(b < a);
}

namespace {
std::vector<std::string> fullIsoSelection() {
  std::vector<std::string> sources;
  for (int index = 0; index < 8; ++index) sources.push_back("zoom:" + std::to_string(index + 1));
  return sources;
}
corevideo::modules::ProbedEncoderCapacity measuredSessions(int count) {
  corevideo::modules::ProbedEncoderCapacity probe;
  probe.status = corevideo::modules::EncoderProbeStatus::Ready;
  probe.probed = true; probe.hardwareAvailable = true; probe.softwareAvailable = true;
  probe.hardwareSessionCeiling = count;
  probe.probeSessionCap = corevideo::modules::kDefaultEncoderProbeSessionCap;
  probe.logicalProcessors = 28;
  return probe;
}
}

TEST(EncoderCapacityProbePolicy, DefaultProbeCanDiscoverProgramAndEightHardwareIsos) {
  using namespace corevideo::modules;
  const auto plan = planIsoEncoders(fullIsoSelection(), IsoEncoderMode::Auto,
      toIsoEncoderCapacity(measuredSessions(kDefaultEncoderProbeSessionCap), 1));
  ASSERT_EQ(plan.size(), 8u);
  for (const auto& track : plan) EXPECT_EQ(track.path, IsoEncoderPath::Hardware) << track.sourceId;
}

TEST(EncoderCapacityProbePolicy, MeasuredEightSessionDriverStillSpillsAfterProgramReservation) {
  using namespace corevideo::modules;
  const auto plan = planIsoEncoders(fullIsoSelection(), IsoEncoderMode::Auto,
      toIsoEncoderCapacity(measuredSessions(8), 1));
  ASSERT_EQ(plan.size(), 8u);
  for (size_t index = 0; index < 7; ++index) EXPECT_EQ(plan[index].path, IsoEncoderPath::Hardware);
  EXPECT_EQ(plan.back().path, IsoEncoderPath::Software);
  EXPECT_EQ(plan.back().reason, "hardware-capacity-exhausted");
}

TEST(EncoderCapacityProbePolicy, InconclusiveProbeDoesNotGrantUnmeasuredHardwareSessions) {
  using namespace corevideo::modules;
  auto probe = measuredSessions(kDefaultEncoderProbeSessionCap);
  probe.probed = false; probe.status = EncoderProbeStatus::Failed;
  const auto capacity = toIsoEncoderCapacity(probe, 1);
  EXPECT_EQ(capacity.hardwareSessionLimit, 8);
  EXPECT_EQ(capacity.softwareSessionLimit, -1);
  const auto plan = planIsoEncoders(fullIsoSelection(), IsoEncoderMode::Auto, capacity);
  ASSERT_EQ(plan.size(), 8u);
  EXPECT_EQ(plan.back().path, IsoEncoderPath::Software);
}
