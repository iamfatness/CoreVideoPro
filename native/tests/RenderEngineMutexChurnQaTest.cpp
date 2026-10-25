#include <gtest/gtest.h>
#include "core/MediaCore.h"
#include "modules/Interfaces.h"
#include "modules/ZoomEngineClient.h"
#include "modules/ZoomEngineRuntime.h"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <mutex>
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
