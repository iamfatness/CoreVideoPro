#include <gtest/gtest.h>
#include "core/MediaCore.h"
#include "modules/Interfaces.h"
#include "modules/ZoomEngineClient.h"
#include "modules/ZoomEngineRuntime.h"
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <mutex>
#include <random>
#include <string>
#include <thread>
#include <vector>

namespace {
void setChurnEnv(const char* name, const char* value) {
#if defined(_WIN32)
  _putenv_s(name, value ? value : "");
#else
  if (value) setenv(name, value, 1); else unsetenv(name);
#endif
}
std::string churnRosterJson(int revision, int count, bool dropOdd) {
  std::string json = R"({"cmd":"participants","meeting_generation":1,"roster_revision":)" +
                     std::to_string(revision) + R"(,"participants":[)";
  bool first = true;
  for (int id = 1; id <= count; ++id) {
    if (dropOdd && (id % 2)) continue;
    if (!first) json += ",";
    first = false;
    json += R"({"id":)" + std::to_string(id) + R"(,"name":"Guest )" + std::to_string(id) + R"("})";
  }
  return json + "]}";
}
struct HoldStats {
  long long worst = 0, p99 = 0, p50 = 0, mean = 0;
};
HoldStats summarize(std::vector<long long> values) {
  HoldStats stats;
  if (values.empty()) return stats;
  std::sort(values.begin(), values.end());
  long long sum = 0;
  for (auto v : values) sum += v;
  stats.worst = values.back();
  stats.p99 = values[values.size() * 99 / 100];
  stats.p50 = values[values.size() / 2];
  stats.mean = sum / static_cast<long long>(values.size());
  return stats;
}
}  // namespace

// #1037 QA harness (NOT a CI gate: timing). Run with COREVIDEO_QA_LOCK_CHURN=1
// on a RELEASE build, once on main (before) and once on the branch (after).
// It reports the coreMutex hold of the render display tick while another
// thread applies Zoom roster events (which hold ZoomEngineRuntime::mutex_) at
// maximum rate.
TEST(RenderEngineMutexChurnQa, WorstRenderHoldUnderRosterChurn) {
  const char* gate = std::getenv("COREVIDEO_QA_LOCK_CHURN");
  if (!gate || std::string(gate) != "1") {
    std::fprintf(stderr, "[  SKIPPED ] RenderEngineMutexChurnQa (set COREVIDEO_QA_LOCK_CHURN=1)\n");
    return;
  }
  setChurnEnv("COREVIDEO_ZOOM_ENGINE_PATH", "C:/fake/corevideo-zoom-engine.exe");
  auto core = std::make_unique<corevideo::core::MediaCore>(corevideo::modules::createStubModules());
  setChurnEnv("COREVIDEO_ZOOM_ENGINE_PATH", nullptr);
  auto* runtime = core->zoomEngineRuntimeForTest();
  ASSERT_TRUE(runtime && runtime->configured());
  constexpr int kParticipants = 50;
  runtime->applyEngineEventForTest(*corevideo::modules::parseZoomEngineEvent(churnRosterJson(1, kParticipants, false)));
  // Multiview active so the multiview plan and tiles run every tick
  // (kMultiviewTickDivisor = 1).
  corevideo::rpc::Json::Array sources;
  for (int i = 0; i < 8; ++i)
    sources.emplace_back(corevideo::rpc::Json::Object{{"sourceId", "zoom:" + std::to_string(i + 1)},
        {"kind", "participant"}, {"participantId", std::to_string(i + 1)}, {"slot", i},
        {"label", "Tile " + std::to_string(i + 1)}});
  (void)core->applyCommand(corevideo::rpc::Json::Object{{"type", "set-multiview-layout"},
      {"canvasWidth", 1920}, {"canvasHeight", 1080}, {"sources", sources}});

  std::atomic<bool> stop{false};
  std::atomic<long long> churnEvents{0};
  std::thread churn([&] {
    for (int revision = 2; !stop.load(); ++revision) {
      runtime->applyEngineEventForTest(*corevideo::modules::parseZoomEngineEvent(
          churnRosterJson(revision, kParticipants, (revision % 2) != 0)));
      churnEvents.fetch_add(1, std::memory_order_relaxed);
    }
  });

  constexpr int kTicks = 1200;
  std::mutex coreMutex;
  std::vector<long long> holdsUs;
  std::vector<long long> preReadUs;
  holdsUs.reserve(kTicks);
  preReadUs.reserve(kTicks);
  auto next = std::chrono::steady_clock::now();
  for (int tick = 0; tick < kTicks; ++tick) {
    next += std::chrono::microseconds(16'667);
    // Production shape (JsonRpcServer render loop, #1037): the engine pre-read
    // happens BEFORE coreMutex. Its duration is the render thread's own
    // residual engine wait, not a coreMutex hold, so it is reported apart.
    const auto p0 = std::chrono::steady_clock::now();
    auto inputs = core->prepareRenderEngineInputsUnlocked();
    preReadUs.push_back(std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::steady_clock::now() - p0).count());
    {
      std::lock_guard<std::mutex> lock(coreMutex);
      const auto t0 = std::chrono::steady_clock::now();
      core->renderDisplayTick(-1, 0, std::move(inputs));
      holdsUs.push_back(std::chrono::duration_cast<std::chrono::microseconds>(
          std::chrono::steady_clock::now() - t0).count());
    }
    std::this_thread::sleep_until(next);
  }
  stop = true;
  churn.join();
  const auto hold = summarize(holdsUs);
  std::fprintf(stderr,
      "[render-churn] participants=%d ticks=%zu churnEvents=%lld hold worst=%lldus p99=%lldus p50=%lldus mean=%lldus\n",
      kParticipants, holdsUs.size(), churnEvents.load(), hold.worst, hold.p99, hold.p50, hold.mean);
  const auto preRead = summarize(preReadUs);
  std::fprintf(stderr,
      "[render-churn] pre-read (off coreMutex) worst=%lldus p99=%lldus p50=%lldus mean=%lldus\n",
      preRead.worst, preRead.p99, preRead.p50, preRead.mean);
  core.reset();
}

// #1049 QA harness (NOT a CI gate: timing). The fetch-before-lock A/B for the
// render frame group (R1 pollCompositorVideoFrames, R2 latestDecodedVideoFrames,
// R5 appendCpuVideoFrames). Run on a RELEASE build with
// COREVIDEO_QA_FRAME_FETCH_AB=1. It drives the production render-loop shape
// (pre-read, coreMutex, 3-argument renderDisplayTick) at 60 Hz against:
//   - a video ingest thread publishing every participant's camera at the
//     source rate (engine mutex, frame-sync queue, like the publish phase);
//   - roster events (engine mutex; applyEvent) at 10 Hz, or max rate with
//     COREVIDEO_QA_AB_ROSTER_HZ=0;
//   - the audio worker's pre-lock engine poll at 50 Hz plus its short
//     coreMutex gather hold;
//   - the Zoom pump's drainFrameEvents at 100 Hz;
//   - a command lane taking coreMutex every 5 ms with a hold drawn from a
//     cmd.handle-like distribution (90 % 50-300 us, 9 % 0.3-1 ms, 1 % 1-8 ms).
// The two orders alternate in blocks so drift and thermal state hit both.
// It reports, per order: Zoom frame age at the tick's consume point (ingest
// observation to consume, the same point in both orders), the coreMutex hold,
// the render thread's coreMutex wait and its pre-read. The decision rule
// (issue #1049): before-lock may become the default only if its median frame
// age is no more than one frame period (16.7 ms) above under-lock's.
namespace {
long long envLongLong(const char* name, long long fallback) {
  const char* value = std::getenv(name);
  if (!value || !*value) return fallback;
  return std::atoll(value);
}
struct Distribution {
  double p50 = 0, p90 = 0, p99 = 0, max = 0, mean = 0;
  std::size_t n = 0;
};
Distribution distributionMs(std::vector<long long> valuesNs) {
  Distribution d;
  if (valuesNs.empty()) return d;
  std::sort(valuesNs.begin(), valuesNs.end());
  long double sum = 0;
  for (auto v : valuesNs) sum += v;
  const auto at = [&](double q) { return valuesNs[static_cast<std::size_t>(q * (valuesNs.size() - 1))] / 1e6; };
  d.p50 = at(0.50);
  d.p90 = at(0.90);
  d.p99 = at(0.99);
  d.max = valuesNs.back() / 1e6;
  d.mean = static_cast<double>(sum / valuesNs.size()) / 1e6;
  d.n = valuesNs.size();
  return d;
}
void spinFor(std::chrono::nanoseconds duration) {
  const auto until = std::chrono::steady_clock::now() + duration;
  while (std::chrono::steady_clock::now() < until) {}
}
std::string abRosterJson(int revision, int count) {
  std::string json = R"({"cmd":"participants","meeting_generation":1,"roster_revision":)" +
                     std::to_string(revision) + R"(,"participants":[)";
  for (int id = 1; id <= count; ++id) {
    if (id > 1) json += ",";
    // The talker rotates: every event changes one participant's state.
    json += R"({"id":)" + std::to_string(id) + R"(,"name":"Guest )" + std::to_string(id) +
            R"(","talking":)" + ((revision % count) + 1 == id ? "true" : "false") + "}";
  }
  return json + "]}";
}
}  // namespace

TEST(RenderFrameFetchAbQa, FrameAgeAndHoldForEachFetchOrder) {
  const char* gate = std::getenv("COREVIDEO_QA_FRAME_FETCH_AB");
  if (!gate || std::string(gate) != "1") {
    std::fprintf(stderr, "[  SKIPPED ] RenderFrameFetchAbQa (set COREVIDEO_QA_FRAME_FETCH_AB=1)\n");
    return;
  }
  const int participants = static_cast<int>(envLongLong("COREVIDEO_QA_AB_PARTICIPANTS", 10));
  const int sourceFps = static_cast<int>(envLongLong("COREVIDEO_QA_AB_SOURCE_FPS", 30));
  const int rosterHz = static_cast<int>(envLongLong("COREVIDEO_QA_AB_ROSTER_HZ", 10));
  const int blocksPerOrder = static_cast<int>(envLongLong("COREVIDEO_QA_AB_BLOCKS", 6));
  const int ticksPerBlock = static_cast<int>(envLongLong("COREVIDEO_QA_AB_TICKS_PER_BLOCK", 300));
  constexpr int kWarmupTicks = 15;

  setChurnEnv("COREVIDEO_ZOOM_ENGINE_PATH", "C:/fake/corevideo-zoom-engine.exe");
  auto core = std::make_unique<corevideo::core::MediaCore>(corevideo::modules::createStubModules());
  setChurnEnv("COREVIDEO_ZOOM_ENGINE_PATH", nullptr);
  auto* runtime = core->zoomEngineRuntimeForTest();
  ASSERT_TRUE(runtime && runtime->configured());
  runtime->applyEngineEventForTest(*corevideo::modules::parseZoomEngineEvent(abRosterJson(1, participants)));
  corevideo::rpc::Json::Array sources;
  for (int i = 0; i < std::min(8, participants); ++i)
    sources.emplace_back(corevideo::rpc::Json::Object{{"sourceId", "zoom:" + std::to_string(i + 1)},
        {"kind", "participant"}, {"participantId", std::to_string(i + 1)}, {"slot", i},
        {"label", "Tile " + std::to_string(i + 1)}});
  (void)core->applyCommand(corevideo::rpc::Json::Object{{"type", "set-multiview-layout"},
      {"canvasWidth", 1920}, {"canvasHeight", 1080}, {"sources", sources}});

  std::mutex coreMutex;
  std::atomic<bool> stop{false};
  std::atomic<long long> published{0}, rosterEvents{0}, commandHolds{0};
  const auto i420 = std::make_shared<const std::vector<std::uint8_t>>(64 * 36 * 3 / 2, 128);

  std::thread ingest([&] {
    std::int64_t frameId = 0;
    auto next = std::chrono::steady_clock::now();
    const auto period = std::chrono::nanoseconds(1'000'000'000LL / std::max(1, sourceFps));
    while (!stop.load()) {
      ++frameId;
      for (int id = 1; id <= participants; ++id) {
        runtime->publishDecodedVideoFrameForTest(std::to_string(id), frameId, i420, 64, 36,
                                                 std::chrono::steady_clock::now());
        published.fetch_add(1, std::memory_order_relaxed);
      }
      next += period;
      std::this_thread::sleep_until(next);
    }
  });
  std::thread roster([&] {
    auto next = std::chrono::steady_clock::now();
    for (int revision = 2; !stop.load(); ++revision) {
      runtime->applyEngineEventForTest(*corevideo::modules::parseZoomEngineEvent(abRosterJson(revision, participants)));
      rosterEvents.fetch_add(1, std::memory_order_relaxed);
      if (rosterHz > 0) {
        next += std::chrono::nanoseconds(1'000'000'000LL / rosterHz);
        std::this_thread::sleep_until(next);
      }
    }
  });
  std::thread audio([&] {
    auto next = std::chrono::steady_clock::now();
    for (long long tick = 0; !stop.load(); ++tick) {
      (void)runtime->pollCompositorAudioFrames(tick * 20);  // pre-lock, like pollZoomAudioUnlocked
      {
        std::lock_guard<std::mutex> lock(coreMutex);         // audio.gather
        spinFor(std::chrono::microseconds(100));
      }
      next += std::chrono::milliseconds(20);
      std::this_thread::sleep_until(next);
    }
  });
  std::thread pump([&] {
    while (!stop.load()) {
      (void)runtime->drainFrameEvents();
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
  });
  std::thread commands([&] {
    std::mt19937 rng(1049);
    std::uniform_real_distribution<double> unit(0.0, 1.0);
    while (!stop.load()) {
      std::this_thread::sleep_for(std::chrono::milliseconds(5));
      const double draw = unit(rng);
      const double holdUs = draw < 0.90 ? 50 + 250 * unit(rng)
                          : draw < 0.99 ? 300 + 700 * unit(rng)
                                        : 1000 + 7000 * unit(rng);
      std::lock_guard<std::mutex> lock(coreMutex);
      spinFor(std::chrono::nanoseconds(static_cast<long long>(holdUs * 1000)));
      commandHolds.fetch_add(1, std::memory_order_relaxed);
    }
  });

  struct Arm {
    std::vector<long long> ageNs, holdNs, waitNs, preReadNs;
  };
  std::array<Arm, 2> arms;  // [0] under-lock, [1] before-lock
  auto next = std::chrono::steady_clock::now();
  for (int block = 0; block < blocksPerOrder * 2; ++block) {
    const bool beforeLock = (block % 2) == 1;
    auto& arm = arms[beforeLock ? 1 : 0];
    core->setRenderFrameFetchBeforeLockForTest(beforeLock);
    for (int tick = 0; tick < ticksPerBlock; ++tick) {
      next += std::chrono::nanoseconds(16'666'667);
      const auto t0 = std::chrono::steady_clock::now();
      auto inputs = core->prepareRenderEngineInputsUnlocked();
      const auto t1 = std::chrono::steady_clock::now();
      long long holdNs = 0;
      std::chrono::steady_clock::time_point t2;
      {
        std::lock_guard<std::mutex> lock(coreMutex);
        t2 = std::chrono::steady_clock::now();
        core->renderDisplayTick(-1, 0, std::move(inputs));
        holdNs = std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - t2).count();
      }
      if (tick < kWarmupTicks) {
        (void)core->takeZoomFrameAgeSamplesNsForTest();  // the previous order's queue tail
      } else {
        arm.holdNs.push_back(holdNs);
        arm.waitNs.push_back(std::chrono::duration_cast<std::chrono::nanoseconds>(t2 - t1).count());
        arm.preReadNs.push_back(std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count());
      }
      std::this_thread::sleep_until(next);
    }
    auto ages = core->takeZoomFrameAgeSamplesNsForTest();
    arm.ageNs.insert(arm.ageNs.end(), ages.begin(), ages.end());
  }
  stop = true;
  for (auto* thread : {&ingest, &roster, &audio, &pump, &commands}) thread->join();

  std::fprintf(stderr,
      "[frame-fetch-ab] workload participants=%d sourceFps=%d rosterHz=%d blocksPerOrder=%d ticksPerBlock=%d "
      "published=%lld rosterEvents=%lld commandHolds=%lld\n",
      participants, sourceFps, rosterHz, blocksPerOrder, ticksPerBlock, published.load(), rosterEvents.load(),
      commandHolds.load());
  std::array<Distribution, 2> age{};
  for (int i = 0; i < 2; ++i) {
    const char* order = i == 0 ? "under-lock " : "before-lock";
    age[i] = distributionMs(arms[i].ageNs);
    const auto hold = distributionMs(arms[i].holdNs);
    const auto wait = distributionMs(arms[i].waitNs);
    const auto pre = distributionMs(arms[i].preReadNs);
    std::fprintf(stderr,
        "[frame-fetch-ab] %s frame-age ms p50=%.3f p90=%.3f p99=%.3f max=%.3f mean=%.3f n=%zu | "
        "hold ms p50=%.3f p99=%.3f max=%.3f | lock-wait ms p50=%.3f p99=%.3f max=%.3f | "
        "pre-read ms p50=%.3f p99=%.3f max=%.3f\n",
        order, age[i].p50, age[i].p90, age[i].p99, age[i].max, age[i].mean, age[i].n,
        hold.p50, hold.p99, hold.max, wait.p50, wait.p99, wait.max, pre.p50, pre.p99, pre.max);
  }
  const double medianDelta = age[1].p50 - age[0].p50;
  std::fprintf(stderr,
      "[frame-fetch-ab] median frame-age delta (before-lock - under-lock) = %.3f ms; rule: <= 16.667 ms -> %s\n",
      medianDelta, medianDelta <= 16.667 ? "before-lock MAY be the default" : "keep under-lock");
  core.reset();
}
