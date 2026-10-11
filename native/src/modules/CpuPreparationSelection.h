#pragma once
#include <cstdlib>
#include <string_view>

namespace corevideo::modules {
// Normal Windows module composition requests preparation; direct adapter
// fixtures retain their legacy default. Explicit overrides keep A/B runs honest.
inline bool cpuPreparationEnabled(bool productionDefault = false) {
  const auto* value = std::getenv("COREVIDEO_CPU_SOURCE_PREPARATION");
  return value ? std::string_view(value) == "1" : productionDefault;
}
}
