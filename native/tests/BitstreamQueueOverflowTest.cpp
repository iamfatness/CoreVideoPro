#include "modules/BitstreamQueueOverflow.h"

#include <gtest/gtest.h>

#include <string>

using corevideo::modules::describeQueueOverflowCause;
using corevideo::modules::describeQueueOverflowDiscard;
using corevideo::modules::describeQueueOverflowFailure;
using corevideo::modules::kQueueOverflowDiscardMarker;
using corevideo::modules::kQueueOverflowFailureMarker;

namespace {

// #597 Task 8b fix round 3, item 1. THESE TESTS EXIST BECAUSE THE GATE GREPS
// THESE STRINGS, and in fix round 2 a reword silently disarmed the assertion
// that depends on them - in the same commit that tightened it. Neither branch
// of the failure message had a test, so nothing broke when the substring the
// gate looks for stopped existing.
//
// THE OTHER HALF OF THIS CONTRACT LIVES IN scripts/validate-gpu-encode.mjs.
// If one of these assertions fails because you reworded a message, the fix is
// to update BOTH this test and that script's patterns - never just this test.

TEST(BitstreamQueueOverflow, TheFailureMarkerIsExactlyWhatTheGateGreps) {
  // The literal, spelled out here so a reword cannot pass silently. The gate
  // greps this substring to decide a congested run hit a keyframe-less
  // overflow - which fails the sender, restarts it via the supervisor and
  // rebuilds the encoder, i.e. #597 itself.
  EXPECT_EQ(std::string(kQueueOverflowFailureMarker),
            "queue overflow with nothing safe to drop");
  EXPECT_EQ(std::string(kQueueOverflowDiscardMarker), "overflow-discard");
}

// BRANCH 1: nothing was dropped - there was no keyframe queued and none
// arriving, so the cut genuinely had nothing safe to remove.
TEST(BitstreamQueueOverflow, TheNothingSafeToDropBranchCarriesTheMarkerTheGateGreps) {
  const auto line = describeQueueOverflowFailure(/*dropped=*/0, /*queuedBytes=*/590149,
                                                 /*queuedChunks=*/60, /*incomingBytes=*/22623,
                                                 /*maxChunks=*/60, /*maxBytes=*/2u << 20);
  EXPECT_NE(line.find(kQueueOverflowFailureMarker), std::string::npos)
      << "the gate greps this substring; without it the burst assertion is vacuous and every "
         "reported zero is UNMEASURED rather than measured-zero: " << line;
  EXPECT_NE(line.find("no keyframe queued and none arriving"), std::string::npos);
  EXPECT_NE(line.find("queuedChunks=60"), std::string::npos);
  EXPECT_NE(line.find("incomingBytes=22623"), std::string::npos);
  EXPECT_NE(line.find("sender unhealthy -> supervisor"), std::string::npos);
}

// BRANCH 2: the cut DID drop chunks and the BYTE budget is still over. A
// different situation with a different fix - reporting "no keyframe queued"
// here would send a live diagnosis hunting a keyframe that was never missing.
TEST(BitstreamQueueOverflow, TheByteBudgetBranchNeverClaimsThereWasNoKeyframe) {
  const auto line = describeQueueOverflowFailure(/*dropped=*/12, /*queuedBytes=*/2097000,
                                                 /*queuedChunks=*/48, /*incomingBytes=*/900000,
                                                 /*maxChunks=*/60, /*maxBytes=*/2u << 20);
  EXPECT_EQ(line.find(kQueueOverflowFailureMarker), std::string::npos)
      << "a byte-budget overflow must NOT claim nothing was safe to drop - 12 chunks were: "
      << line;
  EXPECT_NE(line.find("byte budget is still over"), std::string::npos) << line;
}

// Item 5: the bounds in the message are FORMATTED FROM THE CONSTANTS, so a
// changed cap cannot leave the sentence describing the old one.
TEST(BitstreamQueueOverflow, TheBoundsInTheMessageComeFromTheConstantsNotFromLiterals) {
  const auto standard = describeQueueOverflowCause(/*dropped=*/3, /*maxChunks=*/60,
                                                   /*maxBytes=*/2u << 20);
  EXPECT_NE(standard.find("60-chunk"), std::string::npos) << standard;
  EXPECT_NE(standard.find("2 MiB"), std::string::npos) << standard;

  // Raise both bounds: the sentence must follow them. Hard-coding either one
  // back into the string fails here.
  const auto raised = describeQueueOverflowCause(/*dropped=*/3, /*maxChunks=*/90,
                                                 /*maxBytes=*/8u << 20);
  EXPECT_NE(raised.find("90-chunk"), std::string::npos) << raised;
  EXPECT_NE(raised.find("8 MiB"), std::string::npos) << raised;
  EXPECT_EQ(raised.find("60-chunk"), std::string::npos) << raised;
  EXPECT_EQ(raised.find("2 MiB"), std::string::npos) << raised;
}

// The rescued case, which is the OTHER string the gate greps - and the one
// whose presence is now required for a burst run to pass at all.
TEST(BitstreamQueueOverflow, TheDiscardLineCarriesTheMarkerAndItsYield) {
  const auto line = describeQueueOverflowDiscard(/*dropped=*/60, /*queuedChunks=*/0,
                                                 /*queuedBytes=*/0);
  EXPECT_NE(line.find(kQueueOverflowDiscardMarker), std::string::npos)
      << "the gate REQUIRES this substring to accept a burst run: " << line;
  EXPECT_NE(line.find("dropped=60"), std::string::npos) << line;
  EXPECT_NE(line.find("[stream-backpressure]"), std::string::npos) << line;
}

// The two markers must stay distinguishable: the gate counts them separately,
// and one is a pass condition while the other is a failure condition. If a
// reword ever made the failure line contain the discard marker, a failing run
// would satisfy the "did it reach the branch" half.
TEST(BitstreamQueueOverflow, AFailureLineNeverContainsTheDiscardMarker) {
  const auto failure = describeQueueOverflowFailure(0, 590149, 60, 22623, 60, 2u << 20);
  EXPECT_EQ(failure.find(kQueueOverflowDiscardMarker), std::string::npos) << failure;
  const auto discard = describeQueueOverflowDiscard(60, 0, 0);
  EXPECT_EQ(discard.find(kQueueOverflowFailureMarker), std::string::npos) << discard;
}

}  // namespace
