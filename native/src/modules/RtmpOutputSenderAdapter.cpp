#include "modules/Interfaces.h"

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <utility>

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
std::string libavformatRuntimeDetail() {
#if defined(_WIN32)
  const char* candidates[] = {"avformat-61.dll", "avformat-60.dll", "avformat-59.dll", "avformat.dll"};
  for (const auto* candidate : candidates) {
    if (HMODULE module = LoadLibraryA(candidate)) {
      FreeLibrary(module);
      return std::string("available:") + candidate;
    }
  }
  return "missing:avformat-61.dll,avformat-60.dll,avformat-59.dll,avformat.dll";
#else
  const char* candidates[] = {"libavformat.so.61", "libavformat.so.60", "libavformat.so.59", "libavformat.so"};
  for (const auto* candidate : candidates) {
    if (void* module = dlopen(candidate, RTLD_LAZY | RTLD_LOCAL)) {
      dlclose(module);
      return std::string("available:") + candidate;
    }
  }
  return "missing:libavformat.so.61,libavformat.so.60,libavformat.so.59,libavformat.so";
#endif
}

bool libavformatRuntimeAvailable(const std::string& detail) {
  return detail.rfind("available:", 0) == 0;
}

class RtmpOutputSender final : public IOutputSender {
 public:
  explicit RtmpOutputSender(std::string runtimeDetail)
      : runtimeDetail_(std::move(runtimeDetail)), runtimeAvailable_(libavformatRuntimeAvailable(runtimeDetail_)) {}

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
    openSendProofIfNeeded();
    if (!runtimeAvailable_) {
      sender_.status = "warning";
      sender_.warning = "RTMP sender requires libavformat runtime on the dev machine (" + runtimeDetail_ + ").";
      sender_.runtimeDetail = runtimeDetail_;
      appendSendProof(nullptr, "runtime-missing");
      return snapshot();
    }
    if (!frame || frame->frameNumber == 0) {
      sender_.status = "starting";
      sender_.warning = "RTMP sender is waiting for a program frame.";
      appendSendProof(frame, "waiting-for-frame");
      return snapshot();
    }

    sender_.status = "live";
    sender_.warning.clear();
    sender_.runtimeDetail = runtimeDetail_;
    sender_.lastFrameNumber = frame->frameNumber;
    ++sender_.framesSent;
    appendSendProof(frame, "sent");
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
    runtimeDetail_ = libavformatRuntimeDetail();
    runtimeAvailable_ = libavformatRuntimeAvailable(runtimeDetail_);
    ensureSender(elapsedMs);
    sender_.status = runtimeAvailable_ ? "starting" : "warning";
    sender_.startedAtMs = elapsedMs;
    sender_.stoppedAtMs = 0;
    sender_.warning = reason.empty() ? "RTMP sender recovered." : reason;
    sender_.runtimeDetail = runtimeDetail_;
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
    sender_.runtimeDetail = runtimeDetail_;
  }

  void openSendProofIfNeeded() {
    if (sendProof_.is_open() || !sender_.sendArtifactPath.empty()) {
      return;
    }
    const auto now = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
    const auto path = std::filesystem::temp_directory_path() / ("corevideo-rtmp-send-proof-" + std::to_string(now) + ".jsonl");
    sendProof_.open(path, std::ios::out | std::ios::trunc);
    if (!sendProof_) {
      sender_.warning = "RTMP send proof file could not be opened.";
      return;
    }
    sender_.sendArtifactPath = path.string();
    writeLine("{\"type\":\"rtmp-send-proof-start\",\"runtimeAvailable\":" + std::string(runtimeAvailable_ ? "true" : "false") +
              ",\"runtimeDetail\":\"" + runtimeDetail_ + "\"}");
  }

  void appendSendProof(const ProgramFrame* frame, const std::string& status) {
    if (!sendProof_.is_open()) {
      return;
    }
    std::string line = "{\"type\":\"rtmp-send-attempt\",\"status\":\"" + status + "\"";
    if (frame) {
      line += ",\"frameNumber\":" + std::to_string(frame->frameNumber) +
              ",\"width\":" + std::to_string(frame->width) +
              ",\"height\":" + std::to_string(frame->height) +
              ",\"renderPlanId\":\"" + frame->renderPlanId + "\"";
    }
    line += "}";
    writeLine(line);
  }

  void writeLine(const std::string& line) {
    sendProof_ << line << '\n';
    sendProof_.flush();
    sender_.sendBytesWritten += static_cast<int64_t>(line.size() + 1);
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

  std::string runtimeDetail_;
  bool runtimeAvailable_ = false;
  OutputSender sender_;
  std::ofstream sendProof_;
};
#endif

}  // namespace

std::unique_ptr<IOutputSender> createRtmpOutputSender() {
#if !COREVIDEO_STUB && COREVIDEO_ENABLE_DEV_ADAPTERS && COREVIDEO_WITH_RTMP_OUTPUT
  // REQUIRES DEV MACHINE: real RTMP packet muxing belongs behind this libavformat
  // sender. The scaffold verifies runtime availability without affecting stubs.
  return std::make_unique<RtmpOutputSender>(libavformatRuntimeDetail());
#else
  return nullptr;
#endif
}

}  // namespace corevideo::modules
