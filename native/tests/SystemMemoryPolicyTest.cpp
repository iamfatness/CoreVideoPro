#include "core/SystemMemoryPolicy.h"

#include <gtest/gtest.h>

namespace {
using namespace corevideo::core;

constexpr std::uint64_t kGiB = 1024ull * 1024 * 1024;
constexpr std::uint64_t kMiB = 1024ull * 1024;

SystemMemorySample available(std::uint64_t bytes) {
  return SystemMemorySample{true, bytes, 128 * kGiB};
}

TEST(SystemMemoryPolicy, AnUnmeasuredSampleIsUnknownNeverOk) {
  // Absent evidence is not good news: a platform that cannot measure must not read healthy.
  EXPECT_EQ(classifySystemMemory(SystemMemorySample{}), SystemMemoryLevel::Unknown);
}

TEST(SystemMemoryPolicy, PlentyOfCommitIsOk) {
  EXPECT_EQ(classifySystemMemory(available(60 * kGiB)), SystemMemoryLevel::Ok);
}

TEST(SystemMemoryPolicy, LessThanOneMoreCoresWorthIsLow) {
  EXPECT_EQ(classifySystemMemory(available(4 * kGiB - kMiB)), SystemMemoryLevel::Low);
  EXPECT_EQ(classifySystemMemory(available(4 * kGiB)), SystemMemoryLevel::Ok);
}

TEST(SystemMemoryPolicy, LessThanASmallShowsWorthIsCritical) {
  EXPECT_EQ(classifySystemMemory(available(kGiB - kMiB)), SystemMemoryLevel::Critical);
  EXPECT_EQ(classifySystemMemory(available(0)), SystemMemoryLevel::Critical);
}

// A machine hovering at the line must not flip the level (and log) on every sample.
TEST(SystemMemoryPolicy, LeavingALevelNeedsAMarginOverEnteringIt) {
  const auto justOverLow = available(4 * kGiB + 100 * kMiB);
  EXPECT_EQ(classifySystemMemory(justOverLow, SystemMemoryLevel::Ok), SystemMemoryLevel::Ok);
  EXPECT_EQ(classifySystemMemory(justOverLow, SystemMemoryLevel::Low), SystemMemoryLevel::Low);
  EXPECT_EQ(classifySystemMemory(available(4 * kGiB + 600 * kMiB), SystemMemoryLevel::Low), SystemMemoryLevel::Ok);

  const auto justOverCritical = available(kGiB + 100 * kMiB);
  EXPECT_EQ(classifySystemMemory(justOverCritical, SystemMemoryLevel::Low), SystemMemoryLevel::Low);
  EXPECT_EQ(classifySystemMemory(justOverCritical, SystemMemoryLevel::Critical), SystemMemoryLevel::Critical);
  EXPECT_EQ(classifySystemMemory(available(kGiB + 600 * kMiB), SystemMemoryLevel::Critical), SystemMemoryLevel::Low);
}

TEST(SystemMemoryPolicy, TheSamplerNeverReportsMoreAvailableThanTheLimit) {
  const auto sample = sampleSystemMemory();
  if (!sample.measured) return;  // not a Windows build
  EXPECT_GT(sample.commitLimitBytes, 0u);
  EXPECT_LE(sample.commitAvailableBytes, sample.commitLimitBytes);
}

}  // namespace
