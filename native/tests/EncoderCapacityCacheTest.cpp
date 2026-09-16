#include "modules/EncoderCapacityProbe.h"
#include <gtest/gtest.h>
#include <atomic>
#include <future>
#include <thread>

using namespace corevideo::modules;

namespace {
bool waitForProbe(EncoderCapacityCache& cache) {
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
  while (cache.probeInFlightForTesting() && std::chrono::steady_clock::now() < deadline)
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  return !cache.probeInFlightForTesting();
}
ProbedEncoderCapacity capacity(const EncoderProbeKey& key) {
  ProbedEncoderCapacity result;
  result.key = key; result.status = EncoderProbeStatus::Ready;
  result.probed = true; result.hardwareAvailable = true; result.hardwareSessionCeiling = 7;
  return result;
}
}

TEST(EncoderCapacityCache, ProbesWaitUntilEveryLiveEncoderReleasesItsLease) {
  auto& cache = EncoderCapacityCache::instance();
  cache.setRecordingActive(false);
  std::atomic<int> calls{0};
  cache.setProbeFunctionForTesting([&](const EncoderProbeKey& key) { ++calls; return capacity(key); });
  const EncoderProbeKey key{"h264", 1920, 1080, 60};
  cache.beginLiveEncoding(); cache.beginLiveEncoding();
  EXPECT_FALSE(cache.lookup(key).probed);
  cache.endLiveEncoding();
  cache.prewarm(key);
  EXPECT_EQ(calls.load(), 0);
  cache.endLiveEncoding();
  cache.prewarm(key);
  const bool finished = waitForProbe(cache);
  const auto result = cache.lookup(key);
  cache.setProbeFunctionForTesting({});
  EXPECT_TRUE(finished);
  EXPECT_EQ(calls.load(), 1);
  EXPECT_EQ(result.hardwareSessionCeiling, 7);
}

TEST(EncoderCapacityCache, DiscardsAProbeThatOverlappedRecordingOrStreamingEvenAfterStop) {
  auto& cache = EncoderCapacityCache::instance();
  for (bool streaming : {false, true}) {
    cache.setRecordingActive(false);
    std::promise<void> entered, release;
    auto gate = release.get_future().share();
    cache.setProbeFunctionForTesting([&](const EncoderProbeKey& key) {
      entered.set_value(); gate.wait(); return capacity(key);
    });
    const EncoderProbeKey key{"h264", 1920, 1080, 60};
    cache.prewarm(key);
    const auto started = entered.get_future().wait_for(std::chrono::seconds(2));
    if (streaming) { cache.beginLiveEncoding(); cache.endLiveEncoding(); }
    else { cache.setRecordingActive(true); cache.setRecordingActive(false); }
    release.set_value();
    const bool finished = waitForProbe(cache);
    // Keep lookup from scheduling another probe: inspect the rejected result.
    cache.setRecordingActive(true);
    const auto result = cache.lookup(key);
    cache.setRecordingActive(false);
    cache.setProbeFunctionForTesting({});
    EXPECT_EQ(started, std::future_status::ready);
    EXPECT_TRUE(finished);
    EXPECT_FALSE(result.probed);
    EXPECT_EQ(result.status, EncoderProbeStatus::Pending);
  }
}
