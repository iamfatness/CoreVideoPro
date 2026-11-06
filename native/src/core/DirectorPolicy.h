#pragma once
#include "rpc/Json.h"
#include <cmath>

namespace corevideo::core {
// Validate a whole operator policy before replacing it. The native director
// applies these preferences while selecting people, not after binding a shot.
struct DirectorPolicy {
  bool preferScreenShare = true;
  int panelGuestThreshold = 4;
  static bool read(const rpc::Json& value, DirectorPolicy& result) {
    const auto* share = value.get("preferScreenShare");
    const auto* panel = value.get("panelGuestThreshold");
    if (!share || !share->isBool() || !panel || !panel->isNumber()) return false;
    const double count = panel->asNumber();
    if (!std::isfinite(count) || count < 2 || count > 10 || count != std::floor(count)) return false;
    result = {share->asBool(), static_cast<int>(count)};
    return true;
  }
  rpc::Json json() const {
    return rpc::Json::Object{{"preferScreenShare", preferScreenShare},
                             {"panelGuestThreshold", panelGuestThreshold}};
  }
};
}
