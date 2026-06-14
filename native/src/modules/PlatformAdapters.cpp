#include "modules/Interfaces.h"

#if !COREVIDEO_STUB && COREVIDEO_ENABLE_DEV_ADAPTERS

namespace corevideo::modules {

// REQUIRES DEV MACHINE: Zoom Meeting SDK adapter owns clean participant audio/video
// subscription, metadata, active-speaker events, and meeting lifecycle.

// REQUIRES DEV MACHINE: Blackmagic DeckLink adapter exposes SDI capture/output behind
// ICaptureDevice/IEncoderSink once DeckLink SDK headers and hardware are installed.

// REQUIRES DEV MACHINE: AJA NTV2 adapter exposes AJA device enumeration and routing
// behind the same capture/output interfaces.

// REQUIRES DEV MACHINE: D3D11/Metal compositor implementations consume scene graph
// commands behind ICompositor. Linux CI remains on the CPU/no-op compositor.

// REQUIRES DEV MACHINE: NVENC, QuickSync, AMF, and VideoToolbox encoder sinks live
// behind IEncoderSink and are linked only by platform-specific build presets.

}  // namespace corevideo::modules

#endif
