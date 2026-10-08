#include "modules/RecordingTrackWorker.h"
#include <gtest/gtest.h>
#include <future>
#include <vector>
#include "modules/RecordingQueuePolicy.h"
#include "modules/RecordingAudioContinuity.h"
#include "modules/RecordingPtsClock.h"

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

TEST(RecordingTrackWorker, FileFifoAbsorbsStallAndPreservesCapturePts) {
  std::promise<void> entered, release;
  auto gate = release.get_future().share();
  std::vector<int64_t> written;
  bool finalized = false;
  RecordingTrackWorker writer([] {}, [&] { finalized = true; }, 10, 96, 10, true, 8000, 10000);
  writer.post(RecordingTrackWorker::Kind::Video, [&] { entered.set_value(); gate.wait(); });
  entered.get_future().wait();
  writer.finishStartup();
  for (int64_t pts = 100; pts < 110; ++pts)
    EXPECT_TRUE(writer.post(RecordingTrackWorker::Kind::Video, [&, pts] { written.push_back(pts); }, 100));
  EXPECT_TRUE(writer.post(RecordingTrackWorker::Kind::Audio, [] {}, 64, 8000));
  const auto queued = writer.evidence();
  writer.close(); release.set_value(); writer.join();
  EXPECT_EQ(queued.queueHighWater, 10u);
  EXPECT_EQ(queued.highWaterBytes, 1064u);
  EXPECT_EQ(queued.queuedAudioSamples, 8000u);
  EXPECT_EQ(writer.evidence().droppedVideo, 0u);
  EXPECT_EQ(writer.evidence().droppedAudio, 0u);
  EXPECT_TRUE(finalized);
  ASSERT_EQ(written.size(), 10u);
  for (int i = 0; i < 10; ++i) EXPECT_EQ(written[i], 100 + i);
}

TEST(RecordingTrackWorker, OverflowEvictsOnlyThisFilesOldestPictureAndAudioIsSampleBounded) {
  std::promise<void> entered, release, fastWritten;
  auto gate = release.get_future().share();
  std::vector<int> written;
  RecordingTrackWorker slow([] {}, [] {}, 2, 96, 2, true, 1600, 10000);
  RecordingTrackWorker fast([] {}, [] {}, 2, 96, 2, true, 1600, 10000);
  slow.post(RecordingTrackWorker::Kind::Video, [&] { entered.set_value(); gate.wait(); });
  entered.get_future().wait(); slow.finishStartup();
  slow.post(RecordingTrackWorker::Kind::Video, [&] { written.push_back(1); });
  slow.post(RecordingTrackWorker::Kind::Audio, [&] { written.push_back(9); }, 64, 960);
  slow.post(RecordingTrackWorker::Kind::Video, [&] { written.push_back(2); });
  const bool replaced = slow.post(RecordingTrackWorker::Kind::Video, [&] { written.push_back(3); });
  const bool audioRefused = !slow.post(RecordingTrackWorker::Kind::Audio, [] {}, 64, 960);
  fast.post(RecordingTrackWorker::Kind::Video, [&] { fastWritten.set_value(); });
  const auto progressed = fastWritten.get_future().wait_for(std::chrono::seconds(2));
  slow.close(); fast.close(); release.set_value(); slow.join(); fast.join();
  EXPECT_TRUE(replaced); EXPECT_TRUE(audioRefused);
  EXPECT_EQ(progressed, std::future_status::ready);
  EXPECT_EQ(slow.evidence().droppedVideo, 1u);
  EXPECT_EQ(slow.evidence().startupDroppedVideo, 0u);
  EXPECT_EQ(slow.evidence().droppedAudio, 1u);
  EXPECT_EQ(fast.evidence().droppedVideo, 0u);
  ASSERT_EQ(written.size(), 3u);
  EXPECT_EQ(written[0], 9); EXPECT_EQ(written[1], 2); EXPECT_EQ(written[2], 3);
}

TEST(RecordingTrackWorker, DepthProjectionRefusesMemoryExpansionWithoutChangingLiveBuffer) {
  using corevideo::modules::RecordingQueuePolicy;
  std::vector<std::string> sources;
  for (int i = 0; i < 8; ++i) sources.push_back("zoom:" + std::to_string(i));
  EXPECT_TRUE(RecordingQueuePolicy::accepts(10, 1920, 1080, true, sources));
  EXPECT_FALSE(RecordingQueuePolicy::accepts(30, 1920, 1080, true, sources));
  EXPECT_FALSE(RecordingQueuePolicy::accepts(3, 1920, 1080, true, {}));
  EXPECT_FALSE(RecordingQueuePolicy::accepts(10, 8192, 8192, true, sources));
}

TEST(RecordingTrackWorker, ExplicitAudioLossFillsSamplesWithoutFollowingCallbackJitter) {
  corevideo::modules::RecordingAudioContinuity continuity;
  corevideo::modules::RecordingPtsClock clock;
  clock.reset(10'000'000);
  EXPECT_EQ(continuity.gapBefore(0, 960), 0u);
  EXPECT_EQ(clock.audioPts(10'000'000, 960, 48000), 0);
  EXPECT_EQ(continuity.gapBefore(1920, 960), 960u);
  EXPECT_EQ(clock.audioPts(10'270'000, 960, 48000), 200'000); // lost packet's silence
  EXPECT_EQ(clock.audioPts(10'270'000, 960, 48000), 400'000); // real packet stays at 40ms
  EXPECT_EQ(continuity.gapBefore(2880, 960), 0u);
  EXPECT_EQ(clock.audioPts(10'830'000, 960, 48000), 600'000); // callback jitter does not move it
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

TEST(RecordingTrackWorker, StartupAudioAllowanceDrainsWithoutShrinkingAcceptedPackets) {
  std::promise<void> entered, release;
  auto gate = release.get_future().share();
  RecordingTrackWorker worker([&] { entered.set_value(); gate.wait(); }, [] {},
      10, 96, 96, true, 1920, 1000000, 9600);
  entered.get_future().wait();
  for (int i=0;i<10;++i)
    EXPECT_TRUE(worker.post(RecordingTrackWorker::Kind::Audio, [] {}, 7680, 960));
  EXPECT_FALSE(worker.post(RecordingTrackWorker::Kind::Audio, [] {}, 7680, 960));
  worker.finishStartup();
  EXPECT_FALSE(worker.post(RecordingTrackWorker::Kind::Audio, [] {}, 7680, 960));
  worker.close(); release.set_value(); worker.join();
  const auto e = worker.evidence();
  EXPECT_EQ(e.acceptedAudio, 10u);
  EXPECT_EQ(e.completedAudio, 10u);
  EXPECT_EQ(e.startupDroppedAudio, 1u);
  EXPECT_EQ(e.droppedAudio, 2u);
}
