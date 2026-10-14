#pragma once
#include "modules/Interfaces.h"
#include "rpc/Json.h"
#include <chrono>

namespace corevideo::modules {
inline rpc::Json programDisplayEvidence(const std::shared_ptr<const MonitorRenderResult>& result) {
  const auto value = result ? result->programDisplay : ProgramDisplayObservation{};
  const int64_t now100ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
      std::chrono::steady_clock::now().time_since_epoch()).count() / 100;
  const auto age100ns = value.timelineTimestamp100ns > 0 ? (std::max)(int64_t(0), now100ns - value.timelineTimestamp100ns) : 0;
  constexpr const char* reasons[] = {"none", "read-capacity", "query-create", "query-read-failed",
      "import", "texture-create", "srv-create", "failed-reader"};
  rpc::Json::Object reasonCounts;
  for (size_t i = 1; i < value.refusalReasons.size(); ++i)
    reasonCounts.emplace(reasons[i], static_cast<double>(value.refusalReasons[i]));
  rpc::Json::Array incidents;
  if (value.incidentTrace) for (const auto& event : *value.incidentTrace) {
    incidents.emplace_back(rpc::Json::Object{
        {"observedNs", std::to_string(event.observedNs)}, {"stage", static_cast<double>(event.stage)},
        {"reason", reasons[static_cast<size_t>(event.reason)]},
        {"requestedSequence", std::to_string(event.requested)}, {"copiedSequence", std::to_string(event.copied)},
        {"requestedGeneration", std::to_string(event.generation)},
        {"completedSequence", std::to_string(event.completedSequence)},
        {"targetNs", std::to_string(event.targetNs)}, {"deadlineNs", std::to_string(event.deadlineNs)},
        {"slot", static_cast<double>(event.slot)}, {"width", static_cast<double>(event.width)},
        {"height", static_cast<double>(event.height)}, {"importCached", event.importCached},
        {"hresult", static_cast<double>(event.hresult)}, {"deviceHresult", static_cast<double>(event.deviceHresult)},
        {"pending", static_cast<double>(event.pending)},
        {"pendingSequence0", std::to_string(event.pendingSequences[0])},
        {"pendingSequence1", std::to_string(event.pendingSequences[1])},
        {"pendingAgeNs0", std::to_string(event.pendingAgesNs[0])},
        {"pendingAgeNs1", std::to_string(event.pendingAgesNs[1])}});
  }
  return rpc::Json::Object{
      {"observationVersion", "program-display-snapshot-v1"}, {"observed", value.observed},
      {"state", value.state}, {"generation", static_cast<double>(value.generation)},
      {"requestedSequence", static_cast<double>(value.requestedSequence)},
      {"copiedSequence", static_cast<double>(value.copiedSequence)},
      {"frameNumber", static_cast<double>(value.frameNumber)},
      {"timelineTimestamp100ns", static_cast<double>(value.timelineTimestamp100ns)},
      {"age100ns", static_cast<double>(age100ns)},
      {"copies", static_cast<double>(value.copies)}, {"completedCopies", static_cast<double>(value.completedCopies)},
      {"reused", static_cast<double>(value.reused)}, {"refused", static_cast<double>(value.refused)},
      {"skipped", static_cast<double>(value.skipped)}, {"reordered", static_cast<double>(value.reordered)},
      {"capacity", value.capacity}, {"gpuReads", value.gpuReads},
      {"readerEpoch", std::to_string(value.readerEpoch)}, {"readerReadLimit", value.readerReadLimit},
      {"refusalObservationVersion", "program-display-refusal-v1"},
      {"refusalReasons", std::move(reasonCounts)}, {"notReadyPolls", static_cast<double>(value.notReadyPolls)},
      {"lastHresult", static_cast<double>(value.lastHresult)},
      {"incidentTrace", std::move(incidents)}, {"traceCapacity", 512},
      {"traceOverwritten", std::to_string(value.traceOverwritten)},
      {"traceCaptureFailures", static_cast<double>(value.traceCaptureFailures)},
      {"traceClock", "steady-clock-nanoseconds"}, {"traceAgeIsGpuDuration", false},
      {"allocatedBytes", static_cast<double>(value.allocatedBytes)},
      {"presentationOffset100ns", 166666}, {"displayPresentationVerified", false}};
}
inline rpc::Json monitorInputEvidence(const std::shared_ptr<const MonitorRenderResult>& result) {
  rpc::Json::Array sources;
  if (result) for (const auto& input : result->inputs) {
    if (sources.size() == MonitorRenderDiagnostics::InputCapacity) break;
    sources.emplace_back(rpc::Json::Object{
        {"sourceId", input.sourceId}, {"state", input.state}, {"reason", input.reason},
        {"sourceEpoch", static_cast<double>(input.sourceEpoch)},
        {"requestedEpoch", static_cast<double>(input.requestedEpoch)},
        {"frameId", static_cast<double>(input.frameId)},
        {"captureTimestamp100ns", static_cast<double>(input.captureTimestamp100ns)}});
  }
  return rpc::Json::Object{
      {"observationVersion", "monitor-input-admission-v1"}, {"observed", result != nullptr},
      {"sequence", result ? static_cast<double>(result->sequence) : 0},
      {"omitted", result ? static_cast<double>(result->inputs.size() - sources.size()) : 0},
      {"sources", std::move(sources)}};
}
}
