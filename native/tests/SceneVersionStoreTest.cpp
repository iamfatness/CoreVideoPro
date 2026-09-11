#include "core/SceneVersionStore.h"
#include <gtest/gtest.h>
#include <thread>
#include <atomic>
using namespace corevideo::core;
namespace {
using Store=SceneVersionStore;
SceneVersionRef ref(uint64_t version=1,uint64_t generation=1,std::string epoch="show") {return {std::move(epoch),"scene",generation,version};}
ShowSceneIntent scene(int x=0) {
  ShowSceneIntent s;s.label="Scene";ShowLayerIntent layer;layer.x=x;
  layer.target={ShowRouteKind::FixedSource,ShowSourceRef{"source","instance","process",7},{}};
  s.routes.emplace("layer",layer);s.layerOrder={"layer"};return s;
}
}
TEST(SceneVersionStore, SameScenePreviewEditDoesNotChangeProgramOrRetainedFrame) {
  Store store("show");auto mutableScene=scene();
  auto program=store.publish(ref(),mutableScene);ASSERT_EQ(program.status,Store::Status::Applied);
  mutableScene.routes.at("layer").x=100;
  auto preview=store.publish(ref(2),mutableScene,ref());ASSERT_EQ(preview.status,Store::Status::Applied);
  EXPECT_EQ(program.lease->scene.routes.at("layer").x,0);
  EXPECT_EQ(preview.lease->scene.routes.at("layer").x,100);
  EXPECT_EQ(store.resolve(ref()).lease.get(),program.lease.get());
  auto retainedFrame=program.lease;program.lease=preview.lease;
  EXPECT_EQ(retainedFrame->ref.version,1ULL);EXPECT_EQ(program.lease->ref.version,2ULL);
  auto laterPreview=store.publish(ref(3),scene(200),ref(2));ASSERT_TRUE(laterPreview.lease);
  EXPECT_EQ(program.lease->scene.routes.at("layer").x,100);
  EXPECT_EQ(laterPreview.lease->scene.routes.at("layer").x,200);
}
TEST(SceneVersionStore, BatchCapacityFailureLeavesNeitherDefinitionNorHead) {
  Store store("show", {1, 4, 4, 1024 * 1024});
  const auto rejected = store.publishBatch({{ref(), scene(10), {}}, {ref(2), scene(20), ref()}});
  EXPECT_EQ(rejected.status, Store::Status::Capacity); EXPECT_TRUE(rejected.leases.empty());
  EXPECT_EQ(store.head("scene").status, Store::Status::NotFound);
  EXPECT_FALSE(store.resolve(ref()).lease); EXPECT_FALSE(store.resolve(ref(2)).lease);
  EXPECT_EQ(store.publish(ref(), scene(99)).status, Store::Status::Applied);
}
TEST(SceneVersionStore, BatchConflictRollsBackEarlierEditAndReplayKeepsHead) {
  Store store("show"); store.publish(ref(), scene(10));
  auto secondScene = ref(); secondScene.sceneId = "other";
  store.publish(secondScene, scene(20));
  const auto rejected = store.publishBatch({{ref(2), scene(30), ref()}, {secondScene, scene(99), {}}});
  EXPECT_EQ(rejected.status, Store::Status::Conflict);
  EXPECT_EQ(store.head("scene").lease->ref, ref()); EXPECT_FALSE(store.resolve(ref(2)).lease);
  auto admitted = store.publishBatch({{ref(), scene(10), {}}, {ref(2), scene(30), ref()}});
  EXPECT_EQ(admitted.status, Store::Status::Applied); EXPECT_EQ(admitted.leases.size(), 2U);
  auto replay = store.publishBatch({{ref(), scene(10), {}}, {ref(2), scene(30), ref()}});
  EXPECT_EQ(replay.status, Store::Status::Unchanged);
  EXPECT_EQ(store.head("scene").lease->ref, ref(2));
  EXPECT_EQ(replay.leases[0].get(), admitted.leases[0].get());
}
TEST(SceneVersionStore, ExactReplayDoesNotMoveHeadAndConflictingPayloadIsRejected) {
  Store store("show");store.publish(ref(),scene());store.publish(ref(2),scene(5),ref());
  EXPECT_EQ(store.publish(ref(),scene()).status,Store::Status::Unchanged);
  EXPECT_EQ(store.head("scene").lease->ref.version,2ULL);
  EXPECT_EQ(store.publish(ref(),scene(3)).status,Store::Status::Conflict);
  EXPECT_EQ(store.publish(ref(3),scene(3),ref()).status,Store::Status::Conflict);
  EXPECT_EQ(store.publish(ref(5),scene(5),ref(2)).status,Store::Status::Applied);
}
TEST(SceneVersionStore, DeleteRecreateAndRestartFenceLookupButPreserveExistingLeases) {
  Store store("show");auto held=store.publish(ref(),scene()).lease;
  EXPECT_EQ(store.erase(ref()),Store::Status::Applied);
  EXPECT_EQ(store.resolve(ref()).status,Store::Status::Stale);
  EXPECT_EQ(store.publish(ref(2),scene()).status,Store::Status::Stale);
  EXPECT_EQ(store.publish(ref(1,2),scene(9)).status,Store::Status::Applied);
  EXPECT_EQ(store.resolve(ref()).status,Store::Status::Stale);
  EXPECT_EQ(held->scene.routes.at("layer").x,0);
  EXPECT_EQ(store.restart("show","new-show"),Store::Status::Applied);
  EXPECT_EQ(store.resolve(ref(1,2)).status,Store::Status::Stale);
  EXPECT_EQ(store.publish(ref(1,1,"new-show"),scene()).status,Store::Status::Applied);
  EXPECT_EQ(store.restart("new-show","show"),Store::Status::Stale);
  EXPECT_EQ(held->ref.authorityEpoch,"show");
}
TEST(SceneVersionStore, CapacityCannotEvictPinnedFramesOrCurrentHead) {
  Store store("show",{2,2,1,1024*1024});auto old=store.publish(ref(),scene());
  store.publish(ref(2),scene(5),ref());
  EXPECT_EQ(store.publish(ref(3),scene(6),ref(2)).status,Store::Status::Capacity);
  EXPECT_EQ(store.evict(ref()),Store::Status::Pinned);
  EXPECT_EQ(store.evict(ref(2)),Store::Status::Pinned);
  old.lease.reset();EXPECT_EQ(store.evict(ref()),Store::Status::Applied);
  EXPECT_EQ(store.publish(ref(),scene()).status,Store::Status::Conflict);
  EXPECT_EQ(store.publish(ref(3),scene(6),ref(2)).status,Store::Status::Applied);
  EXPECT_EQ(store.restart("show","next"),Store::Status::Applied);
  EXPECT_EQ(store.restart("next","third"),Store::Status::Capacity);
}
TEST(SceneVersionStore, InvalidDefinitionsAndByteCapacityDoNotPublish) {
  Store store("show");auto invalid=scene();invalid.layerOrder={"missing"};
  EXPECT_EQ(store.publish(ref(),invalid).status,Store::Status::Invalid);
  invalid=scene();invalid.routes.at("layer").target.source->instanceId.clear();
  EXPECT_EQ(store.publish(ref(),invalid).status,Store::Status::Invalid);
  EXPECT_EQ(store.publish(ref(0),scene()).status,Store::Status::Invalid);
  EXPECT_EQ(store.publish(ref(1,9007199254740992ULL),scene()).status,Store::Status::Invalid);
  EXPECT_EQ(store.head("scene").status,Store::Status::NotFound);
  Store tiny("show",{2,2,2,1});
  EXPECT_EQ(tiny.publish(ref(),scene()).status,Store::Status::Capacity);
  EXPECT_EQ(tiny.head("scene").status,Store::Status::NotFound);
}
TEST(SceneVersionStore, ConcurrentEditsHaveOneHeadWinner) {
  Store store("show");store.publish(ref(),scene());std::atomic<int> applied{0};
  auto edit=[&](int version){if(store.publish(ref(version),scene(version),ref()).status==Store::Status::Applied)++applied;};
  std::thread a(edit,2),b(edit,3);a.join();b.join();
  EXPECT_EQ(applied.load(),1);EXPECT_EQ(store.resolve(ref()).lease->scene.routes.at("layer").x,0);
}
