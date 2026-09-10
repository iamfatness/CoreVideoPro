#pragma once

// Pin the encoder-capacity answer for a test that ARMS A REAL RECORDING.
//
// Without this, `MediaFoundationEncoderSink` consults the live capacity probe,
// which (a) is asynchronous, so the result depends on whether the background
// thread finished, and (b) depends on the GPU in whatever machine runs the
// suite. Neither belongs in a test about MP4 writers. `ForcedEncoderCapacity`
// makes `lookup()` answer synchronously with a machine that has ample hardware,
// so these tests exercise the writer path and nothing else.
//
// Tests about the CAPACITY DECISION itself do not use this — they call the pure
// functions directly (see IsoEncoderAdmissionTest.cpp).

#include "modules/EncoderCapacityProbe.h"

namespace corevideo::testing {

class ForcedEncoderCapacity {
 public:
  explicit ForcedEncoderCapacity(int hardwareSessionCeiling = 16, unsigned logicalProcessors = 64) {
    modules::ProbedEncoderCapacity capacity;
    capacity.status = modules::EncoderProbeStatus::Ready;
    capacity.probed = true;
    capacity.adapterDescription = "Test GPU";
    capacity.hardwareAvailable = hardwareSessionCeiling > 0;
    capacity.hardwareSessionCeiling = hardwareSessionCeiling;
    capacity.probeSessionCap = hardwareSessionCeiling;
    capacity.softwareAvailable = true;
    capacity.logicalProcessors = logicalProcessors;
    modules::EncoderCapacityCache::instance().setForcedCapacityForTesting(&capacity);
  }

  ~ForcedEncoderCapacity() {
    modules::EncoderCapacityCache::instance().setForcedCapacityForTesting(nullptr);
  }

  ForcedEncoderCapacity(const ForcedEncoderCapacity&) = delete;
  ForcedEncoderCapacity& operator=(const ForcedEncoderCapacity&) = delete;
};

}  // namespace corevideo::testing
