#pragma once
#include <cstdint>
#include <memory>
#include <array>

namespace corevideo::graphics {
// Internal backend lease, never accepted over RPC or serialized. An accepted
// revision retains its resource independently of cache lookup. GPU completion
// leases additionally protect every submitted draw.
struct GraphicsPreparedResource {
  enum class Backend { D3D11, Metal, Test };
  Backend backend = Backend::Test;
  uintptr_t deviceIdentity = 0;
  uint64_t signature = 0;
  int width = 0, height = 0;
  std::shared_ptr<const void> native;
  std::array<uint8_t, 32> contentHash{};
  bool hasContentHash = false;
};
}
