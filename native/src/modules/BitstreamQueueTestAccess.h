#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>

#include "modules/Interfaces.h"

namespace corevideo::modules {

// #611: the test-only seams that used to ride on IOutputSender as five default-bodied virtuals.
// They are not part of the output-sender contract, no wrapper (AsyncOutputSender,
// SupervisedOutputSender, CompositeOutputSender, NDI) forwards them, and a default body that
// silently did nothing was the shape that hid bugs elsewhere (1-arg connect(), SRT's dropped
// pollAudioFrames). Here every method is pure, so a sender that offers the seams must implement
// all of them, and a test can only reach them through the CONCRETE RTMP sender that
// createRtmpOutputSenderForTest() returns.
//
// TEST-ONLY, structural guard only: no env var, command, config key or wire field reaches this,
// and nothing outside native/tests calls it. (A compile-time gate is impossible:
// corevideo-native-tests links the same corevideo_native library the product does.)

// One read of the bitstream queue's true state, so a test needs ONE call, not three.
struct BitstreamQueueSnapshotForTest {
  std::size_t depth = 0;
  bool hasKeyframe = false;
  std::int64_t bufferedMs = 0;
  // #597 Task 8b. True once the queue's overflow path has FAILED the sender
  // (BitstreamFailure::QueueOverflow). The sender-level symptom - status
  // "failed", lastResultCode "bitstream-queue-overflow" - only appears on a
  // sync() tick that actually reaches submitFrameToGpuEncoder(), i.e. with a
  // real hardware encoder and a launched FFmpeg, so a test that drives the
  // queue alone needs to read the failure where it is RECORDED.
  bool overflowFailed = false;
};

class IBitstreamQueueTestAccess {
 public:
  virtual ~IBitstreamQueueTestAccess() = default;

  // Overrides the bitstream-queue measurement #597's Lever A observes, so the backpressure
  // decision can be driven without a hardware encoder and a congested network. A NEGATIVE
  // bufferedMs clears the override and restores the real measurement.
  virtual void setBackpressureObservationForTest(std::int64_t bufferedMs, bool keyframeInQueue) = 0;

  // Answers "would THIS program frame make the sender flip its encode path?" (tear down FFmpeg
  // AND the hardware encoder and relaunch both). Evaluates the exact comparison
  // ensureFfmpegProcess makes and then ADOPTS the result the way a restart would, so a test can
  // walk a frame sequence and count restarts.
  virtual bool wouldRestartForEncodePathForTest(const ProgramFrame& frame) = 0;

  // Pushes a chunk straight onto the bitstream queue, bypassing the encoder, so a discard
  // test can build a backlog (#597 Lever B).
  virtual void enqueueBitstreamChunkForTest(std::size_t bytes, bool keyframe) = 0;

  // Offers a chunk through the REAL enqueueBitstream(), where the 60-chunk / 2 MiB bound and the
  // last-resort GOP-tail discard live (#597 Task 8b).
  virtual void offerBitstreamChunkForTest(std::size_t bytes, bool keyframe) = 0;

  // Depth, keyframe presence, and the real bitstreamBufferedMs(), unaffected by
  // setBackpressureObservationForTest()'s override.
  virtual BitstreamQueueSnapshotForTest bitstreamQueueSnapshotForTest() const = 0;
};

// The concrete RTMP sender plus its test access. `queue` points into `sender` and is valid for
// its lifetime. Both are null when the RTMP adapter is not compiled in (same as
// createRtmpOutputSender()).
struct RtmpOutputSenderForTest {
  std::unique_ptr<IOutputSender> sender;
  IBitstreamQueueTestAccess* queue = nullptr;
};

RtmpOutputSenderForTest createRtmpOutputSenderForTest();

}  // namespace corevideo::modules
