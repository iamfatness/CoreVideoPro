#include "core/MediaTransportPolicy.h"
#include <gtest/gtest.h>
using namespace corevideo::core;
namespace {
MediaTransportDesired clip(bool onProgram, bool onPreview, const char* path = "C:/m/clip.mp4") {
  MediaTransportDesired d; d.sourceId = "media:clip"; d.assetId = "clip"; d.path = path;
  d.kind = "video"; d.loop = false; d.onProgram = onProgram; d.onPreview = onPreview; return d;
}
MediaTransportDesired loop(bool onProgram, bool onPreview) {
  auto d = clip(onProgram, onPreview, "C:/m/bg.mp4"); d.sourceId = "background:bg"; d.assetId = "bg";
  d.kind = "background"; d.loop = true; return d;
}
}
TEST(MediaTransportPolicy, ANewSourceOnProgramOpensLive) {
  const auto d = decideMediaTransport(std::nullopt, clip(true, false), MediaTransportState::Cued);
  EXPECT_EQ(d.action, MediaTransportAction::OpenLive); EXPECT_EQ(d.next, MediaTransportState::Live);
}
TEST(MediaTransportPolicy, ANewSourceOnPreviewOnlyOpensCued) {
  const auto d = decideMediaTransport(std::nullopt, clip(false, true), MediaTransportState::Cued);
  EXPECT_EQ(d.action, MediaTransportAction::OpenCued); EXPECT_EQ(d.next, MediaTransportState::Cued);
}
TEST(MediaTransportPolicy, ACuedClipEnteringProgramResumesTheSameDecoder) {
  const auto d = decideMediaTransport(clip(false, true), clip(true, true), MediaTransportState::Cued);
  EXPECT_EQ(d.action, MediaTransportAction::Resume); EXPECT_EQ(d.next, MediaTransportState::Live);
}
TEST(MediaTransportPolicy, ALiveClipLeavingProgramButStillCuedRestartsAtZero) {
  for (auto s : {MediaTransportState::Live, MediaTransportState::Paused, MediaTransportState::Ended}) {
    const auto d = decideMediaTransport(clip(true, false), clip(false, true), s);
    EXPECT_EQ(d.action, MediaTransportAction::RestartCued); EXPECT_EQ(d.next, MediaTransportState::Cued);
  }
}
TEST(MediaTransportPolicy, ALiveClipStayingOnProgramAcrossATakeIsLeftAlone) {
  const auto d = decideMediaTransport(clip(true, false), clip(true, true), MediaTransportState::Live);
  EXPECT_EQ(d.action, MediaTransportAction::None); EXPECT_EQ(d.next, MediaTransportState::Live);
  const auto p = decideMediaTransport(clip(true, true), clip(true, false), MediaTransportState::Paused);
  EXPECT_EQ(p.action, MediaTransportAction::None); EXPECT_EQ(p.next, MediaTransportState::Paused);
}
TEST(MediaTransportPolicy, AnIdenticalDesiredRowIsANoOp) {
  const auto d = decideMediaTransport(clip(true, true), clip(true, true), MediaTransportState::Live);
  EXPECT_EQ(d.action, MediaTransportAction::None);
  const auto c = decideMediaTransport(clip(false, true), clip(false, true), MediaTransportState::Cued);
  EXPECT_EQ(c.action, MediaTransportAction::None); EXPECT_EQ(c.next, MediaTransportState::Cued);
}
TEST(MediaTransportPolicy, AbsentFromBothBusesReleases) {
  const auto d = decideMediaTransport(clip(true, true), std::nullopt, MediaTransportState::Live);
  EXPECT_EQ(d.action, MediaTransportAction::Release);
}
TEST(MediaTransportPolicy, APathChangeReopens) {
  const auto d = decideMediaTransport(clip(true, false), clip(true, false, "C:/m/other.mp4"), MediaTransportState::Live);
  EXPECT_EQ(d.action, MediaTransportAction::Reopen); EXPECT_EQ(d.next, MediaTransportState::Live);
  const auto c = decideMediaTransport(clip(false, true), clip(false, true, "C:/m/other.mp4"), MediaTransportState::Cued);
  EXPECT_EQ(c.action, MediaTransportAction::Reopen); EXPECT_EQ(c.next, MediaTransportState::Cued);
}
TEST(MediaTransportPolicy, ALoopIsLiveOnAnyBusAndNeverRestartsOnATake) {
  EXPECT_EQ(decideMediaTransport(std::nullopt, loop(false, true), MediaTransportState::Cued).action, MediaTransportAction::OpenLive);
  EXPECT_EQ(decideMediaTransport(loop(false, true), loop(true, true), MediaTransportState::Live).action, MediaTransportAction::None);
  EXPECT_EQ(decideMediaTransport(loop(true, true), loop(false, true), MediaTransportState::Live).action, MediaTransportAction::None);
}
TEST(MediaTransportPolicy, OperatorPauseAndPlayOnlyOnALiveProgramClip) {
  std::string reason;
  auto d = decideMediaOperator(clip(true, false), MediaTransportState::Live, MediaOperatorAction::Pause, reason);
  ASSERT_TRUE(d); EXPECT_EQ(d->action, MediaTransportAction::Pause); EXPECT_EQ(d->next, MediaTransportState::Paused);
  d = decideMediaOperator(clip(true, false), MediaTransportState::Paused, MediaOperatorAction::Play, reason);
  ASSERT_TRUE(d); EXPECT_EQ(d->action, MediaTransportAction::Play); EXPECT_EQ(d->next, MediaTransportState::Live);
  EXPECT_FALSE(decideMediaOperator(loop(true, false), MediaTransportState::Live, MediaOperatorAction::Pause, reason));
  EXPECT_NE(reason.find("loop"), std::string::npos);
  EXPECT_FALSE(decideMediaOperator(clip(false, true), MediaTransportState::Cued, MediaOperatorAction::Pause, reason));
  EXPECT_NE(reason.find("Program"), std::string::npos);
  // Pausing an already paused clip / playing a live one is a no-op, not a refusal.
  d = decideMediaOperator(clip(true, false), MediaTransportState::Paused, MediaOperatorAction::Pause, reason);
  ASSERT_TRUE(d); EXPECT_EQ(d->action, MediaTransportAction::None);
  // Play on an ENDED clip restarts it from 0 (the only way an operator re-rolls a finished clip on air).
  d = decideMediaOperator(clip(true, false), MediaTransportState::Ended, MediaOperatorAction::Play, reason);
  ASSERT_TRUE(d); EXPECT_EQ(d->action, MediaTransportAction::OpenLive); EXPECT_EQ(d->next, MediaTransportState::Live);
}
TEST(MediaTransportPolicy, StateNamesAreTheWireVocabulary) {
  EXPECT_STREQ(mediaTransportStateName(MediaTransportState::Cued), "cued");
  EXPECT_STREQ(mediaTransportStateName(MediaTransportState::Live), "live");
  EXPECT_STREQ(mediaTransportStateName(MediaTransportState::Paused), "paused");
  EXPECT_STREQ(mediaTransportStateName(MediaTransportState::Ended), "ended");
}
