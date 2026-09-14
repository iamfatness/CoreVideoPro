#include "core/ShadowExactSourceFrames.h"
#include <gtest/gtest.h>
using namespace corevideo;
namespace {
using Store = core::ShadowExactSourceFrames;
core::ExactRouteSourceRef ref(std::string kind = "camera", std::string epoch = "epoch", uint64_t generation = 1) {
  return {kind, kind + "-instance", epoch, kind, generation};
}
modules::VideoFrame frame(const core::ExactRouteSourceRef& id, uint64_t fence = 1, uint64_t sequence = 1) {
  modules::VideoFrame f;
  f.participantId = "42"; f.width = f.i420Width = 4; f.height = f.i420Height = 4;
  f.i420 = std::make_shared<const std::vector<uint8_t>>(24, 128);
  modules::SourceFrameEvidence e;
  e.identity = {id.sourceId, id.instanceId, id.processEpoch, static_cast<int64_t>(id.generation)};
  e.kind = id.kind == "camera" ? modules::SourceFrameEvidence::Kind::Camera : modules::SourceFrameEvidence::Kind::Share;
  e.publicationFence = fence; e.publicationSequence = sequence; e.observedNs = 100; e.payload = f.i420;
  f.exactSourceEvidence = std::make_shared<const modules::SourceFrameEvidence>(e);
  return f;
}
}
TEST(ShadowExactSourceFrames, IndependentCameraShareAndImmutableLeases) {
  Store store; auto camera = ref(); auto share = ref("share");
  ASSERT_EQ(store.reconcile("epoch", 1, {{camera,1,true},{share,1,true}}), Store::Status::Applied);
  auto c = frame(camera); auto s = frame(share);
  ASSERT_EQ(store.publish(c), Store::Status::Applied);
  ASSERT_EQ(store.publish(s), Store::Status::Applied);
  auto held = store.resolve(camera, 100).frame; ASSERT_TRUE(held);
  EXPECT_EQ(held->i420, c.i420);
  EXPECT_EQ(store.resolve(share, 100).frame->i420, s.i420);
  EXPECT_EQ(store.resolve(camera, 101).status, Store::Status::Expired);
  auto newer = frame(camera, 1, 2);
  EXPECT_EQ(store.publish(newer), Store::Status::Applied);
  EXPECT_EQ(store.publish(c), Store::Status::Stale);
  EXPECT_EQ(held->i420, c.i420);
  auto wrong = camera; wrong.instanceId = "other";
  EXPECT_EQ(store.resolve(wrong, 0).status, Store::Status::Missing);
}
TEST(ShadowExactSourceFrames, OffOnAndHelperReplacementFenceOldPublications) {
  Store store; auto id = ref(); auto old = frame(id);
  store.reconcile("epoch",1,{{id,1,true}}); store.publish(old);
  auto held = store.resolve(id,0).frame;
  store.reconcile("epoch",2,{{id,2,false}});
  store.reconcile("epoch",3,{{id,3,true}});
  EXPECT_EQ(store.publish(old),Store::Status::Stale);
  EXPECT_EQ(store.resolve(id,0).status,Store::Status::Missing);
  EXPECT_EQ(store.publish(frame(id,3,2)),Store::Status::Applied);
  auto replacement = ref("camera","new-epoch",1);
  store.reconcile("new-epoch",1,{{replacement,1,true}});
  EXPECT_EQ(store.publish(old),Store::Status::Stale);
  EXPECT_EQ(store.reconcile("epoch",4,{{id,4,true}}),Store::Status::Stale);
  EXPECT_EQ(store.resolve(id,0).status,Store::Status::Missing);
  EXPECT_EQ(held->i420,old.i420);
}
TEST(ShadowExactSourceFrames, CapacityAndMalformedEvidenceFailClosed) {
  Store store({2,1,8192,1}); auto c = ref(); auto s = ref("share");
  store.reconcile("epoch",1,{{c,1,true},{s,1,true}});
  EXPECT_EQ(store.publish(frame(c)),Store::Status::Applied);
  EXPECT_EQ(store.publish(frame(s)),Store::Status::Capacity);
  EXPECT_EQ(store.resolve(s,0).status,Store::Status::Missing);
  auto detached = frame(c); detached.i420 = std::make_shared<const std::vector<uint8_t>>(24,0);
  EXPECT_EQ(store.publish(detached),Store::Status::Invalid);
  EXPECT_EQ(store.reconcile("epoch",2,{{c,1,true},{c,1,true}}),Store::Status::Invalid);
  EXPECT_EQ(store.resolve(c,0).status,Store::Status::Missing);
}
TEST(ShadowExactSourceFrames, DepartureCannotReuseGenerationOrRegressFence) {
  Store store; auto c = ref();
  store.reconcile("epoch",1,{{c,3,true}}); store.publish(frame(c,3));
  store.reconcile("epoch",2,{});
  EXPECT_EQ(store.reconcile("epoch",3,{{c,4,true}}),Store::Status::Stale);
  c.generation = 2;
  EXPECT_EQ(store.reconcile("epoch",4,{{c,1,true}}),Store::Status::Applied);
  EXPECT_EQ(store.reconcile("epoch",5,{{c,0,true}}),Store::Status::Stale);
  EXPECT_EQ(store.resolve(c,0).status,Store::Status::Missing);
}
