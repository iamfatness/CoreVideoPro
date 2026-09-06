#pragma once
#include "rpc/Json.h"
#include <chrono>
#include <deque>
#include <optional>
#include <string>

namespace corevideo::rpc {
// Caller serializes access. Limits account for wire bytes; parsed storage is proportional.
// Coalescing is opt-in and never crosses a command boundary or drops embedded actions.
class CommandMailbox {
 public:
  struct Entry {
    Json request;
    std::size_t bytes;
    std::chrono::steady_clock::time_point enqueuedAt;
  };
  enum class Result { accepted, superseded, overloaded };
  explicit CommandMailbox(std::size_t capacity = 128, std::size_t byteLimit = 8 * 1024 * 1024,
                          std::size_t reserved = 8)
      : capacity_(capacity), byteLimit_(byteLimit), reserved_(reserved) {}
  static bool replaceable(const Json& request) {
    const auto type = request.getString("type");
    if (type != "media-core-sync" && type != "native-media-core-sync") return false;
    const auto* optIn = request.get("replaceableFullState");
    if (!optIn || !optIn->asBool()) return false;
    const auto* commands = request.get("commands");
    if (!commands || !commands->isArray()) return false;
    for (const auto& command : commands->asArray()) {
      const auto name = command.getString("type");
      if (name != "load-scene-graph" && name != "set-preview-scene" &&
          name != "sync-participant-audio-mix" && name != "sync-audio-routing-matrix" &&
          name != "set-multiview-layout") return false;
    }
    return true;
  }
  static bool urgent(const Json& request) {
    const auto type = request.getString("type");
    if (type == "zoom-leave" || type == "zoom-cancel" || type == "zoom-stop-capture") return true;
    const auto* commands = request.get("commands");
    if (commands && commands->isArray()) {
      for (const auto& command : commands->asArray()) {
        const auto name = command.getString("type");
        if (name == "stop-recording-session" || name == "stop-encoder-session") return true;
      }
    }
    return type == "stop-recording-session" || type == "stop-encoder-session";
  }
  Result push(Entry entry, std::optional<Json>& superseded) {
    superseded.reset();
    const bool replaceTail = !queue_.empty() && replaceable(entry.request) &&
        replaceable(queue_.back().request) &&
        entry.request.getString("type") == queue_.back().request.getString("type") &&
        entry.request.getString("coalescingKey") == queue_.back().request.getString("coalescingKey") &&
        entry.request.getString("coalescingKey") != "";
    const auto replacedBytes = replaceTail ? queue_.back().bytes : 0;
    const auto limit = urgent(entry.request) ? capacity_ : capacity_ - (std::min)(reserved_, capacity_);
    // Reserve one eighth of bytes for cancellation/stop as well as queue entries.
    const auto bytesLimit = urgent(entry.request) ? byteLimit_ : byteLimit_ - byteLimit_ / 8;
    if (entry.bytes > bytesLimit || bytes_ - replacedBytes > bytesLimit - entry.bytes ||
        queue_.size() - (replaceTail ? 1 : 0) >= limit) return Result::overloaded;
    if (replaceTail) {
      superseded = queue_.back().request;
      bytes_ -= queue_.back().bytes;
      queue_.pop_back();
    }
    bytes_ += entry.bytes;
    queue_.push_back(std::move(entry));
    return replaceTail ? Result::superseded : Result::accepted;
  }
  Entry pop() {
    auto entry = std::move(queue_.front());
    bytes_ -= entry.bytes;
    queue_.pop_front();
    return entry;
  }
  bool empty() const { return queue_.empty(); }
  std::size_t size() const { return queue_.size(); }
  std::size_t bytes() const { return bytes_; }
 private:
  std::deque<Entry> queue_;
  std::size_t capacity_, byteLimit_, reserved_, bytes_ = 0;
};
} // namespace corevideo::rpc
