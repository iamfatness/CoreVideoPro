#pragma once

#include <cstddef>
#include <string>
#include <string_view>

namespace corevideo::modules {

// #597 Task 8b fix round 3. THE OUTGOING BITSTREAM QUEUE'S OVERFLOW MESSAGES,
// COMPOSED IN ONE PLACE SO THEY CAN BE PINNED BY TESTS.
//
// WHY THIS EXISTS, and it is the most useful thing in this file: these strings
// are a LOAD-BEARING INTERFACE, not diagnostics. `scripts/validate-gpu-encode.mjs`
// greps the core's stderr for them to decide whether a congested run entered
// the queue's overflow branch at all, and whether it survived it - the assertion
// that stops the gate reporting success while measuring nothing.
//
// In fix round 2 that coupling broke SILENTLY and in a single commit: one item
// tightened the gate to require zero "queue overflow with nothing safe to drop"
// lines, and another item reworded the very message the gate greps for. The
// substring stopped existing, `overflowFailLines` became permanently empty, and
// the assertion was vacuous by construction - every reported zero was UNMEASURED
// rather than measured-zero. That is the fourth measuring instrument in this
// sub-project able to report success while measuring nothing, and the first we
// introduced ourselves while fixing the previous one.
//
// The repair is not a better grep. It is that BOTH BRANCHES OF THE FAILURE
// MESSAGE NOW HAVE TESTS (`BitstreamQueueOverflowTest.cpp`), which pin these
// exact markers. A future rewording breaks a test instead of disarming a gate.
//
// IF YOU CHANGE A MARKER, CHANGE scripts/validate-gpu-encode.mjs TOO - the
// tests name that file so the reader is told where the other half lives.

// The gate greps for this to prove the overflow discard actually freed room.
inline constexpr std::string_view kQueueOverflowDiscardMarker = "overflow-discard";
// The gate greps for this and FAILS the run: a keyframe-less overflow fails the
// sender, its supervisor restarts it, and the encoder is rebuilt - #597 itself.
inline constexpr std::string_view kQueueOverflowFailureMarker =
    "queue overflow with nothing safe to drop";

// FINAL-REVIEW FINDING 6. THERE ARE THREE BRANCHES THAT FAIL THE SENDER ON
// QUEUE OVERFLOW, AND ONLY ONE OF THEM USED TO CARRY A GREPPED MARKER.
// `kQueueOverflowFailureMarker` names the keyframe-less case ALONE; the
// byte-budget case (the cut ran, freed chunks, and the byte budget is still
// over) and the oversized-chunk case (one chunk bigger than the whole budget)
// carried no marker at all. So `overflowFailLines` in
// scripts/validate-gpu-encode.mjs undercounted, and the script's comment - "A
// failed overflow is the ONLY remaining path from a destination fault to an
// encoder rebuild, so a run that hits it has not passed" - claimed a
// completeness the grep did not have. The gate's VERDICT was still safe (the
// status=failed and startedCount assertions catch the outcome either way), but
// its attribution column was not measuring what its comment said.
//
// This marker is carried by ALL THREE failing branches, and it is deliberately
// a PREFIX of the keyframe-less line so that case stays findable as its own
// sub-case. Nothing else in the core logs it: the RESCUED path reads
// "[stream-backpressure] overflow-discard ...".
inline constexpr std::string_view kQueueOverflowSenderFailedMarker =
    "[gpu-encode] bitstream queue overflow";

// Why the arriving chunk could not be admitted even after the cut ran.
// `dropped` is how many chunks that cut actually removed. BOTH return values
// begin with "queue overflow", so kQueueOverflowSenderFailedMarker holds for
// every branch once describeQueueOverflowFailure prepends
// "[gpu-encode] bitstream ".
[[nodiscard]] inline std::string describeQueueOverflowCause(std::size_t dropped,
                                                            std::size_t maxBytes) {
  // Fix round 2, item 2: SAY WHICH ONE HAPPENED. This used to report "no
  // keyframe queued" unconditionally, but the cut can also succeed and still
  // leave the BYTE budget over - a different situation with a different fix,
  // and a sentence that would send a live diagnosis looking for a missing
  // keyframe that was never missing.
  //
  // Fix round 3, item 5: the byte bound is FORMATTED FROM THE CONSTANT rather
  // than spelled out, so a changed cap cannot leave the message describing the
  // old one.
  //
  // FINAL-REVIEW FINDING 5 - the regression the refactor into this file
  // introduced, one commit after the file was created to prevent exactly this
  // class of defect, and which that commit's own review missed. The composition
  // is "[gpu-encode] bitstream " + cause, and this branch's cause used to begin
  // "the cut freed 60-chunk room but ...". The live line therefore read
  // "[gpu-encode] bitstream the cut freed 60-chunk room but the 2 MiB byte
  // budget is still over" - ungrammatical, no longer findable by grepping logs
  // for "queue overflow", missing `dropped` (the one number a diagnostician
  // wants), and printing the CHUNK CAP as though it were the yield of the cut.
  // It now names the yield and only the yield; the chunk cap is published as
  // its own `chunkCap=` field on the full line below, where it cannot be read
  // as a measurement.
  if (dropped > 0) {
    return "queue overflow: the cut dropped " + std::to_string(dropped) +
           " chunk(s) and the " + std::to_string(maxBytes / (1024 * 1024)) +
           " MiB byte budget is still over";
  }
  return std::string(kQueueOverflowFailureMarker) + " (no keyframe queued and none arriving)";
}

// THE THIRD FAILING BRANCH (finding 6): a single chunk larger than the whole
// byte budget. No cut can make room for it, so there is no yield to report -
// but it IS a queue overflow that fails the sender, and it said so nowhere the
// gate could see.
[[nodiscard]] inline std::string describeQueueOverflowOversizedChunk(std::size_t incomingBytes,
                                                                     std::size_t maxBytes) {
  return std::string(kQueueOverflowSenderFailedMarker) +
         ": one chunk is larger than the whole " + std::to_string(maxBytes / (1024 * 1024)) +
         " MiB byte budget, so no cut can admit it; incomingBytes=" +
         std::to_string(incomingBytes) + "; sender unhealthy -> supervisor\n";
}

// The full stderr line for an overflow that FAILED the sender.
[[nodiscard]] inline std::string describeQueueOverflowFailure(std::size_t dropped,
                                                              std::size_t queuedBytes,
                                                              std::size_t queuedChunks,
                                                              std::size_t incomingBytes,
                                                              std::size_t maxChunks,
                                                              std::size_t maxBytes) {
  return "[gpu-encode] bitstream " + describeQueueOverflowCause(dropped, maxBytes) +
         "; queuedBytes=" + std::to_string(queuedBytes) +
         " queuedChunks=" + std::to_string(queuedChunks) +
         " chunkCap=" + std::to_string(maxChunks) +
         " incomingBytes=" + std::to_string(incomingBytes) + "; sender unhealthy -> supervisor\n";
}

// The full stderr line for an overflow the GOP-tail cut rescued.
[[nodiscard]] inline std::string describeQueueOverflowDiscard(std::size_t dropped,
                                                              std::size_t queuedChunks,
                                                              std::size_t queuedBytes) {
  return "[stream-backpressure] " + std::string(kQueueOverflowDiscardMarker) +
         " dropped=" + std::to_string(dropped) +
         " queuedChunks=" + std::to_string(queuedChunks) +
         " queuedBytes=" + std::to_string(queuedBytes) +
         " (queue full; GOP tail dropped instead of failing the sender)\n";
}

}  // namespace corevideo::modules
