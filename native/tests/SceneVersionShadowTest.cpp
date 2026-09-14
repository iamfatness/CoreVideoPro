#include "core/SceneVersionShadow.h"
#include "core/ShowPreparationTransaction.h"
#include <gtest/gtest.h>
#include <stdexcept>
using namespace corevideo::core;
namespace {
SceneVersionRef version(uint64_t v) { return {"show", "same-scene", 1, v}; }
ShowSceneIntent content(int x) {
  ShowSceneIntent scene; ShowLayerIntent layer; layer.x = x;
  scene.routes.emplace("layer", layer); scene.layerOrder = {"layer"}; return scene;
}
ShowStateSnapshot show() {
  ShowStateSnapshot snapshot; snapshot.authorityEpoch = "show"; snapshot.revision = 4;
  // The legacy map intentionally disagrees: explicit versioned buses must not use it.
  snapshot.data.scenes.emplace("same-scene", content(999));
  snapshot.data.program = snapshot.data.preview = ShowEntityRef{"same-scene", 1}; return snapshot;
}
SourceRegistry::Snapshot registry() { SourceRegistry::Snapshot r; r.registryEpoch = "registry"; return r; }
struct NeverCalled final : IShowResourcePreparer {
  ShowPreparationEvidence poll(const ShowPreparationToken&, const ShowPreparationRequirement&) override {
    throw std::runtime_error("No real resource preparation is allowed in this test");
  }
  void cancel(const ShowPreparationToken&) noexcept override {}
};
}
TEST(SceneVersionShadow, SameIdProgramRemainsImmutableWhilePreviewEdits) {
  SceneVersionStore store("show");
  store.publish(version(1), content(10)); store.publish(version(2), content(20), version(1));
  auto first = SceneVersionShadowEvidence::capture(store, show(), registry(), {0}, version(1), version(2));
  ASSERT_TRUE(first);
  EXPECT_EQ(first->plans->render.program.layers.at(0).intent.x, 10);
  EXPECT_EQ(first->plans->render.preview.layers.at(0).intent.x, 20);
  store.publish(version(3), content(30), version(2));
  auto next = SceneVersionShadowEvidence::capture(store, show(), registry(), {0}, version(1), version(3));
  ASSERT_TRUE(next);
  EXPECT_EQ(next->plans->render.program.versionLease.get(), first->plans->render.program.versionLease.get());
  EXPECT_EQ(first->plans->render.preview.layers.at(0).intent.x, 20);
  EXPECT_EQ(next->plans->render.preview.layers.at(0).intent.x, 30);
  EXPECT_FALSE(first->plans->render.stamp == next->plans->render.stamp);
}
TEST(SceneVersionShadow, MissingStaleAndWrongEpochCannotFallBackToLegacyScene) {
  SceneVersionStore store("show"); store.publish(version(1), content(10));
  EXPECT_FALSE(SceneVersionShadowEvidence::capture(store, show(), registry(), {0}, version(1), version(2)));
  auto wrong = version(1); wrong.authorityEpoch = "old";
  EXPECT_FALSE(SceneVersionShadowEvidence::capture(store, show(), registry(), {0}, wrong, version(1)));
  auto retained = SceneVersionShadowEvidence::capture(store, show(), registry(), {0}, version(1), version(1));
  store.erase(version(1));
  EXPECT_FALSE(SceneVersionShadowEvidence::capture(store, show(), registry(), {0}, version(1), version(1)));
  ASSERT_TRUE(retained); EXPECT_EQ(retained->plans->render.program.layers.at(0).intent.x, 10);
}
TEST(SceneVersionShadow, ExplicitBlankDoesNotUseMutableBusAndMissingLeasePaintsNoLayers) {
  SceneVersionStore store("show"); store.publish(version(1), content(10));
  auto blank = SceneVersionShadowEvidence::capture(store, show(), registry(), {0}, {}, {});
  ASSERT_TRUE(blank); EXPECT_EQ(blank->plans->render.program.status, PlannedBindingStatus::Blank);
  auto missing = generateShowPlans(show(), registry(), {0}, VersionedSceneBindings{{version(1), {}}, {}});
  EXPECT_EQ(missing->render.program.status, PlannedBindingStatus::Missing);
  EXPECT_TRUE(missing->render.program.layers.empty());
}
TEST(SceneVersionShadow, PreparationCertificateRetainsPinnedVersionsAndCapacityIsExplicit) {
  SceneVersionStore store("show", {2, 4, 4, 1024 * 1024});
  store.publish(version(1), content(10)); store.publish(version(2), content(20), version(1));
  auto evidence = SceneVersionShadowEvidence::capture(store, show(), registry(), {0}, version(1), version(2));
  ASSERT_TRUE(evidence);
  std::shared_ptr<const PreparedShowCertificate> certificate;
  {
    ShowPreparationTransaction transaction({"show", 4, 1}, std::make_shared<NeverCalled>());
    ShowPreparationPlan plan{{"show", 4, 1}, "plan", 5, {}, evidence->plans->render.stamp, evidence};
    EXPECT_EQ(transaction.begin(plan).status, ShowPreparationTransaction::Status::Preparing);
    certificate = transaction.poll().certificate; ASSERT_TRUE(certificate);
  }
  evidence.reset();
  EXPECT_EQ(store.evict(version(1)), SceneVersionStore::Status::Pinned);
  EXPECT_EQ(store.publish(version(3), content(30), version(2)).status, SceneVersionStore::Status::Capacity);
  EXPECT_EQ(certificate->plan->sceneVersions->plans->render.program.layers.at(0).intent.x, 10);
  certificate.reset();
  EXPECT_EQ(store.evict(version(1)), SceneVersionStore::Status::Applied);
  EXPECT_EQ(store.publish(version(3), content(30), version(2)).status, SceneVersionStore::Status::Applied);
}
TEST(SceneVersionShadow, PreparationRejectsDifferentBasisAndVersionStamp) {
  SceneVersionStore store("show"); store.publish(version(1), content(10));
  auto evidence = SceneVersionShadowEvidence::capture(store, show(), registry(), {0}, version(1), version(1));
  ASSERT_TRUE(evidence);
  ShowPreparationTransaction transaction({"show", 4, 1}, std::make_shared<NeverCalled>());
  ShowPreparationPlan plan{{"show", 4, 1}, "plan", 5, {}, evidence->plans->render.stamp, evidence};
  plan.stamp.eligibilityIdentity += "different-version";
  EXPECT_EQ(transaction.begin(plan).status, ShowPreparationTransaction::Status::Invalid);
  EXPECT_FALSE(transaction.snapshot().certificate);
}
TEST(SceneVersionShadow, RecapturedIdenticalEvidenceReplaysSameReadyCertificate) {
  SceneVersionStore store("show"); store.publish(version(1), content(10));
  auto evidence = SceneVersionShadowEvidence::capture(store, show(), registry(), {0}, version(1), version(1));
  ShowPreparationTransaction transaction({"show", 4, 1}, std::make_shared<NeverCalled>());
  ShowPreparationPlan plan{{"show", 4, 1}, "plan", 5, {}, evidence->plans->render.stamp, evidence};
  transaction.begin(plan); const auto first = transaction.poll(); ASSERT_TRUE(first.certificate);
  plan.sceneVersions = SceneVersionShadowEvidence::capture(store, show(), registry(), {0}, version(1), version(1));
  EXPECT_FALSE(plan.sceneVersions.get() == evidence.get());
  const auto replay = transaction.begin(plan);
  EXPECT_EQ(replay.status, ShowPreparationTransaction::Status::Ready);
  EXPECT_EQ(replay.token.transactionGeneration, first.token.transactionGeneration);
  EXPECT_EQ(replay.certificate.get(), first.certificate.get());
}
