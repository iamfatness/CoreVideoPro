#include "modules/VirtualCameraDeliveryEvidence.h"
#include "modules/VirtualCameraReadRetry.h"
#include "modules/VirtualCameraCadence.h"
#include "modules/VirtualCameraRequestSchedule.h"
#include "modules/VirtualCameraCorrelation.h"
#include <gtest/gtest.h>

using namespace corevideo::modules;

TEST(VirtualCameraRequestSchedule, ColdFirstEmissionDoesNotLeaveTheNextReadWindowBehindThePublisher) {
  VirtualCameraRequestSchedule legacy, corrected;
  legacy.reset(60); corrected.reset(60);
  const auto first = corrected.begin(0);
  legacy.begin(0);
  EXPECT_EQ(first.readDeadlineUs, 14666);
  // Allocation/open makes the first fresh emission late. The publisher's next
  // picture arrives 16.6666ms after its first picture at 19ms.
  constexpr int64_t firstEmission = 200000, nextPublication = 356666;
  EXPECT_TRUE(corrected.firstFreshEmission(firstEmission));
  const auto oldWindow = legacy.begin(firstEmission + 10);
  const auto next = corrected.begin(firstEmission + 10);
  EXPECT_TRUE(oldWindow.readDeadlineUs * 10 < nextPublication); // old phase must hold
  EXPECT_TRUE(next.target100ns > nextPublication); // next picture is already due
  EXPECT_EQ(next.wait100ns, 166656);
  EXPECT_EQ(first.readDeadlineUs, 14666); // no extension of an issued window
}

TEST(VirtualCameraRequestSchedule, FreshPicturesCannotContinuouslyRebaseOrExtendTheClock) {
  VirtualCameraRequestSchedule schedule;
  schedule.reset(60); schedule.begin(1000000);
  EXPECT_TRUE(schedule.firstFreshEmission(1200000));
  const auto second = schedule.begin(1366666);
  EXPECT_EQ(second.target100ns, 1366666);
  EXPECT_FALSE(schedule.firstFreshEmission(1390000));
  const auto third = schedule.begin(1400000);
  EXPECT_EQ(third.target100ns, 1533333);
  EXPECT_EQ(second.readDeadlineUs, 151333);
  schedule.reset(60);
  EXPECT_TRUE(schedule.firstFreshEmission(3000000));
  EXPECT_EQ(schedule.begin(3000000).target100ns, 3166666);
}

TEST(VirtualCameraCadence, WholeHourHasNoFractionalFrameClockDrift) {
  for (uint32_t fps : {24u, 30u, 60u}) {
    VirtualCameraCadence cadence(fps); int64_t elapsed = 0;
    for (uint32_t frame = 0; frame < fps * 3600u; ++frame) {
      const auto interval = cadence.nextInterval100ns();
      EXPECT_TRUE(interval == 10000000u / fps || interval == 10000000u / fps + 1);
      elapsed += interval;
    }
    EXPECT_EQ(elapsed, 3600LL * 10000000);
    if (fps == 60) EXPECT_EQ(elapsed - int64_t(10000000u / fps) * fps * 3600, 144000);
  }
}

TEST(VirtualCameraCadence, ReanchorResetsFractionAndZeroRateKeepsThirtyFpsDefault) {
  VirtualCameraCadence cadence(60);
  EXPECT_EQ(cadence.nextInterval100ns(), 166666);
  EXPECT_EQ(cadence.nextInterval100ns(), 166667);
  cadence.reset(60); EXPECT_EQ(cadence.nextInterval100ns(), 166666);
  cadence.reset(0); int64_t total = 0;
  for (int frame = 0; frame < 30; ++frame) total += cadence.nextInterval100ns();
  EXPECT_EQ(total, 10000000);
}

TEST(VirtualCameraReadRetry, ColdFirstPayloadCopyDoesNotConsumeTheBoundedRecoveryWindow) {
  const auto run = [](bool oldEntryClamp) {
    int64_t now = 0; unsigned retries = 0; unsigned copies = 0;
    const int64_t sampleDeadline = 14000;
    const bool got = readCameraUntilDeadline([&](bool first) {
      ++copies;
      if (first) { now += 3000; return VirtualCameraReadResult::Contended; }
      now += 500; return VirtualCameraReadResult::Fresh;
    }, [&] { return now; }, [&](int64_t remaining) {
      EXPECT_TRUE(remaining > 0 && remaining <= sampleDeadline); now += 250; return true;
    }, true, retries, oldEntryClamp ? 2000 : sampleDeadline);
    EXPECT_TRUE(now < sampleDeadline);
    EXPECT_EQ(copies, oldEntryClamp ? 1u : 2u);
    EXPECT_EQ(retries, oldEntryClamp ? 0u : 1u);
    return got;
  };
  EXPECT_FALSE(run(true)); // The former entry-relative clamp loses a recoverable first sample.
  EXPECT_TRUE(run(false));
}

TEST(VirtualCameraReadRetry, EarlyUnchangedConsumerWaitsForPublicationWithinOriginalSampleWindow) {
  const auto run = [](int64_t deadline) {
    int64_t now = 0; unsigned retries = 0;
    return readCameraUntilDeadline([&](bool) {
      return now >= 3500 ? VirtualCameraReadResult::Fresh : VirtualCameraReadResult::Unchanged;
    }, [&] { return now; }, [&](int64_t remaining) {
      now += (std::min)(int64_t(retries ? 2000 : 250), (std::max)(int64_t(1), remaining / 2));
      return true;
    }, true, retries, deadline);
  };
  EXPECT_FALSE(run(2000));
  EXPECT_TRUE(run(14000));
}

TEST(VirtualCameraReadRetry, OriginalDeadlineIsNotRenewedByLateEntryOrOversleptWait) {
  for (int64_t start : {int64_t{1990}, int64_t{2100}}) {
    int64_t now = start; unsigned retries = 0; int reads = 0, waits = 0;
    EXPECT_FALSE(readCameraUntilDeadline([&](bool) {
      ++reads; return VirtualCameraReadResult::Contended;
    }, [&] { return now; }, [&](int64_t remaining) {
      EXPECT_EQ(remaining, 10); ++waits; now += 100; return true;
    }, true, retries, 2000));
    EXPECT_EQ(reads, start < 2000 ? 2 : 1);
    EXPECT_EQ(retries, start < 2000 ? 1u : 0u);
    EXPECT_EQ(waits, start < 2000 ? 1 : 0);
  }
}

TEST(VirtualCameraReadRetry, OriginalDeadlineAllowsOneFinalReadyReadAfterLateWake) {
  int64_t now = 1990; unsigned retries = 0; int reads = 0, waits = 0;
  EXPECT_TRUE(readCameraUntilDeadline([&](bool reopen) {
    ++reads;
    EXPECT_EQ(reopen, reads == 1);
    return reads == 1 ? VirtualCameraReadResult::Contended : VirtualCameraReadResult::Fresh;
  }, [&] { return now; }, [&](int64_t remaining) {
    EXPECT_EQ(remaining, 10); ++waits; now += 100; return true;
  }, true, retries, 2000));
  EXPECT_EQ(reads, 2); EXPECT_EQ(waits, 1); EXPECT_EQ(retries, 1u);
}

TEST(VirtualCameraReadRetry, DeadlineWaitRecoversWriteAndBoundsAttemptsWithoutReopen) {
  int64_t now = 0; unsigned retries = 0; int reopen = 0;
  EXPECT_TRUE(readCameraUntilDeadline([&](bool allowReopen) {
    if (allowReopen) ++reopen;
    return now < 750 ? VirtualCameraReadResult::Contended : VirtualCameraReadResult::Fresh;
  }, [&] { return now; }, [&](int64_t) { now += 250; return true; }, true, retries, 2000));
  EXPECT_EQ(reopen, 1); EXPECT_EQ(retries, 3u);
  now = 0;
  EXPECT_FALSE(readCameraUntilDeadline([](bool) { return VirtualCameraReadResult::Contended; },
      [&] { return now; }, [&](int64_t) { ++now; return true; }, true, retries, 2000));
  EXPECT_EQ(retries, 8u);
}

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

TEST(VirtualCameraReadRetry, FrameServerDefaultRecoversRacesWithoutPublisherEnvironment) {
  // The separately hosted OS camera has no producer-process environment.
  // Exercise the production default through the real bounded-retry decision.
  unsigned retries = 0; int reads = 0; int64_t now = 0;
  EXPECT_TRUE(readCameraWithBoundedRetry([&](bool) {
    return ++reads == 1 ? VirtualCameraReadResult::Contended : VirtualCameraReadResult::Fresh;
  }, [&] { return now; }, [&] { now += 250; return true; },
      cameraReadRetryEnabled(nullptr), retries));
  EXPECT_EQ(retries, 1u);
  EXPECT_FALSE(cameraReadRetryEnabled(L"0"));
  EXPECT_TRUE(cameraReadRetryEnabled(L""));
  EXPECT_TRUE(cameraReadRetryEnabled(L"1"));
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
  EXPECT_EQ(reads, 2); EXPECT_EQ(retries, 1u);
}

TEST(VirtualCameraReadRetry, LateWakeReadsThePublicationBeforeHoldingOldPixels) {
  int64_t now = 0; unsigned retries = 0; int reads = 0;
  EXPECT_TRUE(readCameraWithBoundedRetry([&](bool) {
    ++reads;
    return now >= 16666 ? VirtualCameraReadResult::Fresh : VirtualCameraReadResult::Unchanged;
  }, [&] { return now; }, [&] { now += 16800; return true; }, true, retries, 16666, 64));
  EXPECT_EQ(reads, 2); EXPECT_EQ(retries, 1u);
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
