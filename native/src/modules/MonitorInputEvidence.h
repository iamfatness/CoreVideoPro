#pragma once
#include "modules/Interfaces.h"
#include "rpc/Json.h"

namespace corevideo::modules {
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
