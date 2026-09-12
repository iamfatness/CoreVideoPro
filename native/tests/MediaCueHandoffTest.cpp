// T1.11 / #449 — the pure decision behind the cue-to-Program decoder hand-over.
// Kept separate from OwnedMediaFrameSource so every refusal is testable without
// a decoder, a thread or a clock (the CaptureReaderStallPolicy / TakeRecordPolicy
// shape). The rule it encodes: a Preview cue poster that has NEVER ROLLED may be
// handed to Program, because it sits paused at frame 0 and resuming it IS the
// go-live contract's "roll from 0". Anything else cold-starts, as today.

#include "modules/MediaCueHandoff.h"

#include <gtest/gtest.h>

namespace {

using corevideo::modules::MediaSourceRequest;
using corevideo::modules::isCueHandoff;

MediaSourceRequest cue() {
  return MediaSourceRequest{"preview:media:clip-1", "clip-1", "C:\media\clip.mp4", "media:clip-1:live:1", false};
}

MediaSourceRequest live() {
  return MediaSourceRequest{"media:clip-1", "clip-1", "C:\media\clip.mp4", "media:clip-1:live:2", false};
}

}  // namespace

TEST(MediaCueHandoff, AdoptsTheCuePosterOnItsGoLiveSuccessor) {
  EXPECT_TRUE(isCueHandoff(cue(), live(), /*retiringEverPlayed=*/false));
}

TEST(MediaCueHandoff, RefusesACueThatAlreadyRolled) {
  // A decoder that has played is at an arbitrary position. Adopting it would
  // put the clip on air mid-roll and quietly break "roll from 0, audio on".
  EXPECT_FALSE(isCueHandoff(cue(), live(), /*retiringEverPlayed=*/true));
}

TEST(MediaCueHandoff, RefusesAnythingButTheNextGeneration) {
  auto skipped = live();
  skipped.mediaPlaybackKey = "media:clip-1:live:3";
  EXPECT_FALSE(isCueHandoff(cue(), skipped, false));
  auto same = live();
  same.mediaPlaybackKey = "media:clip-1:live:1";
  EXPECT_FALSE(isCueHandoff(cue(), same, false));
  auto backwards = live();
  backwards.mediaPlaybackKey = "media:clip-1:live:0";
  EXPECT_FALSE(isCueHandoff(cue(), backwards, false));
}

TEST(MediaCueHandoff, RefusesADifferentAssetOrADifferentFile) {
  auto otherAsset = live();
  otherAsset.mediaAssetId = "clip-2";
  EXPECT_FALSE(isCueHandoff(cue(), otherAsset, false));
  // Same asset id, different file on disk: the bin row was repointed. The warm
  // decoder holds the OLD file's pictures and must not be handed over.
  auto otherFile = live();
  otherFile.mediaAssetPath = "C:\media\other.mp4";
  EXPECT_FALSE(isCueHandoff(cue(), otherFile, false));
}

TEST(MediaCueHandoff, RefusesAPairingThatIsNotThePreviewCueCollapsing) {
  // Only `preview:<live source id>` -> `<live source id>` is a hand-over.
  auto notPrefixed = cue();
  notPrefixed.sourceId = "media:clip-1";
  EXPECT_FALSE(isCueHandoff(notPrefixed, live(), false));
  auto strayPrefix = live();
  strayPrefix.sourceId = "preview:media:clip-1";
  EXPECT_FALSE(isCueHandoff(cue(), strayPrefix, false));
}

TEST(MediaCueHandoff, RefusesLoops) {
  // A loop's key carries no generation (`media:<id>`) and a loop is never cued
  // paused — it plays on both buses under one id already. Nothing to adopt.
  auto loopCue = cue();
  loopCue.loop = true;
  loopCue.mediaPlaybackKey = "media:clip-1";
  auto loopLive = live();
  loopLive.loop = true;
  loopLive.mediaPlaybackKey = "media:clip-1";
  EXPECT_FALSE(isCueHandoff(loopCue, loopLive, false));
  auto mismatched = live();
  mismatched.loop = true;
  EXPECT_FALSE(isCueHandoff(cue(), mismatched, false));
}
