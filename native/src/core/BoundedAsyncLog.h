#pragma once

#include <cstdarg>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string_view>

namespace corevideo::core {

// Diagnostic delivery is best effort. Neither a full pipe nor queue pressure may
// stop media work. The worker owns its state even after its facade is destroyed.
class BoundedAsyncLog {
 public:
  static constexpr std::size_t kCapacity = 128;
  static constexpr std::size_t kMessageBytes = 2048;
  using Sink = std::function<bool(std::string_view)>;
  struct Stats {
    std::uint64_t dropped = 0, truncated = 0, sinkFailures = 0;
    std::size_t queued = 0;
  };
  explicit BoundedAsyncLog(Sink sink);
  ~BoundedAsyncLog();
  BoundedAsyncLog(const BoundedAsyncLog&) = delete;
  BoundedAsyncLog& operator=(const BoundedAsyncLog&) = delete;
  void write(std::string_view message) noexcept;
  [[nodiscard]] Stats stats() const;
 private:
  struct State;
  std::shared_ptr<State> state_;
};

void nativeLogf(const char* format, ...) noexcept;
[[nodiscard]] BoundedAsyncLog::Stats nativeLogStats();

}  // namespace corevideo::core
