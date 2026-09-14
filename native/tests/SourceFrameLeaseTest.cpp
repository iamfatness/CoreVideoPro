#include "core/SourceFrameLease.h"
#include <gtest/gtest.h>
using namespace corevideo::core;
namespace {
using Lease = SourceFrameLease;
corevideo::modules::VideoFrame frame(std::string source = "camera", bool share = false) {
  corevideo::modules::VideoFrame f; f.participantId = "42";
  f.width = f.naturalWidth = f.i420Width = 4; f.height = f.naturalHeight = f.i420Height = 4;
  f.i420 = std::make_shared<const std::vector<uint8_t>>(24, 128);
  f.frameId = 77; f.timestampMs = 91; f.i420FullRange = false; f.i420Bt601 = true;
  corevideo::modules::SourceFrameEvidence evidence;
  evidence.identity = {source, source + "-instance", "epoch", 2};
  evidence.kind = share ? corevideo::modules::SourceFrameEvidence::Kind::Share : corevideo::modules::SourceFrameEvidence::Kind::Camera;
  evidence.publicationSequence = 9; evidence.publicationFence = 3; evidence.observedNs = 100;
  evidence.payload = f.i420;
  f.exactSourceEvidence = std::make_shared<const corevideo::modules::SourceFrameEvidence>(evidence);
  return f;
}
Lease::CurrentSource current(std::string id = "camera", bool share = false) {
  return {{id, id + "-instance", "epoch", share ? "share" : "camera", 2}, 3, true};
}
}
TEST(SourceFrameLease, SharesPixelsAndPreservesCompleteFrameAndFormat) {
  auto f = frame(); const auto result = Lease::create(f, current(), 100, 100);
  ASSERT_EQ(result.status, Lease::Status::Ready); ASSERT_TRUE(result.lease);
  EXPECT_EQ(result.lease->frame.i420.get(), f.i420.get());
  EXPECT_TRUE(result.lease->frame.exactSourceEvidence.get() != f.exactSourceEvidence.get());
  EXPECT_EQ(result.lease->frame.timestampMs, 91); EXPECT_EQ(result.lease->frame.frameId, 77);
  EXPECT_EQ(result.lease->frame.naturalWidth, 4); EXPECT_FALSE(result.lease->format.fullRange);
  EXPECT_TRUE(result.lease->format.bt601); EXPECT_EQ(result.lease->publicationSequence, 9ULL);
  EXPECT_EQ(result.lease->observedNs, 100); EXPECT_EQ(result.lease->publicationFence, 3ULL);
}
TEST(SourceFrameLease, RejectsHelperReplacementGenerationAndOffOnFence) {
  auto f = frame();
  for (int mutation = 0; mutation < 5; ++mutation) {
    auto c = current();
    if (mutation == 0) c.identity.processEpoch = "new-helper";
    if (mutation == 1) c.identity.instanceId = "replacement";
    if (mutation == 2) ++c.identity.generation;
    if (mutation == 3) ++c.publicationFence;
    if (mutation == 4) c.available = false;
    EXPECT_EQ(Lease::create(f, c, 0, 100).status, Lease::Status::Stale);
  }
  EXPECT_EQ(Lease::create(f, current(), 101, 102).status, Lease::Status::Stale);
  EXPECT_EQ(Lease::create(f, current(), 0, 99).status, Lease::Status::Stale);
}
TEST(SourceFrameLease, RejectsDetachedOwnershipMalformedFormatAndOversizedPayload) {
  auto f = frame(); f.i420 = std::make_shared<const std::vector<uint8_t>>(24, 1);
  EXPECT_EQ(Lease::create(f,current(),0,100).status,Lease::Status::Invalid);
  f=frame(); f.i420Width=3;
  EXPECT_EQ(Lease::create(f,current(),0,100).status,Lease::Status::Invalid);
  f=frame(); f.i420Height=32768;
  EXPECT_EQ(Lease::create(f,current(),0,100).status,Lease::Status::Invalid);
  f=frame();
  EXPECT_EQ(Lease::create(f,current(),0,100,23).status,Lease::Status::Capacity);
  auto proof = std::make_shared<corevideo::modules::SourceFrameEvidence>(*f.exactSourceEvidence);
  proof->publicationSequence=0; f.exactSourceEvidence=proof;
  EXPECT_EQ(Lease::create(f,current(),0,100).status,Lease::Status::Invalid);
}
TEST(SourceFrameLease, SetKeepsCameraShareDistinctAndEnforcesUniqueBounds) {
  auto camera = Lease::create(frame(),current(),0,100).lease;
  auto share = Lease::create(frame("share",true),current("share",true),0,100).lease;
  ASSERT_TRUE(camera); ASSERT_TRUE(share);
  auto set = SourceFrameLeaseSet::create("epoch",{share,camera},{2,8192});
  ASSERT_EQ(set.status,Lease::Status::Ready);
  EXPECT_EQ(set.leases->frames[0]->identity.kind,"camera");
  EXPECT_EQ(set.leases->frames[1]->identity.kind,"share");
  EXPECT_EQ(SourceFrameLeaseSet::create("epoch",{camera,camera},{2,8192}).status,Lease::Status::Invalid);
  EXPECT_EQ(SourceFrameLeaseSet::create("epoch",{camera,share},{1,8192}).status,Lease::Status::Capacity);
  EXPECT_EQ(SourceFrameLeaseSet::create("epoch",{camera,share},{2,47}).status,Lease::Status::Capacity);
  EXPECT_EQ(SourceFrameLeaseSet::create("new-helper",{camera},{2,8192}).status,Lease::Status::Invalid);
}
TEST(SourceFrameLease, HeldLeaseSurvivesProducerAndSetRetirement) {
  Lease::Ptr held; std::weak_ptr<const std::vector<uint8_t>> pixels;
  {
    auto f=frame(); pixels=f.i420;
    auto lease=Lease::create(f,current(),0,100).lease;
    auto set=SourceFrameLeaseSet::create("epoch",{lease},{1,8192});
    held=set.leases->frames[0];
  }
  EXPECT_FALSE(pixels.expired()); EXPECT_EQ(held->frame.i420->at(0),128);
  held.reset(); EXPECT_TRUE(pixels.expired());
}

TEST(SourceFrameLease, BgraStrideAndOwnerIdentityAreValidatedWithoutPixelCopy) {
  auto f=frame(); f.i420.reset(); f.i420Width=f.i420Height=0;
  f.pixels=std::make_shared<const std::vector<uint8_t>>(80,12);
  f.pixelWidth=f.pixelHeight=4; f.pixelStride=20;
  auto proof=std::make_shared<corevideo::modules::SourceFrameEvidence>(*f.exactSourceEvidence);
  proof->payload=f.pixels; f.exactSourceEvidence=proof;
  auto ready=Lease::create(f,current(),0,100);
  ASSERT_TRUE(ready.lease); EXPECT_EQ(ready.lease->format.encoding,Lease::Format::Encoding::Bgra);
  EXPECT_EQ(ready.lease->frame.pixelStride,20); EXPECT_EQ(ready.lease->frame.pixels.get(),f.pixels.get());
  f.pixelStride=2147483647;
  EXPECT_EQ(Lease::create(f,current(),0,100).status,Lease::Status::Invalid);
  f.pixelStride=20;
  // Same address but a separate non-owning control block is not the producer lease.
  proof=std::make_shared<corevideo::modules::SourceFrameEvidence>(*proof);
  proof->payload=std::shared_ptr<const std::vector<uint8_t>>(f.pixels.get(),[](const auto*){});
  f.exactSourceEvidence=proof;
  EXPECT_EQ(Lease::create(f,current(),0,100).status,Lease::Status::Invalid);
}

TEST(SourceFrameLease, FreezesCompactProofAndChargesAllRetainedMetadata) {
  auto f=frame(); f.participantId.reserve(1024*1024);
  auto proof=std::make_shared<corevideo::modules::SourceFrameEvidence>(*f.exactSourceEvidence);
  proof->identity.sourceId.reserve(1024*1024);
  proof->identity.instanceId.reserve(1024*1024);
  proof->identity.processEpoch.reserve(1024*1024);
  std::weak_ptr<const corevideo::modules::SourceFrameEvidence> caller=proof;
  f.exactSourceEvidence=proof;
  auto result=Lease::create(f,current(),0,100,8192);
  ASSERT_TRUE(result.lease);
  EXPECT_TRUE(result.lease->retainedBytes>f.i420->capacity());
  EXPECT_TRUE(result.lease->frame.participantId.capacity()<1024);
  EXPECT_TRUE(result.lease->frame.exactSourceEvidence->identity.sourceId.capacity()<1024);
  EXPECT_TRUE(result.lease->frame.exactSourceEvidence->identity.instanceId.capacity()<1024);
  EXPECT_TRUE(result.lease->frame.exactSourceEvidence->identity.processEpoch.capacity()<1024);
  EXPECT_EQ(result.lease->frame.i420.get(),f.i420.get());
  EXPECT_EQ(Lease::create(f,current(),0,100,result.lease->retainedBytes-1).status,Lease::Status::Capacity);
  auto set=SourceFrameLeaseSet::create("epoch",{result.lease},{1,8192}); ASSERT_TRUE(set.leases);
  EXPECT_TRUE(set.leases->retainedBytes>result.lease->retainedBytes);
  EXPECT_EQ(SourceFrameLeaseSet::create("epoch",{result.lease},{1,set.leases->retainedBytes-1}).status,Lease::Status::Capacity);
  f.exactSourceEvidence.reset(); proof.reset(); EXPECT_TRUE(caller.expired());
}
