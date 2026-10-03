#include "modules/VirtualCameraDeliveryEvidence.h"
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
