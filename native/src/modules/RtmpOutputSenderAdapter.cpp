#include "modules/Interfaces.h"

#include <algorithm>
#include <memory>

#if !COREVIDEO_STUB && COREVIDEO_ENABLE_DEV_ADAPTERS && COREVIDEO_WITH_RTMP_OUTPUT
#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <dlfcn.h>
#endif
#endif

namespace corevideo::modules {
namespace {

#if !COREVIDEO_STUB && COREVIDEO_ENABLE_DEV_ADAPTERS && COREVIDEO_WITH_RTMP_OUTPUT
bool libavformatRuntimeAvailable() {
#if defined(_WIN32)
  const char* candidates[] = {"avformat-61.dll", "avformat-60.dll", "avformat-59.dll", "avformat.dll"};
  for (const auto* candidate : candidates) {
    if (HMODULE module = LoadLibraryA(candidate)) {
      FreeLibrary(module);
      return true;
    }
  }
  return false;
#else
  const char* candidates[] = {"libavformat.so.61", "libavformat.so.60", "libavformat.so.59", "libavformat.so"};
  for (const auto* candidate : candidates) {
    if (void* module = dlopen(candidate, RTLD_LAZY | RTLD_LOCAL)) {
      dlclose(module);
      return true;
    }
  }
  return false;
#endif
}

class RtmpOutputSender final : public IOutputSender {
 public:
  explicit RtmpOutputSender(bool runtimeAvailable) : runtimeAvailable_(runtimeAvailable) {}

  OutputSenderSession sync(const std::vector<std::string>& destinations, const ProgramFrame* frame, double elapsedMs) override {
    const bool wantsRtmp = std::find(destinations.begin(), destinations.end(), "rtmp") != destinations.end();
    if (!wantsRtmp) {
      if (sender_.status != "idle" && sender_.status != "stopped") {
        sender_.status = "stopped";
        sender_.stoppedAtMs = elapsedMs;
        sender_.warning.clear();
      }
      return snapshot();
    }

    ensureSender(elapsedMs);
    if (!runtimeAvailable_) {
      sender_.status = "warning";
      sender_.warning = "RTMP sender requires libavformat runtime on the dev machine.";
      return snapshot();
    }
    if (!frame || frame->frameNumber == 0) {
      sender_.status = "starting";
      sender_.warning = "RTMP sender is waiting for a program frame.";
      return snapshot();
    }

    sender_.status = "live";
    sender_.warning.clear();
    sender_.lastFrameNumber = frame->frameNumber;
    ++sender_.framesSent;
    return snapshot();
  }

  OutputSenderSession fail(const std::string& destination, const std::string& message, double elapsedMs) override {
    if (destination != "rtmp") {
      return snapshot();
    }
    ensureSender(elapsedMs);
    sender_.status = "failed";
    sender_.stoppedAtMs = elapsedMs;
    ++sender_.retryCount;
    sender_.warning = message;
    return snapshot();
  }

  OutputSenderSession recover(const std::string& destination, double elapsedMs, const std::string& reason) override {
    if (destination != "rtmp") {
      return snapshot();
    }
    runtimeAvailable_ = libavformatRuntimeAvailable();
    ensureSender(elapsedMs);
    sender_.status = runtimeAvailable_ ? "starting" : "warning";
    sender_.startedAtMs = elapsedMs;
    sender_.stoppedAtMs = 0;
    sender_.warning = reason.empty() ? "RTMP sender recovered." : reason;
    return snapshot();
  }

  OutputSenderSession session() const override { return snapshot(); }

 private:
  void ensureSender(double elapsedMs) {
    if (!sender_.senderId.empty()) {
      return;
    }
    sender_.senderId = "rtmp:program";
    sender_.destination = "rtmp";
    sender_.status = "starting";
    sender_.startedAtMs = elapsedMs;
    sender_.latencyMs = 2100;
    sender_.bitrateMbps = 6.0;
  }

  OutputSenderSession snapshot() const {
    OutputSenderSession session;
    if (!sender_.senderId.empty()) {
      session.senders.push_back(sender_);
      if (sender_.status == "live" || sender_.status == "warning" || sender_.status == "starting") {
        session.activeSenderCount = 1;
      }
      if (!sender_.warning.empty()) {
        session.warnings.push_back(sender_.warning);
      }
      session.status = sender_.status == "failed" ? "failed" : !sender_.warning.empty() || sender_.status == "warning" ? "warning" : session.activeSenderCount > 0 ? "live" : "idle";
    }
    return session;
  }

  bool runtimeAvailable_ = false;
  OutputSender sender_;
};
#endif

}  // namespace

std::unique_ptr<IOutputSender> createRtmpOutputSender() {
#if !COREVIDEO_STUB && COREVIDEO_ENABLE_DEV_ADAPTERS && COREVIDEO_WITH_RTMP_OUTPUT
  // REQUIRES DEV MACHINE: real RTMP packet muxing belongs behind this libavformat
  // sender. The scaffold verifies runtime availability without affecting stubs.
  return std::make_unique<RtmpOutputSender>(libavformatRuntimeAvailable());
#else
  return nullptr;
#endif
}

}  // namespace corevideo::modules
