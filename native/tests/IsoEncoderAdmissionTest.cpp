// Admit-or-refuse policy for ISO encoder placement, plus the pure half of the
// capacity probe (the software budget). No GPU, no Media Foundation, no clock —
// the whole decision is exercised here, which is the point: the case that
// matters most (an over-subscribed laptop) is one nobody has on their desk.

#include "modules/EncoderCapacityProbe.h"
#include "modules/IsoEncoderAdmission.h"
#include "modules/IsoEncoderPlacement.h"

#include <gtest/gtest.h>

#include <string>
#include <vector>

using corevideo::modules::assumedIsoEncoderCapacity;
using corevideo::modules::EncoderProbeKey;
using corevideo::modules::EncoderProbeStatus;
using corevideo::modules::evaluateIsoAdmission;
using corevideo::modules::IsoAdmissionDecision;
using corevideo::modules::IsoCapacitySource;
using corevideo::modules::IsoEncoderCapacity;
using corevideo::modules::IsoEncoderMode;
using corevideo::modules::planIsoEncoders;
using corevideo::modules::ProbedEncoderCapacity;
using corevideo::modules::softwareSessionBudget;
using corevideo::modules::toIsoEncoderCapacity;

namespace {

std::vector<std::string> sources(int count) {
  std::vector<std::string> result;
  for (int index = 0; index < count; ++index) {
    result.push_back("zoom:" + std::to_string(index + 1));
  }
  return result;
}

IsoCapacitySource probedSource(std::string adapter = "NVIDIA GeForce RTX 4090") {
  IsoCapacitySource source;
  source.probed = true;
  source.status = "ready";
  source.adapterDescription = std::move(adapter);
  source.workload = "h264 1920x1080@30";
  return source;
}

IsoCapacitySource unprobedSource(std::string status = "pending") {
  IsoCapacitySource source;
  source.probed = false;
  source.status = std::move(status);
  source.workload = "h264 1920x1080@30";
  return source;
}

ProbedEncoderCapacity ready(int ceiling, bool software, unsigned cpus, EncoderProbeKey key) {
  ProbedEncoderCapacity probe;
  probe.status = EncoderProbeStatus::Ready;
  probe.probed = true;
  probe.hardwareAvailable = ceiling > 0;
  probe.hardwareSessionCeiling = ceiling;
  probe.softwareAvailable = software;
  probe.logicalProcessors = cpus;
  probe.key = key;
  return probe;
}

}  // namespace

// --- the software budget (the term that makes a refusal possible at all) -----

TEST(EncoderCapacityProbe, SoftwareBudgetScalesDownWithPixelRate) {
  // A 16-thread box: four 1080p30 software stems, one at 1080p60, none at 4K60.
  EXPECT_EQ(softwareSessionBudget(16, 1920, 1080, 30), 4);
  EXPECT_EQ(softwareSessionBudget(16, 1920, 1080, 60), 2);
  EXPECT_EQ(softwareSessionBudget(16, 3840, 2160, 60), 0);
}

TEST(EncoderCapacityProbe, SoftwareBudgetIsUnknownWhenTheCpuCountIsUnknown) {
  // Unknown must never become a number: -1 is what stops a refusal downstream.
  EXPECT_EQ(softwareSessionBudget(0, 1920, 1080, 30), -1);
}

TEST(EncoderCapacityProbe, AnUnreadyProbeYieldsExactlyTheShippedAssumption) {
  // The whole safety story for a machine we cannot interrogate: it gets the old
  // numbers, unchanged, and an unbounded software budget so nothing is refused.
  ProbedEncoderCapacity pending;
  pending.status = EncoderProbeStatus::Pending;
  const auto capacity = toIsoEncoderCapacity(pending, 1);
  const auto assumed = assumedIsoEncoderCapacity(1);
  EXPECT_EQ(capacity.hardwareSessionLimit, assumed.hardwareSessionLimit);
  EXPECT_EQ(capacity.hardwareSessionLimit, 8);
  EXPECT_TRUE(capacity.hardwareAvailable);
  EXPECT_TRUE(capacity.softwareAvailable);
  EXPECT_EQ(capacity.softwareSessionLimit, -1);
}

TEST(EncoderCapacityProbe, AProbedZeroCeilingReportsHardwareUnavailable) {
  // An integrated GPU that cannot take this codec/size/rate at all. "Hardware
  // exists but zero sessions" must not read as available.
  const auto capacity =
      toIsoEncoderCapacity(ready(0, true, 8, EncoderProbeKey{"h264", 3840, 2160, 60}), 1);
  EXPECT_FALSE(capacity.hardwareAvailable);
  EXPECT_EQ(capacity.hardwareSessionLimit, 0);
  EXPECT_EQ(capacity.softwareSessionLimit, 0);  // 8 threads cannot software-encode 4K60
}

// --- admission ---------------------------------------------------------------

TEST(IsoEncoderAdmission, EverythingOnHardwareArmsSilently) {
  const auto capacity =
      toIsoEncoderCapacity(ready(8, true, 32, EncoderProbeKey{"h264", 1920, 1080, 30}), 1);
  const auto plan = planIsoEncoders(sources(4), IsoEncoderMode::Auto, capacity);
  const auto verdict = evaluateIsoAdmission(plan, IsoEncoderMode::Auto, capacity, probedSource());

  EXPECT_EQ(verdict.decision, IsoAdmissionDecision::Admit);
  EXPECT_EQ(verdict.hardwareCount, 4);
  EXPECT_EQ(verdict.softwareCount, 0);
  EXPECT_TRUE(verdict.message.empty());
  EXPECT_TRUE(verdict.armIso());
}

// THE CASE THIS WORK EXISTS FOR. Two hardware sessions, Program owns one, eight
// ISO sources selected: seven would spill to a CPU that can carry two. Before
// this change the literal said "8 sessions" and every one of those eight armed
// on hardware that does not exist.
TEST(IsoEncoderAdmission, AnOverSubscribedLaptopIsRefusedInsteadOfSpillingSilently) {
  const auto capacity =
      toIsoEncoderCapacity(ready(2, true, 8, EncoderProbeKey{"h264", 1920, 1080, 30}), 1);
  ASSERT_EQ(capacity.hardwareSessionLimit, 2);
  ASSERT_EQ(capacity.softwareSessionLimit, 2);

  const auto plan = planIsoEncoders(sources(8), IsoEncoderMode::Auto, capacity);
  const auto verdict = evaluateIsoAdmission(plan, IsoEncoderMode::Auto, capacity, probedSource("Intel UHD Graphics"));

  EXPECT_EQ(verdict.hardwareCount, 1);  // 2 sessions - 1 for Program
  EXPECT_EQ(verdict.softwareCount, 7);
  EXPECT_EQ(verdict.decision, IsoAdmissionDecision::RefuseIso);
  EXPECT_FALSE(verdict.armIso());
  EXPECT_EQ(verdict.code, "iso-encoder-oversubscribed");
  // The refusal must be ACTIONABLE: 1 hardware + 2 software.
  EXPECT_EQ(verdict.admissibleIsoCount, 3);
  EXPECT_NE(verdict.message.find("about 3 ISO sources"), std::string::npos);
  EXPECT_NE(verdict.message.find("Intel UHD Graphics"), std::string::npos);
  EXPECT_NE(verdict.message.find("Program is still recording"), std::string::npos);
}

TEST(IsoEncoderAdmission, TheSameLaptopAdmitsTheSelectionItCanActuallyCarry) {
  const auto capacity =
      toIsoEncoderCapacity(ready(2, true, 8, EncoderProbeKey{"h264", 1920, 1080, 30}), 1);
  const auto plan = planIsoEncoders(sources(3), IsoEncoderMode::Auto, capacity);
  const auto verdict = evaluateIsoAdmission(plan, IsoEncoderMode::Auto, capacity, probedSource());

  EXPECT_EQ(verdict.decision, IsoAdmissionDecision::AdmitWithWarning);
  EXPECT_TRUE(verdict.armIso());
  EXPECT_EQ(verdict.softwareCount, 2);
  EXPECT_EQ(verdict.code, "iso-encoder-software-spill");
  // A ceiling is never reported as a promise.
  EXPECT_NE(verdict.message.find("not a guarantee"), std::string::npos);
}

// The tester-protection rule: an ASSUMPTION may warn, never refuse.
TEST(IsoEncoderAdmission, AnUnprobedMachineIsNeverRefusedHoweverBigTheSpill) {
  const auto capacity = assumedIsoEncoderCapacity(1);
  const auto plan = planIsoEncoders(sources(12), IsoEncoderMode::Auto, capacity);
  const auto verdict = evaluateIsoAdmission(plan, IsoEncoderMode::Auto, capacity, unprobedSource("failed"));

  EXPECT_EQ(verdict.softwareCount, 5);  // 12 sources, 7 hardware slots
  EXPECT_EQ(verdict.decision, IsoAdmissionDecision::AdmitWithWarning);
  EXPECT_TRUE(verdict.armIso());
  EXPECT_EQ(verdict.code, "iso-encoder-software-spill-unprobed");
  EXPECT_NE(verdict.message.find("could not be measured"), std::string::npos);
  EXPECT_EQ(verdict.admissibleIsoCount, -1);  // unknown, and reported as unknown
}

TEST(IsoEncoderAdmission, NoHardwareEncoderAtAllSaysSoInPlainWords) {
  // Probed, hardware genuinely absent for this workload, but the CPU can carry
  // the one stem selected — so it arms, loudly, rather than being refused.
  const auto capacity =
      toIsoEncoderCapacity(ready(0, true, 16, EncoderProbeKey{"h264", 1920, 1080, 30}), 1);
  const auto plan = planIsoEncoders(sources(1), IsoEncoderMode::Auto, capacity);
  const auto verdict = evaluateIsoAdmission(plan, IsoEncoderMode::Auto, capacity, probedSource("Intel UHD Graphics"));

  EXPECT_EQ(verdict.decision, IsoAdmissionDecision::AdmitWithWarning);
  EXPECT_EQ(verdict.softwareCount, 1);
  EXPECT_NE(verdict.message.find("no hardware encoder is available"), std::string::npos);
}

TEST(IsoEncoderAdmission, UnplaceableTracksRefuseIsoRatherThanWriteDeadStems) {
  // Explicit Hardware with no room left: planIsoEncoders reports Unavailable and
  // those writers would produce nothing. Refuse instead.
  IsoEncoderCapacity capacity;
  capacity.hardwareSessionLimit = 2;
  capacity.reservedHardwareSessions = 1;
  capacity.hardwareAvailable = true;
  capacity.softwareAvailable = true;
  capacity.softwareSessionLimit = 4;

  const auto plan = planIsoEncoders(sources(3), IsoEncoderMode::Hardware, capacity);
  const auto verdict = evaluateIsoAdmission(plan, IsoEncoderMode::Hardware, capacity, probedSource());

  EXPECT_EQ(verdict.unplacedCount, 2);
  EXPECT_EQ(verdict.decision, IsoAdmissionDecision::RefuseIso);
  EXPECT_EQ(verdict.code, "iso-encoder-unplaceable");
}

TEST(IsoEncoderAdmission, AnEmptySelectionIsAdmittedWithoutComment) {
  const auto capacity = assumedIsoEncoderCapacity(1);
  const auto plan = planIsoEncoders({}, IsoEncoderMode::Auto, capacity);
  const auto verdict = evaluateIsoAdmission(plan, IsoEncoderMode::Auto, capacity, probedSource());

  EXPECT_EQ(verdict.decision, IsoAdmissionDecision::Admit);
  EXPECT_TRUE(verdict.message.empty());
}

TEST(IsoEncoderAdmission, ExplicitSoftwareIsOperatorIntentAndStillGetsBudgetChecked) {
  // The operator chose software; we still refuse a selection the CPU cannot run.
  const auto capacity =
      toIsoEncoderCapacity(ready(8, true, 8, EncoderProbeKey{"h264", 1920, 1080, 30}), 1);
  ASSERT_EQ(capacity.softwareSessionLimit, 2);

  const auto plan = planIsoEncoders(sources(6), IsoEncoderMode::Software, capacity);
  const auto verdict = evaluateIsoAdmission(plan, IsoEncoderMode::Software, capacity, probedSource());

  EXPECT_EQ(verdict.softwareCount, 6);
  EXPECT_EQ(verdict.decision, IsoAdmissionDecision::RefuseIso);
  EXPECT_EQ(verdict.code, "iso-encoder-oversubscribed");
}
