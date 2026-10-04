#include "modules/VirtualCameraDeliveryEvidence.h"
#include "modules/VirtualCameraReadRetry.h"
#include "modules/VirtualCameraCorrelation.h"
#include <gtest/gtest.h>

using namespace corevideo::modules;

TEST(VirtualCameraDeliveryEvidence, FailedEventsNeverCountAsDeliveredOrAdvanceTheClock) {
  VirtualCameraDeliveryEvidence evidence;
  evidence.recordEmission(true, VirtualCameraSampleContent::Fresh, 100);
  evidence.recordEmission(false, VirtualCameraSampleContent::Fresh, 200);
  evidence.recordEmission(true, VirtualCameraSampleContent::Held, 450);
  evidence.recordEmission(true, VirtualCameraSampleContent::Slate, 500);
  EXPECT_EQ(evidence.emitted, 3u);
  EXPECT_EQ(evidence.fresh, 1u);
  EXPECT_EQ(evidence.held, 1u);
  EXPECT_EQ(evidence.slate, 1u);
  EXPECT_EQ(evidence.failed, 1u);
  EXPECT_EQ(evidence.maximumIntervalHns, 350u);
}

TEST(VirtualCameraDeliveryEvidence, StopStartDoesNotTurnIdleTimeIntoADeliveryStall) {
  VirtualCameraDeliveryEvidence evidence;
  evidence.recordEmission(true, VirtualCameraSampleContent::Fresh, 100);
  evidence.recordEmission(true, VirtualCameraSampleContent::Fresh, 200);
  evidence.restartCadence();
  evidence.recordEmission(true, VirtualCameraSampleContent::Fresh, 1000000);
  EXPECT_EQ(evidence.maximumIntervalHns, 100u);
  EXPECT_EQ(evidence.emitted, 3u);
}

TEST(VirtualCameraDeliveryEvidence, ReadMissesCannotInventANewPublicationIdentity) {
  VirtualCameraReadEvidence evidence;
  EXPECT_FALSE(evidence.identityObserved);
  evidence.recordFresh(31, 64);
  evidence.record(VirtualCameraReadResult::Contended);
  evidence.record(VirtualCameraReadResult::Unchanged);
  EXPECT_EQ(evidence.lastPublication, 31u);
  EXPECT_EQ(evidence.lastSequence, 64u);
  EXPECT_EQ(evidence.count(VirtualCameraReadResult::Fresh), 1u);
  EXPECT_EQ(evidence.count(VirtualCameraReadResult::Contended), 1u);
  EXPECT_EQ(evidence.count(VirtualCameraReadResult::Unchanged), 1u);
}

TEST(VirtualCameraDeliveryEvidence, CorrelationRequiresStableEpochPixelAndProgramIdentities) {
  VirtualCameraCorrelationRecord before;
  before.magic = kVirtualCameraCorrelationMagic;
  before.sequence = 10; before.pixelSequence = 24; before.publication = 11;
  before.epochHigh = 42; before.epochLow = 73; before.programSequence = 100;
  auto after = before;
  EXPECT_TRUE(correlatesCameraPixels(before, after, 24, 11));
  EXPECT_FALSE(correlatesCameraPixels(before, after, 24, 11, 99, 100));
  EXPECT_FALSE(correlatesCameraPixels(before, after, 22, 11));
  EXPECT_FALSE(correlatesCameraPixels(before, after, 24, 10));
  after.epochLow = 74;
  EXPECT_FALSE(correlatesCameraPixels(before, after, 24, 11));
  after = before; after.sequence = 12;
  EXPECT_FALSE(correlatesCameraPixels(before, after, 24, 11));
  after = before; after.programSequence = 101;
  EXPECT_FALSE(correlatesCameraPixels(before, after, 24, 11));
  before.programSequence = 0; after = before;
  EXPECT_FALSE(correlatesCameraPixels(before, after, 24, 11));
  before.programSequence = 100; before.sequence = 11; after = before;
  EXPECT_FALSE(correlatesCameraPixels(before, after, 24, 11));
}

TEST(VirtualCameraDeliveryEvidence, UncorrelatedReadsAndEpochChangesCannotInventContinuity) {
  VirtualCameraReadEvidence evidence;
  evidence.recordCorrelation(true, 1, 2, 100);
  evidence.recordCorrelation(false);
  EXPECT_FALSE(evidence.programIdentityVerified);
  evidence.recordCorrelation(true, 1, 2, 103);
  EXPECT_EQ(evidence.unobservedProgramFrames, 2u);
  evidence.recordCorrelation(true, 3, 4, 1);
  EXPECT_EQ(evidence.programEpochChanges, 1u);
  EXPECT_EQ(evidence.programRegressions, 0u);
  evidence.recordCorrelation(true, 3, 4, 5);
  evidence.recordCorrelation(true, 3, 4, 2);
  EXPECT_EQ(evidence.programRegressions, 1u);
  EXPECT_EQ(evidence.uncorrelatedReads, 1u);
}

TEST(VirtualCameraReadRetry, RecoversTransientRaceWithoutAdvancingReopenCadence) {
  int reads = 0, reopenReads = 0;
  int64_t now = 0;
  unsigned retries = 0;
  const bool fresh = readCameraWithBoundedRetry([&](bool reopen) {
    ++reads; if (reopen) ++reopenReads;
    return reads == 1 ? VirtualCameraReadResult::Contended : VirtualCameraReadResult::Fresh;
  }, [&] { return now; }, [&] { now += 250; return true; }, true, retries);
  EXPECT_TRUE(fresh); EXPECT_EQ(reads, 2); EXPECT_EQ(reopenReads, 1); EXPECT_EQ(retries, 1u);
}

TEST(VirtualCameraReadRetry, BoundsElapsedTimeAttemptsAndUnavailablePublishers) {
  for (auto result : {VirtualCameraReadResult::Unchanged, VirtualCameraReadResult::Contended,
                      VirtualCameraReadResult::Unavailable, VirtualCameraReadResult::InvalidHeader}) {
    int reads = 0; int64_t now = 0; unsigned retries = 0;
    EXPECT_FALSE(readCameraWithBoundedRetry([&](bool) { ++reads; return result; },
        [&] { return now; }, [&] { now += 250; return true; }, true, retries));
    EXPECT_EQ(reads, result == VirtualCameraReadResult::Unchanged || result == VirtualCameraReadResult::Contended ? 4 : 1);
  }
  int reads = 0; int64_t now = 0; unsigned retries = 0;
  EXPECT_FALSE(readCameraWithBoundedRetry([&](bool) { ++reads; return VirtualCameraReadResult::Contended; },
      [&] { return now; }, [&] { now += 2000; return true; }, true, retries));
  EXPECT_EQ(reads, 1); EXPECT_EQ(retries, 0u);
}

TEST(VirtualCameraReadRetry, DisabledModePreservesSingleRead) {
  unsigned retries = 0;
  EXPECT_FALSE(readCameraWithBoundedRetry([](bool) { return VirtualCameraReadResult::Unchanged; },
      [] { return int64_t{0}; }, [] { throw std::runtime_error("unexpected wait"); return true; }, false, retries));
  EXPECT_EQ(retries, 0u);
}

TEST(VirtualCameraReadRetry, PublicationCanArriveLaterWithinOneFrameWithoutRepeatingOldContent) {
  int64_t now = 0; unsigned retries = 0;
  EXPECT_TRUE(readCameraWithBoundedRetry([&](bool) {
    return now >= 8000 ? VirtualCameraReadResult::Fresh : VirtualCameraReadResult::Unchanged;
  }, [&] { return now; }, [&] { now += 250; return true; }, true, retries, 16666, 64));
  EXPECT_EQ(now, 8000); EXPECT_EQ(retries, 32u);
}

TEST(VirtualCameraReadRetry, SweepsConsumerPhaseAcrossThePublishersWriteWindow) {
  // A 60 Hz producer spends 400 us publishing under an odd seqlock. Sweep
  // reader phases rather than assuming one fortunate start phase is evidence.
  int missedWithoutWait = 0;
  for (int phase = 0; phase < 16666; phase += 125) {
    for (bool enabled : {false, true}) {
      int64_t now = phase;
      int64_t lastPublication = -1;
      for (int frame = 0; frame < 120; ++frame) {
        now = int64_t{frame} * 16666 + phase;
        unsigned retries = 0;
        const bool fresh = readCameraWithBoundedRetry([&](bool) {
          if (now % 16666 < 400) return VirtualCameraReadResult::Contended;
          const auto publication = now / 16666;
          if (publication == lastPublication) return VirtualCameraReadResult::Unchanged;
          lastPublication = publication;
          return VirtualCameraReadResult::Fresh;
        }, [&] { return now; }, [&] { now += 250; return true; }, enabled, retries, 16666, 64);
        if (enabled) { EXPECT_TRUE(fresh); EXPECT_EQ(lastPublication, frame); }
        else if (!fresh) ++missedWithoutWait;
      }
    }
  }
  EXPECT_GT(missedWithoutWait, 0);
}
