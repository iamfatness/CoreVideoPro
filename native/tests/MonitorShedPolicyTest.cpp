#include "core/MonitorShedPolicy.h"

#include "core/MediaCore.h"
#include "modules/Interfaces.h"
#include "rpc/Json.h"

#include <gtest/gtest.h>

#include <chrono>
#include <cstdint>
#include <memory>
#include <string>
#include <thread>
#include <vector>

namespace {

using corevideo::core::MonitorShedObservation;
using corevideo::core::MonitorShedPolicy;
using corevideo::core::MonitorShedTransition;

// 60fps Program.
constexpr std::int64_t kBudgetNs = 1'000'000'000 / 60;
constexpr std::int64_t kMs = 1'000'000;

MonitorShedObservation tickNs(std::int64_t programNs, std::int64_t monitorNs) {
  MonitorShedObservation o;
  o.budgetNs = kBudgetNs;
  o.programCostNs = programNs;
  o.monitorCycleCostNs = monitorNs;
  return o;
}

MonitorShedObservation tick(std::int64_t programMs, std::int64_t monitorMs) {
  return tickNs(programMs * kMs, monitorMs * kMs);
}

// A sustained monitor overload that divisor 2 absorbs: 26ms at divisor 1
// (156% of budget), 28ms per 33.3ms cycle at divisor 2 (84% — under the 90%
// ceiling), and nowhere near comfortable at divisor 1.
MonitorShedObservation stalled() { return tick(2, 24); }
MonitorShedObservation healthy() { return tick(3, 1); }

void feed(MonitorShedPolicy& policy, const MonitorShedObservation& o, int ticks) {
  for (int i = 0; i < ticks; ++i) (void)policy.observe(o);
}

}  // namespace

TEST(MonitorShedPolicy, AHealthyTickStreamNeverSheds) {
  MonitorShedPolicy policy;
  for (int i = 0; i < 10'000; ++i) {
    EXPECT_EQ(policy.observe(healthy()), MonitorShedTransition::None);
  }
  EXPECT_EQ(policy.divisor(), 1);
  EXPECT_EQ(policy.level(), 0);
  EXPECT_EQ(policy.enteredCount(), 0);
  EXPECT_EQ(policy.shedTicks(), 0);
  EXPECT_EQ(std::string(policy.lastReason()), "none");
}

TEST(MonitorShedPolicy, KConsecutiveOverBudgetTicksEngageShedding) {
  MonitorShedPolicy policy;
  for (int i = 1; i < MonitorShedPolicy::kEnterAfterOverBudgetTicks; ++i) {
    EXPECT_EQ(policy.observe(stalled()), MonitorShedTransition::None) << "tick " << i;
    EXPECT_EQ(policy.divisor(), 1);
  }
  EXPECT_EQ(policy.observe(stalled()), MonitorShedTransition::Enter);
  EXPECT_EQ(policy.divisor(), 2);
  EXPECT_EQ(policy.level(), 1);
  EXPECT_EQ(policy.enteredCount(), 1);
  EXPECT_EQ(std::string(policy.lastReason()), "over-budget");
}

// A load that fits under the ceiling at divisor 2 and not comfortably at 1: the
// policy settles at 2 and stays there for as long as the stall lasts. It
// neither over-sheds to 3 nor flaps back to 1.
TEST(MonitorShedPolicy, ASustainedLoadThatFitsAtTwoSettlesAtTwo) {
  MonitorShedPolicy policy;
  feed(policy, stalled(), 10'000);
  EXPECT_EQ(policy.divisor(), 2);
  EXPECT_EQ(policy.enteredCount(), 1) << "a stable shed must not re-enter";
  EXPECT_GT(policy.shedTicks(), 9'000);
}

// THE MEASURED CASE (RTX 4090 rig, MonitorRenderFaultInjection): a 25ms
// Preview stall measured 30.4ms per monitor pass against 0.7ms of other render
// work. At divisor 2 that is 31.5ms of a 33.3ms cycle — it "fits", and with the
// first cut of this policy (100% ceiling) it held 2 while Program still lost a
// third of its frames to jitter in the <2ms of slack. It must go to 3.
TEST(MonitorShedPolicy, ALoadThatLeavesUnderTenPercentSlackAtTwoGoesToThree) {
  MonitorShedPolicy policy;
  const auto measured = tickNs(700'000, 30'400'000);
  ASSERT_FALSE((2 * measured.programCostNs + measured.monitorCycleCostNs) > 2 * measured.budgetNs)
      << "precondition: this load fits the raw 2-tick budget";
  EXPECT_TRUE(MonitorShedPolicy::overBudgetAt(measured, 2));
  EXPECT_FALSE(MonitorShedPolicy::overBudgetAt(measured, 3));
  feed(policy, measured, 1'000);
  EXPECT_EQ(policy.divisor(), 3);
}

TEST(MonitorShedPolicy, AHeavierSustainedLoadStepsOneLevelAtATimeToTheMaximum) {
  MonitorShedPolicy policy;
  const auto heavy = tick(3, 40);  // 2*3+40 = 46ms > 33.3ms: does not fit at 2
  feed(policy, heavy, MonitorShedPolicy::kEnterAfterOverBudgetTicks);
  EXPECT_EQ(policy.divisor(), 2);
  for (int i = 1; i < MonitorShedPolicy::kEnterAfterOverBudgetTicks; ++i) {
    EXPECT_EQ(policy.observe(heavy), MonitorShedTransition::None);
  }
  EXPECT_EQ(policy.observe(heavy), MonitorShedTransition::StepUp);
  EXPECT_EQ(policy.divisor(), 3);
  EXPECT_EQ(policy.level(), 2);
  EXPECT_EQ(policy.enteredCount(), 1) << "stepping within a shed is not a new entry";
}

TEST(MonitorShedPolicy, ThreeIsTheCeilingEvenWhenThreeDoesNotFit) {
  MonitorShedPolicy policy;
  const auto hopeless = tick(3, 200);
  for (int i = 0; i < 10'000; ++i) {
    const auto transition = policy.observe(hopeless);
    EXPECT_NE(transition, MonitorShedTransition::StepDown);
    EXPECT_NE(transition, MonitorShedTransition::Exit);
  }
  EXPECT_EQ(policy.divisor(), MonitorShedPolicy::kMaxDivisor);
  EXPECT_EQ(MonitorShedPolicy::kMaxDivisor, 3);
}

TEST(MonitorShedPolicy, ASingleSpikeDoesNotShed) {
  MonitorShedPolicy policy;
  feed(policy, healthy(), 100);
  (void)policy.observe(tick(3, 100));  // one 100ms monitor pass
  feed(policy, healthy(), 100);
  EXPECT_EQ(policy.divisor(), 1);
  EXPECT_EQ(policy.enteredCount(), 0);
}

// At d=2 a pass's cost sample persists across the tick that skips it, so one
// slow pass is seen on (at most) two consecutive ticks. That must still not
// count as sustained.
TEST(MonitorShedPolicy, ASpikeSeenOnTwoTicksStillDoesNotShed) {
  static_assert(MonitorShedPolicy::kEnterAfterOverBudgetTicks > 2,
                "a stale pass sample at d=2 spans two ticks; entry must need more");
  MonitorShedPolicy policy;
  (void)policy.observe(tick(3, 100));
  (void)policy.observe(tick(3, 100));
  feed(policy, healthy(), 10);
  EXPECT_EQ(policy.divisor(), 1);
}

TEST(MonitorShedPolicy, InterleavedSpikesThatNeverReachKInARowDoNotShed) {
  MonitorShedPolicy policy;
  for (int i = 0; i < 1'000; ++i) {
    (void)policy.observe(stalled());
    (void)policy.observe(stalled());
    (void)policy.observe(healthy());
  }
  EXPECT_EQ(policy.divisor(), 1);
}

TEST(MonitorShedPolicy, RecoveryNeedsMConsecutiveHealthyTicksAndStepsBackToOne) {
  MonitorShedPolicy policy;
  feed(policy, tick(3, 40), 100);
  ASSERT_EQ(policy.divisor(), 3);

  // M-1 healthy ticks: still shedding.
  feed(policy, healthy(), MonitorShedPolicy::kRecoverAfterHealthyTicks - 1);
  EXPECT_EQ(policy.divisor(), 3);
  // The M-th steps down ONE level, not straight to 1.
  EXPECT_EQ(policy.observe(healthy()), MonitorShedTransition::StepDown);
  EXPECT_EQ(policy.divisor(), 2);
  EXPECT_EQ(std::string(policy.lastReason()), "recovered");

  // And the next level needs its own full M.
  feed(policy, healthy(), MonitorShedPolicy::kRecoverAfterHealthyTicks - 1);
  EXPECT_EQ(policy.divisor(), 2);
  EXPECT_EQ(policy.observe(healthy()), MonitorShedTransition::Exit);
  EXPECT_EQ(policy.divisor(), 1);
  EXPECT_EQ(policy.level(), 0);
  EXPECT_EQ(policy.enteredCount(), 1);
}

TEST(MonitorShedPolicy, ASlowTickDuringRecoveryRestartsTheHealthyCount) {
  MonitorShedPolicy policy;
  feed(policy, stalled(), 10);
  ASSERT_EQ(policy.divisor(), 2);
  feed(policy, healthy(), MonitorShedPolicy::kRecoverAfterHealthyTicks - 1);
  (void)policy.observe(stalled());  // fits at 2 but not comfortably at 1
  feed(policy, healthy(), MonitorShedPolicy::kRecoverAfterHealthyTicks - 1);
  EXPECT_EQ(policy.divisor(), 2) << "M must be CONSECUTIVE";
  EXPECT_EQ(policy.observe(healthy()), MonitorShedTransition::Exit);
}

// The hysteresis band: a load that fits at 1 but uses more than 75% of the
// budget does not count toward recovery, so it cannot step down into an
// overrun and flap.
TEST(MonitorShedPolicy, ALoadInsideTheHysteresisBandHoldsTheShed) {
  MonitorShedPolicy policy;
  feed(policy, stalled(), 10);
  ASSERT_EQ(policy.divisor(), 2);
  const auto marginal = tick(3, 11);  // 14ms at d=1: under 90%, but >75% of 16.7ms
  ASSERT_FALSE(MonitorShedPolicy::overBudgetAt(marginal, 1));
  ASSERT_FALSE(MonitorShedPolicy::comfortablyWithinAt(marginal, 1));
  feed(policy, marginal, 10'000);
  EXPECT_EQ(policy.divisor(), 2);
  EXPECT_EQ(policy.enteredCount(), 1);
}

TEST(MonitorShedPolicy, ARecurringOverloadCountsEachEntry) {
  MonitorShedPolicy policy;
  for (int cycle = 0; cycle < 3; ++cycle) {
    feed(policy, stalled(), 10);
    feed(policy, healthy(), MonitorShedPolicy::kRecoverAfterHealthyTicks);
    ASSERT_EQ(policy.divisor(), 1);
  }
  EXPECT_EQ(policy.enteredCount(), 3);
}

TEST(MonitorShedPolicy, ShedTicksCountTicksObservedWhileShedding) {
  MonitorShedPolicy policy;
  feed(policy, stalled(), MonitorShedPolicy::kEnterAfterOverBudgetTicks);
  EXPECT_EQ(policy.shedTicks(), 0) << "the entering tick itself ran at divisor 1";
  feed(policy, stalled(), 5);
  EXPECT_EQ(policy.shedTicks(), 5);
}

TEST(MonitorShedPolicy, AnUnknownBudgetIsNoEvidence) {
  MonitorShedPolicy policy;
  MonitorShedObservation o = stalled();
  o.budgetNs = 0;
  feed(policy, o, 100);
  EXPECT_EQ(policy.divisor(), 1);
  feed(policy, stalled(), 10);
  ASSERT_EQ(policy.divisor(), 2);
  feed(policy, o, 1'000);
  EXPECT_EQ(policy.divisor(), 2) << "no evidence must not count as healthy";
}

TEST(MonitorShedPolicy, NegativeCostsAreClampedNotRewarded) {
  EXPECT_FALSE(MonitorShedPolicy::overBudgetAt(tick(-100, -100), 1));
  EXPECT_TRUE(MonitorShedPolicy::overBudgetAt(tick(-100, 30), 1));
}

TEST(MonitorShedPolicy, TheObservationBehindTheLastTransitionIsKept) {
  MonitorShedPolicy policy;
  EXPECT_EQ(policy.lastTransitionObservation().budgetNs, 0) << "nothing before the first transition";
  const auto programBound = tick(15, 2);  // 17ms at d=1: Program, not the monitors, is the cost
  feed(policy, programBound, MonitorShedPolicy::kEnterAfterOverBudgetTicks);
  ASSERT_EQ(policy.divisor(), 2);
  EXPECT_EQ(policy.lastTransitionObservation().programCostNs, 15 * kMs);
  EXPECT_EQ(policy.lastTransitionObservation().monitorCycleCostNs, 2 * kMs);
  feed(policy, healthy(), MonitorShedPolicy::kRecoverAfterHealthyTicks);
  ASSERT_EQ(policy.divisor(), 1);
  EXPECT_EQ(policy.lastTransitionObservation().programCostNs, 3 * kMs);
}

TEST(MonitorShedPolicy, TransitionsHaveLogNames) {
  EXPECT_EQ(std::string(MonitorShedPolicy::transitionName(MonitorShedTransition::Enter)), "enter");
  EXPECT_EQ(std::string(MonitorShedPolicy::transitionName(MonitorShedTransition::StepUp)), "step-up");
  EXPECT_EQ(std::string(MonitorShedPolicy::transitionName(MonitorShedTransition::StepDown)), "step-down");
  EXPECT_EQ(std::string(MonitorShedPolicy::transitionName(MonitorShedTransition::Exit)), "exit");
}

// ---------------------------------------------------------------------------
// MediaCore integration: the policy wired into renderSyntheticTick. These use a
// stub compositor whose preview pass SLEEPS 30ms — a sleep can only overrun the
// 16.7ms budget, never undershoot it, so the direction of every assertion here
// holds on a loaded machine. (The real-GPU rate measurement lives in the opt-in
// MonitorRenderFaultInjection timing tests.)

namespace {

class SlowPreviewCompositor final : public corevideo::modules::ICompositor {
 public:
  std::string rendererName() const override { return "slow-preview-test"; }
  corevideo::modules::ProgramFrame render(const corevideo::modules::CompositorRenderPlan& plan,
                                          const std::vector<corevideo::modules::VideoFrame>&) override {
    corevideo::modules::ProgramFrame frame;
    frame.frameNumber = ++programRenders;
    frame.renderPlanId = plan.renderPlanId;
    frame.gpuComposed = true;
    return frame;
  }
  corevideo::modules::ProgramFrameSharedTexture renderPreview(
      const corevideo::modules::CompositorRenderPlan&,
      const std::vector<corevideo::modules::VideoFrame>&) override {
    ++previewRenders;
    std::this_thread::sleep_for(std::chrono::milliseconds(previewSleepMs));
    corevideo::modules::ProgramFrameSharedTexture texture;
    if (!exportHandle) return {};  // a compositor with no shareable preview texture
    texture.sharedHandleHex = "0x5EED";
    texture.width = 1280;
    texture.height = 720;
    return texture;
  }
  int programRenders = 0;
  int previewRenders = 0;
  int previewSleepMs = 30;
  bool exportHandle = true;
};

corevideo::rpc::Json previewSceneCommand(const std::string& sceneId) {
  return corevideo::rpc::Json::Object{
      {"type", "set-preview-scene"},
      {"sceneId", sceneId},
      {"routes", corevideo::rpc::Json::Array{
                     corevideo::rpc::Json::Object{{"routeId", "pa"}, {"mode", "fixed"}, {"participantId", "spk-1"}},
                     corevideo::rpc::Json::Object{{"routeId", "pb"}, {"mode", "fixed"}, {"participantId", "spk-2"}},
                 }},
  };
}

struct SlowPreviewRig {
  SlowPreviewCompositor* compositor = nullptr;
  std::unique_ptr<corevideo::core::MediaCore> core;
};

SlowPreviewRig makeSlowPreviewRig(bool liveWorkers) {
  SlowPreviewRig rig;
  auto modules = corevideo::modules::createStubModules();
  auto compositor = std::make_unique<SlowPreviewCompositor>();
  rig.compositor = compositor.get();
  modules.compositor = std::move(compositor);
  rig.core = std::make_unique<corevideo::core::MediaCore>(std::move(modules));
  // With live workers, commands only publish state and the display tick is the
  // only renderer — exactly the running server.
  if (liveWorkers) rig.core->enableAudioOutputWorker();
  (void)rig.core->applyCommands(corevideo::rpc::Json::Array{previewSceneCommand("pvw-a")});
  return rig;
}

const corevideo::rpc::Json* monitorShedNode(const corevideo::rpc::Json& state) {
  const auto* evidence = state.get("realtimeEvidence");
  return evidence != nullptr ? evidence->get("monitorShed") : nullptr;
}

}  // namespace

TEST(MonitorShedIntegration, TheSnapshotPublishesTheHealthyStateUnconditionally) {
  corevideo::core::MediaCore core(corevideo::modules::createStubModules());
  const auto state = core.sessionState();
  const auto* shed = monitorShedNode(state);
  ASSERT_NE(shed, nullptr) << "realtimeEvidence.monitorShed must exist before any shedding";
  if (shed == nullptr) return;
  EXPECT_EQ(shed->getNumber("divisor"), 1);
  EXPECT_EQ(shed->getNumber("level"), 0);
  EXPECT_EQ(shed->getNumber("enteredCount"), 0);
  EXPECT_EQ(shed->getNumber("shedTicks"), 0);
  EXPECT_EQ(shed->getString("lastReason"), "none");
}

TEST(MonitorShedIntegration, ASlowPreviewPassIsShedAndItsIdentityNeverReadsEmpty) {
  auto rig = makeSlowPreviewRig(/*liveWorkers=*/true);
  auto& core = *rig.core;
  constexpr int kTicks = 30;
  int ticksWithoutPreviewIdentity = 0;
  for (int i = 0; i < kTicks; ++i) {
    core.renderDisplayTick();
    const auto state = core.sessionState();
    const auto* preview = state.get("previewSharedTexture");
    if (preview == nullptr || preview->getString("sharedHandleHex") != "0x5EED") {
      ++ticksWithoutPreviewIdentity;
    }
    (void)core.drainPreviewSharedTextureEvents();
    (void)core.drainProgramSharedTextureEvents();
  }
  EXPECT_EQ(rig.compositor->programRenders, kTicks) << "Program must render on EVERY tick";
  EXPECT_LT(rig.compositor->previewRenders, kTicks * 2 / 3)
      << "preview renders=" << rig.compositor->previewRenders << " over " << kTicks << " ticks";
  EXPECT_EQ(ticksWithoutPreviewIdentity, 0)
      << "a shed tick must republish the cached preview identity, never an empty one";
  const auto state = core.sessionState();
  const auto* shed = monitorShedNode(state);
  ASSERT_NE(shed, nullptr);
  if (shed == nullptr) return;
  EXPECT_GE(shed->getNumber("divisor"), 2);
  EXPECT_EQ(shed->getNumber("enteredCount"), 1);
  EXPECT_GT(shed->getNumber("shedTicks"), 0);
  EXPECT_EQ(shed->getString("lastReason"), "over-budget");
  // The observation behind the transition is published, so a monitor-bound shed
  // (this one: a 30ms preview pass) is distinguishable from a Program-bound one.
  EXPECT_GE(shed->getNumber("lastTransitionMonitorCycleMs"), 29.0);
  EXPECT_LT(shed->getNumber("lastTransitionProgramMs"), shed->getNumber("lastTransitionMonitorCycleMs"));
  EXPECT_GT(shed->getNumber("lastTransitionBudgetMs"), 16.0);
  EXPECT_LT(shed->getNumber("lastTransitionBudgetMs"), 17.0);
}

TEST(MonitorShedIntegration, AStructuralPreviewChangeRendersImmediatelyEvenWhileShedding) {
  auto rig = makeSlowPreviewRig(/*liveWorkers=*/true);
  auto& core = *rig.core;
  for (int i = 0; i < 12; ++i) core.renderDisplayTick();
  const auto shedState = core.sessionState();
  const auto* shed = monitorShedNode(shedState);
  ASSERT_TRUE(shed != nullptr && shed->getNumber("divisor") >= 2);
  // Two scene changes on two CONSECUTIVE ticks: at divisor >= 2 at most one of
  // those ticks is on the preview phase, so both rendering proves the
  // structural gate, not the cadence, let them through.
  for (const char* scene : {"pvw-b", "pvw-c"}) {
    const int before = rig.compositor->previewRenders;
    (void)core.applyCommands(corevideo::rpc::Json::Array{previewSceneCommand(scene)});
    core.renderDisplayTick();
    EXPECT_EQ(rig.compositor->previewRenders, before + 1) << "scene " << scene;
  }
}

// A compositor that exports no preview handle forces the preview pass on EVERY
// tick (nothing cached to republish), so shedding cannot touch it. Feeding its
// cost into the decision would pin the divisor up while shedding nothing.
TEST(MonitorShedIntegration, APreviewPassThatCannotBeShedNeverDrivesTheDivisor) {
  auto rig = makeSlowPreviewRig(/*liveWorkers=*/true);
  rig.compositor->exportHandle = false;
  auto& core = *rig.core;
  constexpr int kTicks = 12;
  for (int i = 0; i < kTicks; ++i) core.renderDisplayTick();
  EXPECT_EQ(rig.compositor->previewRenders, kTicks) << "an unsheddable pass still runs every tick";
  const auto state = core.sessionState();
  const auto* shed = monitorShedNode(state);
  ASSERT_NE(shed, nullptr);
  if (shed == nullptr) return;
  EXPECT_EQ(shed->getNumber("divisor"), 1);
  EXPECT_EQ(shed->getNumber("enteredCount"), 0);
}

TEST(MonitorShedIntegration, SyntheticFullTicksNeverFeedThePolicy) {
  // A direct caller with no render worker renders full synthetic ticks (CPU
  // readback + audio). They are not the paced display timeline, and letting
  // them shed would make ordinary unit tests timing-dependent.
  auto rig = makeSlowPreviewRig(/*liveWorkers=*/false);
  auto& core = *rig.core;
  const int before = rig.compositor->previewRenders;
  (void)core.applyCommands(corevideo::rpc::Json::Array{}, 330.0);  // ~10 synthetic ticks
  EXPECT_GT(rig.compositor->previewRenders - before, 5);
  const auto state = core.sessionState();
  const auto* shed = monitorShedNode(state);
  ASSERT_NE(shed, nullptr);
  if (shed == nullptr) return;
  EXPECT_EQ(shed->getNumber("divisor"), 1);
  EXPECT_EQ(shed->getNumber("enteredCount"), 0);
}
