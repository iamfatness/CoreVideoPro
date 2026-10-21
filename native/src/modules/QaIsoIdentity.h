#pragma once
#include "core/DeliveryTrace.h"
#include "modules/DeliveryCounterPattern.h"
#include <cstdlib>
#include <string_view>

namespace corevideo::modules {
inline bool qaIsoCounterEnabled() {
  static const bool enabled = [] { const auto* value = std::getenv("COREVIDEO_QA_ISO_COUNTER");
    return value && std::string_view(value) == "1"; }();
  return enabled;
}
// Observes each offered source arrival, not writer success or physical acquisition.
// Queue/ring loss counters and finalized file pixels must be checked separately.
inline void traceQaIsoArrival(const std::string& sourceId, uint64_t epoch, int64_t frameId,
    int64_t observed100ns, int width, int height) {
  if (!qaIsoCounterEnabled()) return;
  core::DeliveryTraceEvent event;
  event.stage = core::DeliveryStage::IsoArrivalOffered;
  event.sourceTag = core::deliveryTraceTag("iso:" + sourceId);
  event.sourceEpoch = epoch; event.sourceFrameId = frameId;
  event.sourceObservation100ns = observed100ns;
  event.layoutTag = (uint64_t(static_cast<uint32_t>(width)) << 32) | static_cast<uint32_t>(height);
  core::recordDeliveryTrace(event);
}
}
