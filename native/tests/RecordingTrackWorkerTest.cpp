#include "modules/RecordingTrackWorker.h"
#include <gtest/gtest.h>
#include <future>
#include <vector>

using corevideo::modules::RecordingTrackWorker;

TEST(RecordingTrackWorker, SlowTrackDoesNotBlockAnotherAndStopDrainsAcceptedAudioVideo) {
  std::promise<void> entered, release, fastWritten;
  auto gate = release.get_future().share();
  std::vector<int> applied;
  RecordingTrackWorker slow([] {}, [&] { applied.push_back(4); });
  RecordingTrackWorker fast([] {}, [] {});
  slow.post(RecordingTrackWorker::Kind::Video, [&] {
    entered.set_value(); gate.wait(); applied.push_back(1);
  });
  entered.get_future().wait();
  slow.post(RecordingTrackWorker::Kind::Audio, [&] { applied.push_back(2); });
  slow.post(RecordingTrackWorker::Kind::Video, [&] { applied.push_back(3); });
  fast.post(RecordingTrackWorker::Kind::Video, [&] { fastWritten.set_value(); });
  const auto progressed = fastWritten.get_future().wait_for(std::chrono::seconds(2));
  slow.close(); fast.close();
  const bool acceptedAfterStop = slow.post(RecordingTrackWorker::Kind::Audio, [] {});
  release.set_value(); slow.join(); fast.join();
  EXPECT_EQ(progressed, std::future_status::ready);
  EXPECT_FALSE(acceptedAfterStop);
  ASSERT_EQ(applied.size(), 4u);
  for (int i = 0; i < 4; ++i) EXPECT_EQ(applied[i], i + 1);
  EXPECT_EQ(slow.evidence().droppedVideo, 0u);
  EXPECT_EQ(slow.evidence().droppedAudio, 0u);
}

TEST(RecordingTrackWorker, OverflowIsBoundedAndCannotEraseAcceptedTailOrOtherMediaKind) {
  std::promise<void> entered, release;
  auto gate = release.get_future().share();
  int video = 0, audio = 0;
  RecordingTrackWorker worker([] {}, [] {}, 2, 2);
  worker.post(RecordingTrackWorker::Kind::Video, [&] { entered.set_value(); gate.wait(); ++video; });
  entered.get_future().wait();
  const bool v1 = worker.post(RecordingTrackWorker::Kind::Video, [&] { ++video; });
  const bool v2 = worker.post(RecordingTrackWorker::Kind::Video, [&] { ++video; });
  const bool overflow = worker.post(RecordingTrackWorker::Kind::Video, [&] { video += 100; });
  const bool a1 = worker.post(RecordingTrackWorker::Kind::Audio, [&] { ++audio; });
  const bool a2 = worker.post(RecordingTrackWorker::Kind::Audio, [&] { ++audio; });
  worker.close(); release.set_value(); worker.join();
  EXPECT_TRUE(v1); EXPECT_TRUE(v2); EXPECT_TRUE(a1); EXPECT_TRUE(a2); EXPECT_FALSE(overflow);
  EXPECT_EQ(video, 3); EXPECT_EQ(audio, 2);
  EXPECT_EQ(worker.evidence().droppedVideo, 1u);
  EXPECT_EQ(worker.evidence().droppedAudio, 0u);
  EXPECT_EQ(worker.evidence().queuedVideo, 0u);
}

TEST(RecordingTrackWorker, WriteExceptionIsReportedAndFinalizationStillRuns) {
  bool finalized = false;
  RecordingTrackWorker worker([] {}, [&] { finalized = true; });
  worker.post(RecordingTrackWorker::Kind::Video, [] { throw std::runtime_error("disk failure"); });
  worker.close(); worker.join();
  EXPECT_TRUE(finalized);
  EXPECT_EQ(worker.evidence().error, "disk failure");
}

TEST(RecordingTrackWorker, StartupBurstDrainsBeforeTheSteadyLimitTakesOver) {
  std::promise<void> releaseStartup, startupEntered, startupDrained;
  auto startupGate = releaseStartup.get_future().share();
  RecordingTrackWorker worker([&] { startupEntered.set_value(); startupGate.wait(); }, [] {}, 2, 2, 4);
  startupEntered.get_future().wait();
  int count = 0;
  for (int i=0;i<4;++i) {
    EXPECT_TRUE(worker.post(RecordingTrackWorker::Kind::Video, [&, i] {
      ++count;
      if(i==0) worker.finishStartup();
      if(i==3) startupDrained.set_value();
    }));
  }
  EXPECT_FALSE(worker.post(RecordingTrackWorker::Kind::Video, [] {}));
  releaseStartup.set_value();
  const auto drained = startupDrained.get_future().wait_for(std::chrono::seconds(2));
  std::promise<void> steadyEntered, releaseSteady;
  auto steadyGate = releaseSteady.get_future().share();
  worker.post(RecordingTrackWorker::Kind::Video, [&] { steadyEntered.set_value(); steadyGate.wait(); });
  const auto entered = steadyEntered.get_future().wait_for(std::chrono::seconds(2));
  const bool one = worker.post(RecordingTrackWorker::Kind::Video, [&] { ++count; });
  const bool two = worker.post(RecordingTrackWorker::Kind::Video, [&] { ++count; });
  const bool overflow = worker.post(RecordingTrackWorker::Kind::Video, [] {});
  worker.close(); releaseSteady.set_value(); worker.join();
  EXPECT_EQ(drained, std::future_status::ready);
  EXPECT_EQ(entered, std::future_status::ready);
  EXPECT_TRUE(one); EXPECT_TRUE(two); EXPECT_FALSE(overflow);
  EXPECT_EQ(count, 6);
  EXPECT_EQ(worker.evidence().droppedVideo, 2u);
}
