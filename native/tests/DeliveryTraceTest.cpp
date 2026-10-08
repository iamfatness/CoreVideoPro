#include "core/DeliveryTrace.h"
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
