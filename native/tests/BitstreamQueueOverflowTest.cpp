#include "modules/BitstreamQueueOverflow.h"

#include <gtest/gtest.h>

#include <string>

using corevideo::modules::describeQueueOverflowCause;
using corevideo::modules::describeQueueOverflowDiscard;
using corevideo::modules::describeQueueOverflowOversizedChunk;
using corevideo::modules::describeQueueOverflowFailure;
using corevideo::modules::kQueueOverflowDiscardMarker;
using corevideo::modules::kQueueOverflowFailureMarker;
using corevideo::modules::kQueueOverflowSenderFailedMarker;

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

// Item 5: the byte bound in the message is FORMATTED FROM THE CONSTANT, so a
// changed cap cannot leave the sentence describing the old one.
TEST(BitstreamQueueOverflow, TheBoundsInTheMessageComeFromTheConstantsNotFromLiterals) {
  const auto standard = describeQueueOverflowCause(/*dropped=*/3, /*maxBytes=*/2u << 20);
  EXPECT_NE(standard.find("2 MiB"), std::string::npos) << standard;

  // Raise the bound: the sentence must follow it. Hard-coding it back fails here.
  const auto raised = describeQueueOverflowCause(/*dropped=*/3, /*maxBytes=*/8u << 20);
  EXPECT_NE(raised.find("8 MiB"), std::string::npos) << raised;
  EXPECT_EQ(raised.find("2 MiB"), std::string::npos) << raised;
}

// FINAL-REVIEW FINDING 5, pinned. The byte-budget branch lost four separate
// properties in the refactor into BitstreamQueueOverflow.h, and the commit's
// own review missed every one because the only assertion on this branch was
// "does not contain the nothing-safe-to-drop marker" plus "contains 'byte
// budget is still over'" - both of which the broken line satisfied. All four
// are asserted here, on the FULL composed line, which is what actually reaches
// a log.
TEST(BitstreamQueueOverflow, TheByteBudgetLineIsGrammaticalGreppableAndNamesTheYield) {
  const auto line = describeQueueOverflowFailure(/*dropped=*/12, /*queuedBytes=*/2097000,
                                                 /*queuedChunks=*/48, /*incomingBytes=*/900000,
                                                 /*maxChunks=*/60, /*maxBytes=*/2u << 20);

  // (1) GREPPABLE. The regression read "[gpu-encode] bitstream the cut freed
  // 60-chunk room but ...", which no search for "queue overflow" finds - and
  // "queue overflow" is what an operator and this repo's own gate look for.
  EXPECT_NE(line.find(kQueueOverflowSenderFailedMarker), std::string::npos)
      << "every sender-failing overflow branch must carry the marker the gate counts: " << line;
  EXPECT_NE(line.find("queue overflow"), std::string::npos) << line;

  // (2) GRAMMATICAL. "bitstream the cut freed ..." is not a sentence. The
  // composed prefix is "[gpu-encode] bitstream ", so the cause must read as a
  // noun phrase after it.
  EXPECT_NE(line.find("[gpu-encode] bitstream queue overflow: the cut dropped"), std::string::npos)
      << line;

  // (3) NAMES THE YIELD. `dropped` is the one number a diagnostician wants and
  // the regression omitted it entirely.
  EXPECT_NE(line.find("dropped 12 chunk(s)"), std::string::npos)
      << "the line must say how many chunks the cut actually freed: " << line;

  // (4) DOES NOT PRINT THE CAP AS THOUGH IT WERE THE YIELD. The regression read
  // "freed 60-chunk room" where 60 is kMaxQueuedChunks, not anything measured.
  // The cap is still published - as its own clearly-labelled field.
  EXPECT_EQ(line.find("60-chunk"), std::string::npos)
      << "the chunk CAP must never be formatted as the cut's yield: " << line;
  EXPECT_NE(line.find("chunkCap=60"), std::string::npos) << line;

  // And the properties the original test held, unchanged.
  EXPECT_EQ(line.find(kQueueOverflowFailureMarker), std::string::npos)
      << "12 chunks were safe to drop - this branch must not claim none were: " << line;
  EXPECT_NE(line.find("byte budget is still over"), std::string::npos) << line;
}

// FINAL-REVIEW FINDING 6, the third sender-failing branch. A chunk larger than
// the whole byte budget fails the sender exactly like the other two, and used
// to be logged with a hand-written string carrying no grepped marker at all -
// so the gate's attribution column could not see it.
TEST(BitstreamQueueOverflow, TheOversizedChunkBranchCarriesTheSenderFailedMarkerToo) {
  const auto line = describeQueueOverflowOversizedChunk(/*incomingBytes=*/3000000,
                                                        /*maxBytes=*/2u << 20);
  EXPECT_NE(line.find(kQueueOverflowSenderFailedMarker), std::string::npos) << line;
  EXPECT_NE(line.find("incomingBytes=3000000"), std::string::npos) << line;
  EXPECT_NE(line.find("2 MiB"), std::string::npos) << line;
  EXPECT_NE(line.find("sender unhealthy -> supervisor"), std::string::npos) << line;
  // It is NOT the keyframe-less case: nothing was even considered for dropping.
  EXPECT_EQ(line.find(kQueueOverflowFailureMarker), std::string::npos) << line;
}

// All three failing branches share one marker, and the RESCUED path shares
// none of it - otherwise the gate's pass condition and its fail condition
// would be the same grep.
TEST(BitstreamQueueOverflow, AllThreeFailingBranchesShareTheMarkerAndTheRescuedPathDoesNot) {
  const std::string noKeyframe =
      describeQueueOverflowFailure(0, 590149, 60, 22623, 60, 2u << 20);
  const std::string byteBudget =
      describeQueueOverflowFailure(12, 2097000, 48, 900000, 60, 2u << 20);
  const std::string oversized = describeQueueOverflowOversizedChunk(3000000, 2u << 20);
  for (const auto* line : {&noKeyframe, &byteBudget, &oversized}) {
    EXPECT_NE(line->find(kQueueOverflowSenderFailedMarker), std::string::npos) << *line;
  }
  const auto rescued = describeQueueOverflowDiscard(60, 0, 0);
  EXPECT_EQ(rescued.find(kQueueOverflowSenderFailedMarker), std::string::npos)
      << "a rescued overflow must never be counted as a sender failure: " << rescued;
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
