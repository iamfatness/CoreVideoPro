#include "modules/IsoEncoderPlacement.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <string>
#include <vector>

using corevideo::modules::IsoEncoderCapacity;
using corevideo::modules::IsoEncoderMode;
using corevideo::modules::IsoEncoderPath;
using corevideo::modules::planIsoEncoders;

namespace {

std::vector<std::string> sources(int count) {
  std::vector<std::string> result;
  for (int index = 0; index < count; ++index) {
    result.push_back("zoom:" + std::to_string(index + 1));
  }
  return result;
}

int countPath(const std::vector<corevideo::modules::IsoEncoderAssignment>& plan,
              IsoEncoderPath path) {
  return static_cast<int>(std::count_if(plan.begin(), plan.end(), [path](const auto& item) {
    return item.path == path;
  }));
}

}  // namespace

TEST(IsoEncoderPlacement, AutoSpillsOverflowToSoftwareWithoutOversubscribingHardware) {
  const auto plan = planIsoEncoders(
      sources(8), IsoEncoderMode::Auto,
      IsoEncoderCapacity{.hardwareSessionLimit = 8,
                         .reservedHardwareSessions = 1,
                         .hardwareAvailable = true,
                         .softwareAvailable = true});

  EXPECT_EQ(countPath(plan, IsoEncoderPath::Hardware), 7);
  EXPECT_EQ(countPath(plan, IsoEncoderPath::Software), 1);
  ASSERT_EQ(plan.size(), 8u);
  EXPECT_EQ(plan.back().reason, "hardware-capacity-exhausted");
}

TEST(IsoEncoderPlacement, AutoUsesSoftwareWhenHardwareIsUnavailable) {
  const auto plan = planIsoEncoders(
      sources(3), IsoEncoderMode::Auto,
      IsoEncoderCapacity{.hardwareSessionLimit = 8,
                         .hardwareAvailable = false,
                         .softwareAvailable = true});

  EXPECT_EQ(countPath(plan, IsoEncoderPath::Hardware), 0);
  EXPECT_EQ(countPath(plan, IsoEncoderPath::Software), 3);
  EXPECT_TRUE(std::all_of(plan.begin(), plan.end(), [](const auto& item) {
    return item.reason == "hardware-unavailable";
  }));
}

TEST(IsoEncoderPlacement, ExplicitHardwareDoesNotHideCapacityFailure) {
  const auto plan = planIsoEncoders(
      sources(4), IsoEncoderMode::Hardware,
      IsoEncoderCapacity{.hardwareSessionLimit = 3,
                         .reservedHardwareSessions = 1,
                         .hardwareAvailable = true,
                         .softwareAvailable = true});

  EXPECT_EQ(countPath(plan, IsoEncoderPath::Hardware), 2);
  EXPECT_EQ(countPath(plan, IsoEncoderPath::Software), 0);
  EXPECT_EQ(countPath(plan, IsoEncoderPath::Unavailable), 2);
}

TEST(IsoEncoderPlacement, ExplicitSoftwareNeverConsumesHardwareBudget) {
  const auto plan = planIsoEncoders(
      sources(4), IsoEncoderMode::Software,
      IsoEncoderCapacity{.hardwareSessionLimit = 8,
                         .hardwareAvailable = true,
                         .softwareAvailable = true});

  EXPECT_EQ(countPath(plan, IsoEncoderPath::Hardware), 0);
  EXPECT_EQ(countPath(plan, IsoEncoderPath::Software), 4);
  EXPECT_TRUE(std::all_of(plan.begin(), plan.end(), [](const auto& item) {
    return item.reason == "operator-selected";
  }));
}

TEST(IsoEncoderPlacement, AutoReportsUnavailableWhenNoFallbackExists) {
  const auto plan = planIsoEncoders(
      sources(2), IsoEncoderMode::Auto,
      IsoEncoderCapacity{.hardwareSessionLimit = 1,
                         .reservedHardwareSessions = 1,
                         .hardwareAvailable = true,
                         .softwareAvailable = false});

  EXPECT_EQ(countPath(plan, IsoEncoderPath::Unavailable), 2);
  EXPECT_TRUE(std::all_of(plan.begin(), plan.end(), [](const auto& item) {
    return item.reason == "hardware-capacity-exhausted";
  }));
}

TEST(IsoEncoderPlacement, ReservationsAreClampedAtTheHardwareLimit) {
  const auto plan = planIsoEncoders(
      sources(1), IsoEncoderMode::Auto,
      IsoEncoderCapacity{.hardwareSessionLimit = 4,
                         .reservedHardwareSessions = 99,
                         .hardwareAvailable = true,
                         .softwareAvailable = true});

  ASSERT_EQ(plan.size(), 1u);
  EXPECT_EQ(plan[0].path, IsoEncoderPath::Software);
}
