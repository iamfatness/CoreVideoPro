#pragma once
#include "core/AtomicTakeJsonCodec.h"
#include <memory>

namespace corevideo::core {
// Opt-in boundary for a future authoritative executor. Not registered by MediaCore.
// The coordinator owner supplies preparation and trusted media observations;
// this client RPC surface cannot mint certificates or claim rendered/delivered.
class AtomicTakeRpcAdapter final {
 public:
  struct Config { bool enabled{false}; };
  AtomicTakeRpcAdapter();
  AtomicTakeRpcAdapter(Config config, std::shared_ptr<AtomicTakeCoordinator> coordinator);
  [[nodiscard]] rpc::Json capabilities() const;
  [[nodiscard]] rpc::Json handle(const rpc::Json& request) const;
 private:
  const Config config_;
  const std::shared_ptr<AtomicTakeCoordinator> coordinator_;
};
}
