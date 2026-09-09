#include "core/AtomicTakeRpcAdapter.h"
#include <stdexcept>

namespace corevideo::core {
namespace {
rpc::Json failure(const char* error, bool reconcile = false) {
  return rpc::Json::Object{{"ok", false}, {"type", "take-result"},
      {"error", error}, {"reconcileRequired", reconcile}};
}
}
AtomicTakeRpcAdapter::AtomicTakeRpcAdapter() : AtomicTakeRpcAdapter(Config{}, {}) {}
AtomicTakeRpcAdapter::AtomicTakeRpcAdapter(Config config, std::shared_ptr<AtomicTakeCoordinator> coordinator)
    : config_(config), coordinator_(std::move(coordinator)) {
  if (config_.enabled && !coordinator_) throw std::invalid_argument("Enabled Take adapter requires an authority owner");
}
rpc::Json AtomicTakeRpcAdapter::capabilities() const {
  return rpc::Json::Object{{"atomicTake", config_.enabled}, {"atomicTakeVersion", 1},
      {"clientMediaObservations", false}};
}
rpc::Json AtomicTakeRpcAdapter::handle(const rpc::Json& request) const {
  if (!config_.enabled) return failure("capability-disabled");
  const auto typed = AtomicTakeJsonCodec::decodeRequest(request);
  if (!typed) return failure("invalid-request");
  try {
    const auto reply = coordinator_->take(*typed);
    const auto outcome = AtomicTakeJsonCodec::encodeOutcome(reply.outcome);
    // Failure to represent the result cannot mean that no mutation occurred.
    // Caller retains the exact operation ID/fingerprint and reconciles by replay.
    if (!outcome) return failure("outcome-unavailable", true);
    return rpc::Json::Object{{"ok", reply.outcome.error == AtomicTakeCoordinator::Error::None},
        {"type", "take-result"}, {"outcome", *outcome}, {"replayed", reply.replayed},
        {"retryable", reply.retryable}, {"reconcileRequired", reply.outcome.pending}};
  } catch (...) {
    return failure("outcome-unavailable", true);
  }
}
}
