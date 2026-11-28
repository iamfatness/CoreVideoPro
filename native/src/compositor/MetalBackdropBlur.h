#pragma once

// P9 true backdrop blur on Metal: the twin of D3D11BackdropBlur. The kernel,
// geometry, constant layout and CPU reference are compositor/BackdropBlur.h,
// shared with Windows, so both GPUs run the same numbers.
//
// Metal cannot read a render attachment while it is being rendered to, so the
// compositor ends its render encoder, calls apply() (one compute encoder for
// downsample + two blur passes, then one render encoder that composites into
// the target with loadAction Load), and opens a fresh render encoder for the
// rest of the layers. The compute stages read Program directly (it is a
// ShaderRead texture); no copy is made. Hazard tracking orders the stages.
//
// Resource rules match Windows: pipelines compile once in initialize(); the
// two half-float scratch planes are created by reserve() with the Program
// target and reused; apply() creates no texture and never waits.

#if !COREVIDEO_STUB && COREVIDEO_ENABLE_DEV_ADAPTERS && COREVIDEO_WITH_METAL

#include <string>

#include "compositor/BackdropBlur.h"

#ifdef __OBJC__
#import <Metal/Metal.h>

namespace corevideo::modules {

class MetalBackdropBlur {
 public:
  bool initialize(id<MTLDevice> device, std::string& error);
  bool reserve(id<MTLDevice> device, int width, int height);
  bool ready() const { return ready_; }
  int capacityWidth() const { return capacityWidth_; }
  int capacityHeight() const { return capacityHeight_; }
  // Encodes one plate into `target` (BGRA8, ShaderRead + RenderTarget) on
  // `commandBuffer`. No encoder may be open on the command buffer.
  bool apply(id<MTLCommandBuffer> commandBuffer, id<MTLTexture> target, const compositor::BackdropDraw& draw);

 private:
  bool ready_ = false;
  int capacityWidth_ = 0, capacityHeight_ = 0;
  id<MTLComputePipelineState> downsample_ = nil;
  id<MTLComputePipelineState> blur_ = nil;
  id<MTLRenderPipelineState> composite_ = nil;
  id<MTLTexture> lowA_ = nil;
  id<MTLTexture> lowB_ = nil;
};

}  // namespace corevideo::modules

#endif  // __OBJC__
#endif  // !COREVIDEO_STUB && COREVIDEO_ENABLE_DEV_ADAPTERS && COREVIDEO_WITH_METAL
