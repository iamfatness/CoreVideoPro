#pragma once

// P9 true backdrop blur on D3D11 (see compositor/BackdropBlur.h for the
// shared kernel, geometry, constant layout and the CPU reference it matches).
//
// Resource rules (realtime-isolation-audit, shared-texture-export-cost):
//   - shaders, states and the constant buffer are created once in initialize();
//   - the three scratch textures are created by reserve() for a target size and
//     reused for every plate of every frame. The compositor reserves them with
//     its Program target (ensureRenderTarget), so a glass graphic going to air
//     allocates nothing; only a LARGER secondary target grows them, once;
//   - apply() never creates, maps for read, flushes or waits. It records one
//     region copy and four draws on the caller's immediate context.
// Device loss: any failed create leaves the pass not ready; apply() then
// refuses and the compositor degrades the frame with a named warning. A device
// that is removed mid-show fails every later call the same way, and the pass
// is rebuilt with the compositor that owns the new device.

#if !COREVIDEO_STUB && COREVIDEO_ENABLE_DEV_ADAPTERS && COREVIDEO_WITH_D3D11

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#include <d3d11.h>

#include <string>

#include "compositor/BackdropBlur.h"
#include "compositor/ComPtrLite.h"

namespace corevideo::modules {

class D3D11BackdropBlur {
 public:
  // Compiles the pass. False (with `error`) leaves it unavailable.
  bool initialize(ID3D11Device* device, std::string& error);
  // Scratch capacity for targets up to width x height. Reuses what it has.
  bool reserve(ID3D11Device* device, int width, int height);
  bool ready() const { return ready_; }
  int capacityWidth() const { return capacityWidth_; }
  int capacityHeight() const { return capacityHeight_; }
  // Blurs `draw` from `targetTexture` (BGRA8, bound through `targetView`)
  // back into it. Leaves the target bound; the caller restores its own
  // shaders, blend, rasterizer, sampler and constant buffer afterwards.
  bool apply(ID3D11DeviceContext* context, ID3D11Texture2D* targetTexture, ID3D11RenderTargetView* targetView,
             int targetWidth, int targetHeight, const compositor::BackdropDraw& draw);

 private:
  bool writeConstants(ID3D11DeviceContext* context, const compositor::BackdropShaderConstants& constants);
  bool ready_ = false;
  int capacityWidth_ = 0, capacityHeight_ = 0;
  ComPtrLite<ID3D11VertexShader> vertex_;
  ComPtrLite<ID3D11PixelShader> downsample_, blur_, composite_;
  ComPtrLite<ID3D11Buffer> constants_;
  ComPtrLite<ID3D11BlendState> opaque_, straightAlpha_;
  ComPtrLite<ID3D11RasterizerState> plain_, scissor_;
  // Region copy of Program (BGRA8) and the two half-float low-resolution planes.
  ComPtrLite<ID3D11Texture2D> copy_, lowA_, lowB_;
  ComPtrLite<ID3D11ShaderResourceView> copyView_, lowAView_, lowBView_;
  ComPtrLite<ID3D11RenderTargetView> lowATarget_, lowBTarget_;
};

}  // namespace corevideo::modules

#endif  // !COREVIDEO_STUB && COREVIDEO_ENABLE_DEV_ADAPTERS && COREVIDEO_WITH_D3D11
