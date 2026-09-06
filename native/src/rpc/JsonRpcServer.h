#pragma once

#include "core/MediaCore.h"
#include "rpc/Json.h"

#include <iosfwd>

namespace corevideo::rpc {

class JsonRpcServer {
 public:
  using JoinHandler = std::function<Json(const Json&, const std::function<bool()>&)>;
  // Injectable lifecycle boundary for deterministic delayed-auth/cancellation tests.
  explicit JsonRpcServer(core::MediaCore& mediaCore, JoinHandler joinHandler = {});

  [[nodiscard]] Json handshake() const;
  [[nodiscard]] Json handle(const Json& request);
  void run(std::istream& input, std::ostream& output);

 private:
  [[nodiscard]] Json success(const Json& id, Json::Object payload) const;
  [[nodiscard]] Json failure(const Json& id, std::string code, std::string message) const;
  void flushFrameEvents(std::ostream& output);

  core::MediaCore& mediaCore_;
  std::string processEpoch_;
  JoinHandler joinHandler_;
};

}  // namespace corevideo::rpc
