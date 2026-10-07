#pragma once
#include <cstdint>
#include <string>

namespace corevideo::modules {
// Optional immutable native image. Backend handles never enter IPC/contracts.
// CPU payloads remain independent representations for software and ISO readers.
struct GpuVideoFrame {
  enum class Backend { D3D11Bgra, D3D11I420 };
  virtual ~GpuVideoFrame() = default;
  Backend backend = Backend::D3D11Bgra;
  int width = 0, height = 0;
  uint64_t generation = 0;
  // A separately allocated optional pool; never a production capture lease.
  bool monitorPrivate = false;
  // Present only on uniquely published prepared CPU-source views. Pool storage
  // must never mutate these fields under a retained descriptor/read lease.
  uint64_t sourceEpoch = 0;
  std::string sourceId;
  int64_t sourceFrameId = -1, sourceCaptureTimestamp100ns = 0;
};
}
