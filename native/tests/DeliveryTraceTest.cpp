#include "core/DeliveryTrace.h"
#include "core/DeliveryTraceAggregate.h"
#include "contracts/Lifecycle.h"
#include <gtest/gtest.h>
#include <atomic>
#include <filesystem>
#include <fstream>
#include <future>
#include <set>
#include <thread>
#include <vector>

namespace {
using namespace corevideo::core;
std::filesystem::path newPath() {
  static std::atomic<int> serial{0};
  return std::filesystem::temp_directory_path() / ("corevideo-trace-" +
      std::to_string(deliveryTraceNow()) + "-" + std::to_string(++serial) + ".bin");
}
struct Capture {
  std::filesystem::path path = newPath();
  DeliveryTraceCapture trace{path.string()};
  ~Capture() { trace.close(); std::error_code error; std::filesystem::remove(path, error); }
};
DeliveryTraceHeader readHeader(const std::filesystem::path& path) {
  DeliveryTraceHeader header;
  std::ifstream input(path, std::ios::binary);
  input.read(reinterpret_cast<char*>(&header), sizeof(header));
  EXPECT_TRUE(input.good()); return header;
}
}
TEST(DeliveryTrace, CompletedExportPreservesEveryAcceptedIdentityAndClock) {
  Capture capture;
  for (int i = 0; i < 1500; ++i) {
    DeliveryTraceEvent event; event.programSequence = i; event.sourceEpoch = 7;
    event.sourceFrameId = i * 2; event.sourceObservation100ns = 123 + i;
    event.layoutTag = 12; event.stage = DeliveryStage::ProgramDelivered;
    ASSERT_TRUE(capture.trace.record(event));
  }
  ASSERT_TRUE(capture.trace.close());
  const auto header = readHeader(capture.path);
  EXPECT_EQ(header.complete, 1); EXPECT_EQ(header.exported, 1500); EXPECT_EQ(header.lost, 0);
  EXPECT_GT(header.clockFrequency, 0); EXPECT_GT(header.sessionEpoch, 0);
  EXPECT_EQ(std::filesystem::file_size(capture.path), sizeof(header) + 1500 * sizeof(DeliveryTraceEvent));
  std::ifstream input(capture.path, std::ios::binary); input.seekg(sizeof(header));
  for (int i = 0; i < 1500; ++i) {
    DeliveryTraceEvent event; input.read(reinterpret_cast<char*>(&event), sizeof(event)); ASSERT_TRUE(input.good());
    EXPECT_EQ(event.programSequence, i); EXPECT_EQ(event.sourceFrameId, i * 2);
    EXPECT_EQ(event.sourceObservation100ns, 123 + i); EXPECT_EQ(event.sourceEpoch, 7);
    EXPECT_EQ(event.sessionEpoch, header.sessionEpoch); EXPECT_GE(event.timestamp, header.started);
    EXPECT_LE(event.timestamp, header.ended); EXPECT_EQ(event.layoutTag, 12);
  }
}
TEST(DeliveryTrace, ConcurrentWritersNeverTearAndAccountForEveryRefusal) {
  Capture capture; std::atomic<bool> go{false}; std::vector<std::thread> writers;
  for (int writer = 0; writer < 4; ++writer) writers.emplace_back([&, writer] {
    while (!go.load()) std::this_thread::yield();
    for (int i = 0; i < 10000; ++i) {
      DeliveryTraceEvent event; event.programSequence = writer * 10000 + i;
      event.sourceFrameId = event.programSequence ^ 0x5a5a;
      capture.trace.record(event);
    }
  });
  go.store(true); for (auto& writer : writers) writer.join();
  ASSERT_TRUE(capture.trace.close()); const auto header = readHeader(capture.path);
  EXPECT_EQ(header.exported + header.lost, 40000); EXPECT_EQ(header.failures, 0);
  std::ifstream input(capture.path, std::ios::binary); input.seekg(sizeof(header));
  std::set<std::int64_t> identities;
  for (std::uint64_t i = 0; i < header.exported; ++i) {
    DeliveryTraceEvent event; input.read(reinterpret_cast<char*>(&event), sizeof(event)); ASSERT_TRUE(input.good());
    EXPECT_EQ(event.sourceFrameId, event.programSequence ^ 0x5a5a);
    EXPECT_TRUE(identities.insert(event.programSequence).second);
  }
}
TEST(DeliveryTrace, RingReusesStorageBeyondOneFullGeneration) {
  Capture capture;
  for (int i = 0; i < 220000; ++i) {
    DeliveryTraceEvent event; event.programSequence = i;
    ASSERT_TRUE(capture.trace.record(event));
    if (i % 512 == 0) std::this_thread::sleep_for(std::chrono::milliseconds(2));
  }
  ASSERT_TRUE(capture.trace.close()); const auto header = readHeader(capture.path);
  EXPECT_EQ(header.exported, 220000); EXPECT_EQ(header.lost, 0);
  EXPECT_EQ(capture.trace.snapshot().getNumber("storageBytes"), 16 * 1024 * 1024);
}
TEST(DeliveryTrace, ExistingEvidenceIsPreservedAndFailureIsExplicit) {
  const auto path = newPath(); { std::ofstream output(path); output << "retained-control"; }
  DeliveryTraceCapture trace(path.string());
  EXPECT_FALSE(trace.close()); EXPECT_EQ(trace.snapshot().getNumber("exportFailures"), 1);
  std::ifstream input(path); std::string text; input >> text; EXPECT_EQ(text, "retained-control");
  std::error_code error; std::filesystem::remove(path, error);
}
TEST(DeliveryTrace, MissingParentCannotProduceCompletedEvidence) {
  DeliveryTraceCapture trace((newPath() / "missing" / "trace.bin").string());
  EXPECT_FALSE(trace.close()); EXPECT_EQ(trace.snapshot().getNumber("exportFailures"), 1);
}
TEST(DeliveryTrace, DisabledPeersRemainUnobservedAndStopRejectsNewEvents) {
  EXPECT_FALSE(deliveryEvidenceSnapshot().get("enabled")->asBool());
  EXPECT_EQ(deliveryEvidenceSnapshot().get("accepted"), nullptr);
  Capture capture; ASSERT_TRUE(capture.trace.close());
  EXPECT_FALSE(capture.trace.record({})); EXPECT_EQ(readHeader(capture.path).exported, 0);
}

TEST(DeliveryTrace, FullStorageRefusesWithoutWaitingAndRetainsEveryAcceptedEvent) {
  auto gate = std::make_shared<std::promise<void>>(); auto release = gate->get_future().share();
  const auto path = newPath();
  DeliveryTraceCapture trace(path.string(), [release] { release.wait(); });
  const auto capacity = static_cast<int>(trace.snapshot().getNumber("capacity"));
  for (int i = 0; i < capacity; ++i) { DeliveryTraceEvent event; event.programSequence = i; ASSERT_TRUE(trace.record(event)); }
  const auto start = std::chrono::steady_clock::now();
  EXPECT_FALSE(trace.record({}));
  EXPECT_LT(std::chrono::steady_clock::now() - start, std::chrono::milliseconds(50));
  gate->set_value(); ASSERT_TRUE(trace.close()); const auto header = readHeader(path);
  EXPECT_EQ(header.exported, capacity); EXPECT_EQ(header.lost, 1); EXPECT_EQ(header.complete, 1);
  std::error_code error; std::filesystem::remove(path, error);
}

TEST(DeliveryTrace, AggregateKeepsUnobservedStagesUnknownAndClockDomainsSeparate) {
  DeliveryTraceAggregate aggregate;
  auto empty = aggregate.snapshot(100000, 1000000, 0);
  ASSERT_EQ(empty.get("stages")->asArray().size(), 10u);
  const auto& unobserved = empty.get("stages")->asArray()[0];
  EXPECT_FALSE(unobserved.get("observed")->asBool()); EXPECT_EQ(unobserved.get("lastTicks"), nullptr);
  EXPECT_EQ(unobserved.get("lastProgressAgeTicks"), nullptr);
  DeliveryTraceEvent event; event.stage = DeliveryStage::SourceGpuReady;
  event.sourceTag = 42; event.sourceEpoch = 7; event.sourceFrameId = 18;
  event.timestamp = 100000; event.sourceObservation100ns = 9007199254740993LL;
  aggregate.observe(event, 1000000);
  event.timestamp += 16667; aggregate.observe(event, 1000000);
  event.timestamp += 61000000; aggregate.observe(event, 1000000);
  event.timestamp -= 1; aggregate.observe(event, 1000000); // Export order, not a pipeline reorder.
  const auto snapshot = aggregate.snapshot(event.timestamp + 1001, 1000000, 1);
  const auto& row = snapshot.get("stages")->asArray()[0];
  EXPECT_TRUE(row.get("observed")->asBool()); EXPECT_EQ(row.getNumber("exportedEvents"), 4);
  EXPECT_EQ(row.getNumber("timestampOrderInversions"), 1);
  EXPECT_EQ(row.getString("lastSourceFrameId"), "18"); EXPECT_EQ(row.getString("lastSourceEpoch"), "7");
  EXPECT_EQ(row.getString("lastSourceTag"), "42"); EXPECT_EQ(row.getString("lastProgressAgeTicks"), "1000");
  const auto& intervals = row.get("intervalCounts")->asArray();
  ASSERT_EQ(intervals.size(), 20u); EXPECT_EQ(intervals[7].asNumber(), 1); EXPECT_EQ(intervals[19].asNumber(), 1);
  double total = 0; for (const auto& value : intervals) total += value.asNumber(); EXPECT_EQ(total, 2);
  EXPECT_EQ(row.get("reasonCounts")->asArray()[0].asNumber(), 4);
  event.stage = static_cast<DeliveryStage>(11); aggregate.observe(event, 1000000);
  event.stage = DeliveryStage::ProgramDelivered; aggregate.observe(event, 0);
  EXPECT_FALSE(aggregate.snapshot(99999999, 1000000, 2).get("stages")->asArray()[4].get("observed")->asBool());
}

TEST(DeliveryTrace, LiveAggregateRefreshesAtMostOncePerSecondAndFinalizesAcceptedEvents) {
  Capture capture;
  DeliveryTraceEvent event; event.stage = DeliveryStage::ProgramDelivered; event.programSequence = 12;
  ASSERT_TRUE(capture.trace.record(event));
  const auto initial = capture.trace.snapshot().get("aggregate")->getNumber("revision"); EXPECT_EQ(initial, 0);
  const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(3);
  while (capture.trace.snapshot().get("aggregate")->getNumber("revision") == 0 && std::chrono::steady_clock::now() < until)
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  const auto first = *capture.trace.snapshot().get("aggregate");
  EXPECT_TRUE(corevideo::contracts::validateDeliveryEvidenceObservation(capture.trace.snapshot()));
  EXPECT_EQ(first.getNumber("revision"), 1);
  const auto later = *capture.trace.snapshot().get("aggregate");
  EXPECT_EQ(later.getString("observedAtTicks"), first.getString("observedAtTicks"));
  EXPECT_EQ(later.getNumber("revision"), first.getNumber("revision"));
  ASSERT_TRUE(capture.trace.close()); const auto final = *capture.trace.snapshot().get("aggregate");
  EXPECT_TRUE(corevideo::contracts::validateDeliveryEvidenceObservation(capture.trace.snapshot()));
  EXPECT_EQ(final.getNumber("revision"), 2);
  const auto& row = final.get("stages")->asArray()[4];
  EXPECT_EQ(row.getNumber("exportedEvents"), 1); EXPECT_EQ(row.getString("lastProgramSequence"), "12");
  EXPECT_TRUE(row.get("observed")->asBool());
}

TEST(DeliveryTrace, ConcurrentDiagnosticReadersSeeCompleteCachedObservationsThroughFinalization) {
  Capture capture;
  std::atomic<bool> stop{false};
  std::atomic<unsigned> invalid{0}, reads{0};
  std::vector<std::thread> readers;
  for (int i = 0; i < 3; ++i) readers.emplace_back([&] {
    double revision = 0;
    while (!stop.load()) {
      const auto snapshot = capture.trace.snapshot();
      const auto& aggregate = *snapshot.get("aggregate");
      if (!corevideo::contracts::validateDeliveryEvidenceObservation(snapshot) ||
          aggregate.getNumber("revision") < revision) ++invalid;
      revision = aggregate.getNumber("revision");
      const auto& row = aggregate.get("stages")->asArray()[4];
      const auto count = row.getNumber("exportedEvents");
      if (row.get("observed")->asBool() &&
          row.getString("lastProgramSequence") != std::to_string(static_cast<unsigned>(count))) ++invalid;
      ++reads;
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
  });
  for (int i = 1; i <= 100; ++i) {
    DeliveryTraceEvent event; event.stage = DeliveryStage::ProgramDelivered; event.programSequence = i;
    if (!capture.trace.record(event)) ++invalid;
    std::this_thread::sleep_for(std::chrono::milliseconds(12));
  }
  const bool closed = capture.trace.close();
  std::this_thread::sleep_for(std::chrono::milliseconds(10));
  stop.store(true); for (auto& reader : readers) reader.join();
  EXPECT_TRUE(closed); EXPECT_EQ(invalid.load(), 0u); EXPECT_GT(reads.load(), 100u);
  EXPECT_EQ(capture.trace.snapshot().get("aggregate")->get("stages")->asArray()[4].getNumber("exportedEvents"), 100);
}
