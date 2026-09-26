#pragma once

#include <cstdint>
#include <string_view>

namespace corevideo::core {

enum class ControlCommandDecision { Apply, Duplicate, Conflict, Invalid };

// Pure admission decision. The serialized core executor owns the revision and
// bounded operation-result cache; this policy performs no media work.
inline ControlCommandDecision decideControlCommand(
    bool validEnvelope, bool duplicateOperation,
    std::string_view authorityEpoch, std::uint64_t currentRevision,
    std::string_view expectedEpoch, std::uint64_t expectedRevision) {
  if (!validEnvelope) return ControlCommandDecision::Invalid;
  if (duplicateOperation) return ControlCommandDecision::Duplicate;
  if (authorityEpoch != expectedEpoch || currentRevision != expectedRevision)
    return ControlCommandDecision::Conflict;
  return ControlCommandDecision::Apply;
}

}  // namespace corevideo::core
