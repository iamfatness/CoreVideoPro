#pragma once
#include <cstdint>

namespace corevideo::modules {
// Optional immutable native image. Backend handles never enter IPC/contracts.
// CPU payloads remain independent representations for software and ISO readers.
struct GpuVideoFrame {
  enum class Backend { D3D11Bgra };
  virtual ~GpuVideoFrame() = default;
  Backend backend = Backend::D3D11Bgra;
  int width = 0, height = 0;
  uint64_t generation = 0;
  // A separately allocated optional pool; never a production capture lease.
  bool monitorPrivate = false;
};
}
