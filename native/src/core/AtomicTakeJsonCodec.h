#pragma once
#include "core/AtomicTakeCoordinator.h"
#include "rpc/Json.h"
#include <optional>

namespace corevideo::core {
// Typed wire boundary only. No certificate lookup, authority mutation, or I/O.
class AtomicTakeJsonCodec final {
 public:
  static std::optional<AtomicTakeCoordinator::Request> decodeRequest(const rpc::Json&);
  static std::optional<AtomicTakeCoordinator::Outcome> decodeOutcome(const rpc::Json&);
  // Reject invalid native values rather than rounding unsafe counters on output.
  static std::optional<rpc::Json> encodeRequest(const AtomicTakeCoordinator::Request&);
  static std::optional<rpc::Json> encodeOutcome(const AtomicTakeCoordinator::Outcome&);
};
}
