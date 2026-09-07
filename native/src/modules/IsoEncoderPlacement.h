#pragma once

#include <algorithm>
#include <string>
#include <utility>
#include <vector>

namespace corevideo::modules {

// Per-session ISO encoder placement. This is deliberately separate from
// EncoderPolicy.h: that file decides which codec implementations CoreVideo is
// licensed and willing to ship; this file divides a finite hardware budget
// among independently encoded ISO tracks.
enum class IsoEncoderMode {
  Auto,
  Hardware,
  Software,
};

enum class IsoEncoderPath {
  Unavailable,
  Hardware,
  Software,
};

struct IsoEncoderCapacity {
  int hardwareSessionLimit = 0;
  int reservedHardwareSessions = 0;
  bool hardwareAvailable = false;
  bool softwareAvailable = true;
};

struct IsoEncoderAssignment {
  std::string sourceId;
  IsoEncoderPath path = IsoEncoderPath::Unavailable;
  std::string reason = "capacity-unavailable";
};

inline const char* isoEncoderPathId(IsoEncoderPath path) {
  switch (path) {
    case IsoEncoderPath::Hardware:
      return "hardware";
    case IsoEncoderPath::Software:
      return "software";
    case IsoEncoderPath::Unavailable:
      return "unavailable";
  }
  return "unavailable";
}

// Builds one immutable placement plan at recording arm. Auto consumes the
// remaining hardware budget first, then intentionally spills to the supported
// OS software encoder. Explicit Hardware never substitutes software: it keeps
// operator intent and reports tracks that cannot be placed.
inline std::vector<IsoEncoderAssignment> planIsoEncoders(
    const std::vector<std::string>& sourceIds,
    IsoEncoderMode mode,
    const IsoEncoderCapacity& capacity) {
  std::vector<IsoEncoderAssignment> result;
  result.reserve(sourceIds.size());

  int hardwareRemaining = capacity.hardwareAvailable
                              ? (std::max)(0, capacity.hardwareSessionLimit -
                                                 (std::max)(0, capacity.reservedHardwareSessions))
                              : 0;

  for (const auto& sourceId : sourceIds) {
    IsoEncoderAssignment assignment;
    assignment.sourceId = sourceId;

    if (mode == IsoEncoderMode::Software) {
      if (capacity.softwareAvailable) {
        assignment.path = IsoEncoderPath::Software;
        assignment.reason = "operator-selected";
      }
      result.push_back(std::move(assignment));
      continue;
    }

    if (hardwareRemaining > 0) {
      assignment.path = IsoEncoderPath::Hardware;
      assignment.reason = "preferred";
      --hardwareRemaining;
      result.push_back(std::move(assignment));
      continue;
    }

    if (mode == IsoEncoderMode::Auto && capacity.softwareAvailable) {
      assignment.path = IsoEncoderPath::Software;
      assignment.reason = capacity.hardwareAvailable
                              ? "hardware-capacity-exhausted"
                              : "hardware-unavailable";
    } else {
      assignment.reason = capacity.hardwareAvailable
                              ? "hardware-capacity-exhausted"
                              : "hardware-unavailable";
    }
    result.push_back(std::move(assignment));
  }

  return result;
}

}  // namespace corevideo::modules
