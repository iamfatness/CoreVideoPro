#pragma once
// Test-only: defined where GpuWebcamFramer actually renders (roadmap M9.2 Task 9).
// Windows D3D11 builds and the macOS Metal framer behind COREVIDEO_WITH_MAC_VIRTUALCAM.
#if (defined(_WIN32) && COREVIDEO_WITH_D3D11) || (defined(__APPLE__) && COREVIDEO_WITH_MAC_VIRTUALCAM)
#define COREVIDEO_FRAMER_RENDERS 1
#endif
