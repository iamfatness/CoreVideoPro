#pragma once
#include <string>
#include <vector>

namespace corevideo::modules {
enum class SourceVideoConsumer { Program, Preview, Multiview, Inspector, Popout, Iso, SceneEditor };
enum class SourceVideoRepresentation { Gpu, Cpu };
struct SourceVideoDemand {
  std::string sourceId;
  SourceVideoConsumer consumer = SourceVideoConsumer::Program;
  std::string instance;
  SourceVideoRepresentation representation = SourceVideoRepresentation::Gpu;
};
inline bool sourceNeedsCpuVideo(const std::vector<SourceVideoDemand>& demands, const std::string& id) {
  for (const auto& demand : demands)
    if (demand.sourceId == id && demand.representation == SourceVideoRepresentation::Cpu) return true;
  return false;
}
}
