#pragma once

#include "modules/GpuVideoEncoder.h"

#include <functional>
#include <memory>

namespace corevideo::modules {

// Profile-matched stream destinations subscribe to one hardware Program
// encoder. Each returned client keeps the GpuVideoEncoder contract used by
// RTMP/SRT/HLS; stopping one client only removes its own sink.
class SharedGpuVideoEncoderPool {
 public:
  using Factory = std::function<std::unique_ptr<GpuVideoEncoder>()>;
  explicit SharedGpuVideoEncoderPool(Factory factory);
  ~SharedGpuVideoEncoderPool();

  [[nodiscard]] std::unique_ptr<GpuVideoEncoder> createClient();

 private:
  struct State;
  class Client;
  std::shared_ptr<State> state_;
};

// The production pool lives in the native core process. The default factory
// keeps the same platform fallback decision as the former per-sender encoder.
[[nodiscard]] std::unique_ptr<GpuVideoEncoder> createSharedProgramGpuVideoEncoder();

}  // namespace corevideo::modules
