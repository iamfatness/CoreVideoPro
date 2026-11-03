#pragma once

#include "core/MediaCore.h"
#include "rpc/Json.h"

#include <iosfwd>
#include <optional>
#include <condition_variable>

namespace corevideo::rpc {

class JsonRpcServer {
 public:
  using JoinHandler = std::function<Json(const Json&, const std::function<bool()>&)>;
  using SnapshotAssembler = std::function<Json(const core::CoreStateCapture&, core::SnapshotProjection)>;
  using ResourceFormatter = std::function<std::string(const Json&)>;
  // Constructor-only observers for forcing the writer's pre-wait shutdown race.
  // No operator/environment binding; unset in production.
  struct WriterWaitTestHooks {
    std::function<void(std::condition_variable&)> beforeWait;
    std::function<void(bool published)> stop;
  };
  // Injectable lifecycle boundary for deterministic delayed-auth/cancellation tests.
  explicit JsonRpcServer(core::MediaCore& mediaCore, JoinHandler joinHandler = {}, SnapshotAssembler assembler = {}, ResourceFormatter formatter = {}, WriterWaitTestHooks writerWaitHooks = {});

  [[nodiscard]] Json handshake() const;
  [[nodiscard]] Json handle(const Json& request);
  void run(std::istream& input, std::ostream& output);

 private:
  // #1048 C1: the engine half of a zoom-media-spine-sync that handle()
  // deferred while the live loop held coreMutex; finished after the release.
  struct DeferredEngineSpine {
    Json id;
    const Json* payload = nullptr;  // into the request; the loop keeps it alive
    double elapsedMs = 0;
  };
  // #1048 C2: answers zoom-snapshot without coreMutex when an engine is
  // configured; nullopt sends the request down the locked path.
  [[nodiscard]] std::optional<Json> handleWithoutCore(const Json& request);
  [[nodiscard]] Json finishDeferredEngineSpine(const DeferredEngineSpine& deferred);
  [[nodiscard]] Json spineSyncResponse(const Json& id, const Json& snapshot) const;
  [[nodiscard]] Json success(const Json& id, Json::Object payload) const;
  [[nodiscard]] Json failure(const Json& id, std::string code, std::string message) const;
  void flushFrameEvents(std::ostream& output);

  core::MediaCore& mediaCore_;
  std::string processEpoch_;
  JoinHandler joinHandler_;
  SnapshotAssembler snapshotAssembler_;
  ResourceFormatter resourceFormatter_;
  WriterWaitTestHooks writerWaitHooks_;
  // Set by run() around handle() for one command (command thread only).
  std::optional<DeferredEngineSpine>* engineSpineDeferral_ = nullptr;
};

}  // namespace corevideo::rpc
