#include "core/RenderWorkDistribution.h"
#include "core/MediaCore.h"
#include "modules/Interfaces.h"
#include <gtest/gtest.h>
#include <limits>
#include <thread>

using corevideo::core::RenderWorkDistribution;
TEST(RenderWorkDistribution, DisabledIsUnknownAndEnabledBucketsBoundExtremeDurations) {
  RenderWorkDistribution disabled(false); disabled.record(123);
  EXPECT_FALSE(disabled.snapshot().get("enabled")->asBool());
  EXPECT_EQ(disabled.snapshot().get("bins"), nullptr);
  RenderWorkDistribution enabled(true);
  for (auto value : {int64_t{0}, int64_t{999}, int64_t{1000}, int64_t{32767999},
      int64_t{32768000}, (std::numeric_limits<int64_t>::max)(), int64_t{-1}}) enabled.record(value);
  const auto result = enabled.snapshot();
  EXPECT_EQ(result.getNumber("sampleCount"), 6); EXPECT_EQ(result.getNumber("invalidSamples"), 1);
  ASSERT_EQ(result.get("bins")->asArray().size(), 4u);
  const auto& rows = result.get("bins")->asArray();
  EXPECT_EQ(rows[0].asArray()[0].asNumber(), 0); EXPECT_EQ(rows[0].asArray()[1].asNumber(), 2);
  EXPECT_EQ(rows[1].asArray()[0].asNumber(), 1);
  EXPECT_EQ(rows[2].asArray()[0].asNumber(), 32767);
  EXPECT_EQ(rows[3].asArray()[0].asNumber(), 32768); EXPECT_EQ(rows[3].asArray()[1].asNumber(), 2);
  EXPECT_LE(std::stoll(result.getString("scanStartedAtNs")), std::stoll(result.getString("scanEndedAtNs")));
}
TEST(RenderWorkDistribution, ConcurrentScansRetainEverySampleWithoutResetOrOverwrite) {
  RenderWorkDistribution distribution(true);
  std::thread writer([&] { for (int i = 0; i < 100000; ++i) distribution.record((i % 700) * 1000); });
  double previous = 0;
  for (int i = 0; i < 20; ++i) {
    const auto snapshot = distribution.snapshot();
    double sum = 0; for (const auto& row : snapshot.get("bins")->asArray()) sum += row.asArray()[1].asNumber();
    EXPECT_EQ(sum, snapshot.getNumber("sampleCount")); EXPECT_GE(sum, previous); previous = sum;
  }
  writer.join(); EXPECT_EQ(distribution.snapshot().getNumber("sampleCount"), 100000);
}
TEST(RenderWorkDistribution, RealCoreWorkerReportingReachesDiagnosticSnapshotsOnlyWhenExplicitlyEnabled) {
  const auto* original = std::getenv("COREVIDEO_QA_RENDER_WORK_DISTRIBUTION");
  const std::string saved = original ? original : "";
  struct Restore {
    std::string saved; bool present;
    ~Restore() {
#if defined(_WIN32)
      _putenv_s("COREVIDEO_QA_RENDER_WORK_DISTRIBUTION", saved.c_str());
#else
      if (present) setenv("COREVIDEO_QA_RENDER_WORK_DISTRIBUTION", saved.c_str(), 1);
      else unsetenv("COREVIDEO_QA_RENDER_WORK_DISTRIBUTION");
#endif
    }
  } restore{saved, original != nullptr};
#if defined(_WIN32)
  _putenv_s("COREVIDEO_QA_RENDER_WORK_DISTRIBUTION", "1");
#else
  setenv("COREVIDEO_QA_RENDER_WORK_DISTRIBUTION", "1", 1);
#endif
  corevideo::core::MediaCore core(corevideo::modules::createStubModules());
  core.reportRenderWorkerStarted();
  core.reportRenderWorkerProgress(1, 0, 0, 0, 10, 123456, 20);
  core.reportRenderWorkerProgress(2, 0, 0, 0, 10, 999999, 20);
  const auto state = core.sessionState();
  const auto& work = *state.get("realtimeEvidence")->get("render")->get("workDistribution");
  EXPECT_TRUE(work.get("enabled")->asBool()); EXPECT_EQ(work.getNumber("sampleCount"), 2);
  EXPECT_EQ(work.get("bins")->asArray()[0].asArray()[0].asNumber(), 123);
  EXPECT_EQ(work.get("bins")->asArray()[1].asArray()[0].asNumber(), 999);
  EXPECT_FALSE(state.get("realtimeEvidence")->get("render")->get("deliveryVerified")->asBool());
}
