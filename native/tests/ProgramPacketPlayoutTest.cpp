#include "core/ProgramPacketPlayout.h"
#include <gtest/gtest.h>
#include <thread>
#include <atomic>
using namespace corevideo::core;
namespace {
DeliveredProgramPacket packet(int depth,int64_t slot=0,bool gpu=false) {
  DeliveredProgramPacket p;
  p.showEpoch="show";p.sceneId="scene";p.clockEpoch="clock";
  p.showRevision=1;p.sceneRevision=2;p.renderRevision=3;p.clockGeneration=1;p.packetSequence=slot+1;
  p.productionSlot=slot;p.producedAtNs=slot*1000000000LL/60;
  p.productionDeadlineNs=ProgramPlayoutTimeline(2,0).deadlineNs(slot-1);
  p.deliveryDeadlineNs=ProgramPlayoutTimeline(depth,0).deadlineNs(slot);
  p.width=p.height=4;p.stride=16;
  if(gpu){p.stride=0;p.gpuLease=std::make_shared<const int>(1);p.gpuBytes=64;}
  else p.pixels=std::make_shared<const std::vector<uint8_t>>(64,128);
  p.sources.push_back({"camera","instance","helper","camera",1});return p;
}
}
TEST(ProgramPacketPlayout, BothDepthsUseExactFixedDeadlinesAndConsumeOnce) {
  for(int depth:{2,3}) {
    ProgramPacketPlayout playout({depth,"clock"});auto p=packet(depth);
    ASSERT_TRUE(validateDeliveredProgramPacket(p).valid);
    EXPECT_EQ(playout.enqueue(p),IProgramPlayout::Admission::Accepted);
    EXPECT_FALSE(playout.takeDue(p.deliveryDeadlineNs-1));
    auto due=playout.takeDue(p.deliveryDeadlineNs);ASSERT_TRUE(due);ASSERT_TRUE(due->packet);
    EXPECT_EQ(due->status,IProgramPlayout::Status::Delivered);
    EXPECT_EQ(due->packet->delivery->sequence,1ULL);
    EXPECT_TRUE(validateDeliveredProgramPacket(*due->packet).valid);
    EXPECT_FALSE(playout.takeDue(p.deliveryDeadlineNs));
    EXPECT_EQ(playout.enqueue(p),IProgramPlayout::Admission::Expired);
  }
}
TEST(ProgramPacketPlayout, StallSkipsExpiredSlotsWithoutReanchoring) {
  ProgramPacketPlayout playout({3,"clock"});
  playout.enqueue(packet(3,0));playout.enqueue(packet(3,1));playout.enqueue(packet(3,2));
  auto due=playout.takeDue(ProgramPlayoutTimeline(3,0).deadlineNs(2));ASSERT_TRUE(due);
  EXPECT_EQ(due->slot,2);EXPECT_EQ(due->skippedSlots,2);ASSERT_TRUE(due->packet);
  auto missing=playout.takeDue(ProgramPlayoutTimeline(3,0).deadlineNs(3));ASSERT_TRUE(missing);
  EXPECT_EQ(missing->status,IProgramPlayout::Status::Missing);
  EXPECT_FALSE(playout.takeDue(missing->deadlineNs));
  auto p=packet(3,4);playout.enqueue(p);
  auto late=playout.takeDue(p.deliveryDeadlineNs+1);ASSERT_TRUE(late);
  EXPECT_EQ(late->status,IProgramPlayout::Status::LateDelivery);
  EXPECT_EQ(late->deadlineNs,p.deliveryDeadlineNs);
}
TEST(ProgramPacketPlayout, GpuReadyMustMatchFullPacketAndOriginalDeadline) {
  ProgramPacketPlayout playout({2,"clock"});auto p=packet(2,0,true);playout.enqueue(p);
  EXPECT_EQ(playout.gpuReady("old",1,0,1,3,1),IProgramPlayout::Admission::StaleClock);
  EXPECT_EQ(playout.gpuReady("clock",1,0,2,3,1),IProgramPlayout::Admission::Conflict);
  EXPECT_EQ(playout.gpuReady("clock",1,0,1,4,1),IProgramPlayout::Admission::Conflict);
  EXPECT_EQ(playout.gpuReady("clock",1,0,1,3,p.deliveryDeadlineNs+1),IProgramPlayout::Admission::Accepted);
  auto due=playout.takeDue(p.deliveryDeadlineNs+2);ASSERT_TRUE(due);
  EXPECT_EQ(due->status,IProgramPlayout::Status::GpuLate);EXPECT_FALSE(due->packet);
  EXPECT_EQ(playout.gpuReady("clock",1,0,1,3,p.deliveryDeadlineNs),IProgramPlayout::Admission::Expired);
  auto next=packet(2,1,true);playout.enqueue(next);
  EXPECT_EQ(playout.gpuReady("clock",1,1,2,3,next.deliveryDeadlineNs),IProgramPlayout::Admission::Accepted);
  EXPECT_TRUE(playout.takeDue(next.deliveryDeadlineNs)->packet);
  auto absent=packet(2,2,true);playout.enqueue(absent);
  auto missingReady=playout.takeDue(absent.deliveryDeadlineNs);ASSERT_TRUE(missingReady);
  EXPECT_EQ(missingReady->status,IProgramPlayout::Status::GpuNotReady);
  EXPECT_FALSE(missingReady->packet);
}
TEST(ProgramPacketPlayout, BoundsClockAndPacketValidationFailClosed) {
  for(int depth:{0,1,4}) {bool threw=false;try{ProgramPacketPlayout invalid({depth,"clock"});}catch(...){threw=true;}EXPECT_TRUE(threw);}
  ProgramPacketPlayout playout({2,"clock"});
  auto mismatchedSequence=packet(2);mismatchedSequence.packetSequence=2;
  EXPECT_FALSE(validateDeliveredProgramPacket(mismatchedSequence).valid);
  auto gpuLayout=packet(2,0,true);gpuLayout.stride=16;
  EXPECT_FALSE(validateDeliveredProgramPacket(gpuLayout).valid);
  auto bad=packet(2);bad.sources.push_back(bad.sources[0]);EXPECT_FALSE(validateDeliveredProgramPacket(bad).valid);
  bad=packet(2);bad.deliveryDeadlineNs++;EXPECT_EQ(playout.enqueue(bad),IProgramPlayout::Admission::Invalid);
  bad=packet(2);bad.clockGeneration=2;EXPECT_EQ(playout.enqueue(bad),IProgramPlayout::Admission::StaleClock);
  bad=packet(2);bad.producedAtNs=bad.deliveryDeadlineNs+1;EXPECT_EQ(playout.enqueue(bad),IProgramPlayout::Admission::Expired);
  playout.enqueue(packet(2));playout.enqueue(packet(2,1));
  EXPECT_EQ(playout.enqueue(packet(2,2)),IProgramPlayout::Admission::Capacity);
  auto leases=playout.stop();EXPECT_EQ(leases.size(),2U);
  EXPECT_EQ(playout.enqueue(packet(2,2)),IProgramPlayout::Admission::Stopped);
  EXPECT_FALSE(playout.takeDue(1000000000));
}
TEST(ProgramPacketPlayout, ConcurrentDueConsumersDeliverOnlyOnce) {
  ProgramPacketPlayout playout({3,"clock"});auto p=packet(3);playout.enqueue(p);
  std::atomic<int> decisions{0};
  auto consume=[&]{if(playout.takeDue(p.deliveryDeadlineNs))++decisions;};
  std::thread a(consume),b(consume);a.join();b.join();EXPECT_EQ(decisions.load(),1);
}

TEST(ProgramPacketPlayout, ReplacementContinuesTransferredDeliverySequenceAndExhaustionIsExplicit) {
  ProgramPacketPlayout first({3,"clock"});auto p=packet(3);first.enqueue(p);
  auto delivered=first.takeDue(p.deliveryDeadlineNs);ASSERT_TRUE(delivered);ASSERT_TRUE(delivered->packet);
  const auto sequence=delivered->packet->delivery->sequence;
  auto missing=first.takeDue(ProgramPlayoutTimeline(3,0).deadlineNs(1));ASSERT_TRUE(missing);
  EXPECT_EQ(missing->status,IProgramPlayout::Status::Missing);
  first.stop();
  ProgramPacketPlayout::Config c{3,"clock"};c.initialSlot=2;c.initialDeliverySequence=sequence;
  ProgramPacketPlayout replacement(c);p=packet(3,2);replacement.enqueue(p);
  delivered=replacement.takeDue(p.deliveryDeadlineNs);ASSERT_TRUE(delivered);ASSERT_TRUE(delivered->packet);
  EXPECT_EQ(delivered->packet->delivery->sequence,sequence+1);
  EXPECT_EQ(delivered->packet->productionSlot,2);
  c.initialSlot=3;c.initialDeliverySequence=kProgramPacketMaxInteger;
  ProgramPacketPlayout exhausted(c);p=packet(3,3);exhausted.enqueue(p);
  auto terminal=exhausted.takeDue(p.deliveryDeadlineNs);ASSERT_TRUE(terminal);
  EXPECT_EQ(terminal->status,IProgramPlayout::Status::SequenceExhausted);EXPECT_FALSE(terminal->packet);
  EXPECT_FALSE(exhausted.takeDue(p.deliveryDeadlineNs));
  c.initialDeliverySequence=static_cast<uint64_t>(kProgramPacketMaxInteger)+1;
  bool threw=false;try{ProgramPacketPlayout invalid(c);}catch(...){threw=true;}EXPECT_TRUE(threw);
}
