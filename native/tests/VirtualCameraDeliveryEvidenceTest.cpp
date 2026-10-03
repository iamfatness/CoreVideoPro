#include "modules/VirtualCameraDeliveryEvidence.h"
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
