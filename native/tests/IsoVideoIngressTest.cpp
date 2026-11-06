#include "core/IsoVideoIngress.h"
#include "core/IsoDeparture.h"
#include <gtest/gtest.h>
#include <thread>
#include <future>

namespace {
corevideo::modules::VideoFrame picture(const std::string& id, int64_t sequence, uint64_t epoch = 1) {
  corevideo::modules::VideoFrame frame;
  frame.participantId = id; frame.frameId = sequence; frame.sourceEpoch = epoch;
  frame.width = frame.pixelWidth = frame.height = frame.pixelHeight = 2;
  frame.pixelStride = 8;
  frame.pixels = std::make_shared<const std::vector<uint8_t>>(16, static_cast<uint8_t>(sequence));
  frame.captureTimestamp100ns = sequence * 166667;
  return frame;
}
}

TEST(IsoVideoIngress, CpuBatchKeepsItsOwnIdentitiesAndCaptureTimes) {
  corevideo::core::IsoVideoIngress ingress;
  auto newer = picture("capture:screen", 100);
  std::vector<corevideo::modules::VideoFrame> converted{picture("capture:screen", 10), picture("capture:screen", 11)};
  ingress.observe({"capture:screen"}, {newer}, converted, 999999999);
  const auto result = ingress.drain(std::chrono::milliseconds(0));
  ASSERT_EQ(result.size(), 2u);
  EXPECT_EQ(result[0].frame.frameId, 10); EXPECT_EQ(result[1].frame.frameId, 11);
  EXPECT_EQ(result[0].timelineTimestamp100ns, converted[0].captureTimestamp100ns);
  EXPECT_EQ(result[1].timelineTimestamp100ns, converted[1].captureTimestamp100ns);
}

TEST(IsoVideoIngress, ProductionObservationSeparatesSelectionAndLeafLockFromAdmission) {
  using corevideo::core::RenderTickPhases;
  using Phase = RenderTickPhases::Phase;
  corevideo::core::IsoVideoIngress ingress;
  RenderTickPhases phases;
  phases.begin();
  RenderTickPhases::Scope scope(phases);
  ingress.observe({"42"}, {}, {picture("42", 1)}, 1, true);
  RenderTickPhases::markCurrent(Phase::IsoIngress);
  EXPECT_TRUE(phases.visited(Phase::IsoSelection));
  EXPECT_TRUE(phases.visited(Phase::IsoLockWait));
  EXPECT_TRUE(phases.visited(Phase::IsoIngress));
  EXPECT_TRUE(phases.visited(Phase::IsoLatest));
  EXPECT_TRUE(phases.visited(Phase::IsoQueue));
  EXPECT_EQ(ingress.drain(std::chrono::milliseconds(0)).size(), 1u);
  phases.begin();
  ingress.clear();
  RenderTickPhases::markCurrent(Phase::IsoIngress);
  EXPECT_TRUE(phases.visited(Phase::IsoLockWait));
  EXPECT_TRUE(phases.visited(Phase::IsoIngress));
  EXPECT_FALSE(phases.visited(Phase::IsoSelection));
}

TEST(IsoVideoIngress, IndependentZoomNeverFallsBackToAnOlderRenderPictureBetweenArrivalBatches) {
  corevideo::core::IsoVideoIngress ingress;
  auto delayedRender = picture("42", 2);
  ingress.observe({"zoom:42"}, {delayedRender}, {picture("42", 10), picture("42", 12)}, 1, true);
  auto first = ingress.drain(std::chrono::milliseconds(0));
  ASSERT_EQ(first.size(), 2u);
  EXPECT_EQ(first[0].frame.frameId, 10); EXPECT_EQ(first[1].frame.frameId, 12);
  ingress.observe({"zoom:42"}, {delayedRender}, {}, 2, true);
  EXPECT_TRUE(ingress.drain(std::chrono::milliseconds(0)).empty());
  ingress.observe({"zoom:42"}, {delayedRender}, {picture("42", 14)}, 3, true);
  const auto next = ingress.drain(std::chrono::milliseconds(0));
  ASSERT_EQ(next.size(), 1u); EXPECT_EQ(next[0].frame.frameId, 14);
}

TEST(IsoVideoIngress, BoundIsPerSourceAndCountsEveryEviction) {
  corevideo::core::IsoVideoIngress ingress;
  std::vector<corevideo::modules::VideoFrame> converted;
  for (int id = 1; id <= 6; ++id) converted.push_back(picture("capture:fast", id));
  converted.push_back(picture("zoom:slow", 1));
  ingress.observe({"capture:fast", "slow"}, {}, converted, 1);
  const auto result = ingress.drain(std::chrono::milliseconds(0));
  ASSERT_EQ(result.size(), 5u);
  EXPECT_EQ(result.front().frame.frameId, 3);
  EXPECT_EQ(result.back().sourceId, "zoom:slow");
  const auto counters = ingress.counters();
  EXPECT_EQ(counters.at("capture:fast").distinctSubmitted, 6u);
  EXPECT_EQ(counters.at("capture:fast").queueOverflowed, 2u);
}

TEST(IsoVideoIngress, DuplicateIsSuppressedButNewEpochIsDistinct) {
  corevideo::core::IsoVideoIngress ingress;
  auto frame = picture("capture:screen", 1);
  ingress.observe({"capture:screen"}, {frame}, {}, 1);
  ingress.observe({"capture:screen"}, {frame}, {}, 2);
  frame.sourceEpoch = 2;
  ingress.observe({"capture:screen"}, {frame}, {}, 3);
  EXPECT_EQ(ingress.drain(std::chrono::milliseconds(0)).size(), 2u);
  EXPECT_EQ(ingress.counters().at("capture:screen").duplicateRejected, 1u);
}

TEST(IsoVideoIngress, LegacyTimelineAndDirectPathRemainAvailable) {
  corevideo::core::IsoVideoIngress ingress;
  auto frame = picture("7", 1, 0); frame.captureTimestamp100ns = 0;
  ingress.observe({"7"}, {frame, picture("media:logo", 2)}, {}, 3456789);
  const auto result = ingress.drain(std::chrono::milliseconds(0));
  ASSERT_EQ(result.size(), 1u);
  EXPECT_EQ(result[0].sourceId, "zoom:7");
  EXPECT_EQ(result[0].timelineTimestamp100ns, 3456789);
  ASSERT_EQ(ingress.latest({"7"}).size(), 1u);
  ingress.clear();
  EXPECT_TRUE(ingress.latest({"7"}).empty()); EXPECT_TRUE(ingress.counters().empty());
  ingress.observe({"7"}, {frame}, {}, 999);
  EXPECT_EQ(ingress.drain(std::chrono::milliseconds(0)).size(), 1u);
}

TEST(IsoVideoIngress, HoldsCompletedCpuViewBetweenArrivalsAndReleasesDisconnectedSource) {
  corevideo::core::IsoVideoIngress ingress;
  auto cpu = picture("capture:screen", 10);
  auto gpuOnly = picture("capture:screen", 11); gpuOnly.pixels.reset();
  ingress.observe({"capture:screen"}, {gpuOnly}, {cpu}, 1);
  ingress.drain(std::chrono::milliseconds(0));
  ingress.observe({"capture:screen"}, {gpuOnly}, {}, 2);
  const auto held = ingress.latest({"capture:screen"});
  ASSERT_EQ(held.size(), 1u);
  EXPECT_EQ(held[0].frame.frameId, 10);
  EXPECT_TRUE(ingress.drain(std::chrono::milliseconds(0)).empty());
  ingress.observe({"capture:screen"}, {}, {}, 3);
  EXPECT_TRUE(ingress.latest({"capture:screen"}).empty());
}

// Investigation fixture for #1177: identify the owner that retires an ISO
// payload after the encoder has drained it. This is ownership evidence, not
// a claimed fix or a timing acceptance test.
TEST(IsoVideoIngress, RetainedPayloadRetiresDuringLatestReplacementOnTheCallingThread) {
  corevideo::core::IsoVideoIngress ingress;
  std::thread::id retiredOn;
  bool replacing = false, retiredDuringReplacement = false;
  auto old = picture("42", 1);
  old.pixels = std::shared_ptr<const std::vector<uint8_t>>(
      new std::vector<uint8_t>(16, 1), [&](const std::vector<uint8_t>* bytes) {
        retiredOn = std::this_thread::get_id();
        retiredDuringReplacement = replacing;
        delete bytes;
      });
  ingress.observe({"42"}, {}, {old}, 1, true);
  old.pixels.reset();
  ingress.drain(std::chrono::milliseconds(0)).clear();
  EXPECT_EQ(retiredOn, std::thread::id{});
  replacing = true;
  ingress.observe({"42"}, {}, {picture("42", 2)}, 2, true);
  replacing = false;
  EXPECT_EQ(retiredOn, std::this_thread::get_id());
  EXPECT_TRUE(retiredDuringReplacement);
}

TEST(IsoVideoIngress, WorkerRetirementCannotBlockTheNextProducerAdmission) {
  using Ingress = corevideo::core::IsoVideoIngress;
  Ingress ingress;
  std::promise<void> retired, release, drained, produced;
  auto retiredReady = retired.get_future();
  auto releaseReady = release.get_future().share();
  auto drainedReady = drained.get_future();
  auto producedReady = produced.get_future();
  std::thread::id retiredOn;
  auto frame = picture("42", 1);
  frame.pixels = std::shared_ptr<const std::vector<uint8_t>>(
      new std::vector<uint8_t>(16, 1), [&](const std::vector<uint8_t>* bytes) {
        retiredOn = std::this_thread::get_id();
        retired.set_value();
        releaseReady.wait();
        delete bytes;
      });
  std::vector<corevideo::modules::VideoFrame> arrivals;
  arrivals.push_back(std::move(frame));
  ingress.observe({"42"}, {}, std::move(arrivals), 1, true, Ingress::Delivery::Worker);
  std::thread consumer([&] {
    auto batch = ingress.drain(std::chrono::milliseconds(0));
    drained.set_value();
    batch.clear();
  });
  const auto consumerId = consumer.get_id();
  drainedReady.wait();
  std::thread producer([&] {
    ingress.observe({"42"}, {}, {picture("42", 2)}, 2, true, Ingress::Delivery::Worker);
    produced.set_value();
  });
  const bool retirementEntered = retiredReady.wait_for(std::chrono::seconds(2)) == std::future_status::ready;
  const bool producerIndependent = producedReady.wait_for(std::chrono::milliseconds(100)) == std::future_status::ready;
  // Release and join before assertions: the lightweight test runner throws
  // on failed expectations, and even the deliberately broken control must exit.
  release.set_value();
  producer.join(); consumer.join();
  EXPECT_TRUE(retirementEntered);
  EXPECT_TRUE(producerIndependent) << "last-owner retirement held the next render admission";
  EXPECT_EQ(retiredOn, consumerId);
  EXPECT_TRUE(ingress.latest({"42"}).empty()) << "live delivery must not retain an unused latest picture";
  const auto next = ingress.drain(std::chrono::milliseconds(0));
  ASSERT_EQ(next.size(), 1u);
  EXPECT_EQ(next[0].frame.frameId, 2);
}

TEST(IsoVideoIngress, WorkerHandoffPreservesOrderEpochBoundsAndClear) {
  using Ingress = corevideo::core::IsoVideoIngress;
  Ingress ingress;
  const auto observe = [&](std::vector<corevideo::modules::VideoFrame> frames) {
    ingress.observe({"42"}, {}, std::move(frames), 999, true, Ingress::Delivery::Worker);
  };
  observe({picture("42", 1), picture("42", 1), picture("42", 1, 2)});
  auto first = ingress.drain(std::chrono::milliseconds(0));
  ASSERT_EQ(first.size(), 2u);
  EXPECT_EQ(first[0].frame.sourceEpoch, 1u);
  EXPECT_EQ(first[1].frame.sourceEpoch, 2u);
  EXPECT_EQ(first[0].timelineTimestamp100ns, 166667);
  EXPECT_EQ(ingress.counters().at("zoom:42").duplicateRejected, 1u);
  observe({picture("42", 2, 2), picture("42", 3, 2), picture("42", 4, 2),
      picture("42", 5, 2), picture("42", 6, 2)});
  auto bounded = ingress.drain(std::chrono::milliseconds(0));
  ASSERT_EQ(bounded.size(), 4u);
  for (size_t i = 0; i < bounded.size(); ++i) EXPECT_EQ(bounded[i].frame.frameId, static_cast<int64_t>(i + 3));
  EXPECT_EQ(ingress.counters().at("zoom:42").queueOverflowed, 1u);
  ingress.observe({}, {}, {picture("42", 7, 2)}, 1000, true, Ingress::Delivery::Worker);
  EXPECT_TRUE(ingress.drain(std::chrono::milliseconds(0)).empty());
  observe({picture("42", 8, 2)});
  ingress.clear();
  EXPECT_TRUE(ingress.drain(std::chrono::milliseconds(0)).empty());
  EXPECT_TRUE(ingress.counters().empty());
  observe({picture("42", 1)});
  EXPECT_EQ(ingress.drain(std::chrono::milliseconds(0)).size(), 1u);
  EXPECT_TRUE(ingress.latest({"42"}).empty());
}

// #1030: the roster is the departure signal for Zoom ISO stems. Only a Zoom
// participant missing from a KNOWN roster has departed; capture/media sources
// never depart. An explicitly known empty roster means everyone left.
TEST(IsoDeparture, OnlyZoomSourcesMissingFromAKnownRosterHaveDeparted) {
  using corevideo::core::departedIsoSources;
  const std::vector<std::string> selected{"zoom:16778240", "zoom:33559552", "capture:cam-1", "media:clip"};
  const auto departed = departedIsoSources(selected, {"16778240", "99"});
  EXPECT_EQ(departed.size(), 1u);
  EXPECT_TRUE(departed.count("zoom:33559552"));
  EXPECT_EQ(departedIsoSources(selected, {}).size(), 2u);
  EXPECT_TRUE(departedIsoSources(selected, {"16778240", "33559552"}).empty());
}

TEST(IsoDeparture, UnavailableRosterCannotResurrectDepartedStems) {
  using Ids = std::unordered_set<std::string>;
  corevideo::core::IsoDepartureTracker tracker;
  const std::vector<std::string> selected{"zoom:1", "zoom:2", "capture:cam", "media:clip"};
  EXPECT_TRUE(tracker.observe(selected, std::nullopt).empty());
  EXPECT_TRUE(tracker.observe(selected, Ids{"1", "2"}).empty());
  EXPECT_EQ(tracker.observe(selected, Ids{"2"}), (Ids{"zoom:1"}));
  EXPECT_EQ(tracker.observe(selected, std::nullopt), (Ids{"zoom:1"}));
  EXPECT_EQ(tracker.observe(selected, Ids{}), (Ids{"zoom:1", "zoom:2"}));
  EXPECT_EQ(tracker.observe(selected, std::nullopt), (Ids{"zoom:1", "zoom:2"}));
  EXPECT_EQ(tracker.observe(selected, Ids{"1"}), (Ids{"zoom:2"}));
  EXPECT_TRUE(tracker.observe({"zoom:1"}, std::nullopt).empty());
  (void)tracker.observe(selected, Ids{});
  tracker.clear();
  EXPECT_TRUE(tracker.observe(selected, std::nullopt).empty());
}
