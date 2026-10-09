#pragma once
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace corevideo::modules {
// Worker-owned output fork. Never modifies the shared clean Program NV12.
// Upload/readback occur only on the camera worker; the built-in PNG is cached.
class GpuWebcamFramer {
 public:
  GpuWebcamFramer();
  ~GpuWebcamFramer();
  GpuWebcamFramer(const GpuWebcamFramer&) = delete;
  GpuWebcamFramer& operator=(const GpuWebcamFramer&) = delete;
  std::shared_ptr<const std::vector<uint8_t>> apply(
      const std::shared_ptr<const std::vector<uint8_t>>& clean, int width, int height,
      bool mirror);
  const std::string& warning() const;
  // Initialize on a bounded preparation thread, then transfer exclusive
  // ownership to the camera worker. Never call prepare/apply concurrently.
  bool prepare(int width, int height);
 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};
}
