#pragma once

#include <cstdint>
#include <string>
#include <vector>

// Pure, platform-free helpers for the native UVC (Media Foundation) capture
// adapter. Everything in this header is deterministic and unit-testable in the
// stub build: format negotiation, NV12/YUY2 -> I420 repacking, the stable
// device-id hash shared with the WinUI shell, and the YUV color-space hints
// that drive the GPU conversion shader. The Media Foundation plumbing itself
// lives in UvcCaptureDeviceAdapter.cpp behind COREVIDEO_WITH_UVC.
namespace corevideo::modules::uvc {

// One native media type advertised by a capture device stream.
struct UvcFormatCandidate {
  int width = 0;
  int height = 0;
  int fpsNumerator = 0;
  int fpsDenominator = 1;
  // FourCC of the native subtype: "NV12", "YUY2", "MJPG", or anything else.
  std::string fourcc;
  // The media-type index on the source stream (opaque to the picker; carried
  // through so the adapter can re-fetch the winning IMFMediaType).
  int nativeIndex = -1;
};

[[nodiscard]] double uvcCandidateFps(const UvcFormatCandidate& candidate);

// Picks the best native format for the product target (1080p60 by default).
// Returns the index into `candidates`, or -1 when the list is empty.
//
// Preference order (lexicographic — quality first, per the north star):
//   1. Resolution: largest area that does not exceed the target on either
//      axis; if nothing fits under the target, the smallest area above it.
//   2. Frame rate: the smallest rate >= the target (60 beats 120 beats 30);
//      if nothing reaches the target, the largest available.
//   3. Subtype: NV12 > YUY2 > MJPG > anything else. Resolution and rate
//      outrank subtype on purpose — a camera that only reaches 1080p60 over
//      MJPG (the classic USB2 webcam) still negotiates 1080p60; the MJPG
//      decode happens inside Media Foundation on the capture thread, never on
//      the render path.
[[nodiscard]] int pickBestUvcFormat(
    const std::vector<UvcFormatCandidate>& candidates,
    int targetWidth = 1920,
    int targetHeight = 1080,
    double targetFps = 60.0);

// Subtype preference rank used by pickBestUvcFormat (0 is best). Exposed for
// tests.
[[nodiscard]] int uvcSubtypeRank(const std::string& fourcc);

// Repacks an NV12 image (Y plane + interleaved half-height UV plane) into a
// tightly packed I420 buffer (Y, then U, then V). This is a byte reshuffle
// during the mandatory copy out of the Media Foundation sample buffer — no
// per-pixel color math; the YUV->RGB conversion stays on the GPU (the same
// HLSL path Zoom I420 frames use). `width`/`height` must be even.
void nv12ToI420(
    const uint8_t* yPlane,
    int yStride,
    const uint8_t* uvPlane,
    int uvStride,
    int width,
    int height,
    std::vector<uint8_t>& out);

// Repacks a packed YUY2 (4:2:2, Y0 U Y1 V) image into tightly packed I420,
// averaging each vertical chroma pair down to 4:2:0. Fallback for devices
// where the NV12 output path cannot be negotiated. `width`/`height` must be
// even.
void yuy2ToI420(
    const uint8_t* source,
    int sourceStride,
    int width,
    int height,
    std::vector<uint8_t>& out);

// Stable capture-device id derived from the Windows device-interface symbolic
// link: lowercase hex of the first 8 bytes of SHA-256(UTF-8 symbolic link).
// MUST stay in lockstep with the WinUI shell's
// CaptureDeviceDiscoveryMapper.CreateStableDeviceId so a scene route's
// `captureDeviceId` resolves to the same `capture:<id>` source whether the
// frames arrive from the native UVC adapter or the WinUI shared-memory bridge.
[[nodiscard]] std::string stableCaptureDeviceIdFromSymbolicLink(const std::string& symbolicLink);

// Color-space hints for the GPU YUV->RGB conversion of a captured frame.
struct YuvColorHints {
  // True when the device/decoder marks the output 0-255 (MFNominalRange_Normal).
  // Cameras typically deliver studio swing (16-235), so the default is false.
  bool fullRange = false;
  // True when the frame should be converted with BT.601 coefficients.
  bool bt601 = false;
};

// Derives the hints from the Media Foundation output media type attributes.
// `nominalRange` is the raw MFNominalRange value (0 = unknown, 1 = 0-255,
// 2 = 16-235); `transferMatrix` is the raw MFVideoTransferMatrix value
// (0 = unknown, 1 = BT.709, 2 = BT.601). Unknown range defaults to limited
// (the broadcast/OBS default for capture devices); unknown matrix defaults by
// frame height: >= 720 is BT.709, below is BT.601.
[[nodiscard]] YuvColorHints deriveYuvColorHints(int nominalRange, int transferMatrix, int frameHeight);

// ---------------------------------------------------------------------------
// No-first-frame watchdog (single-consumer capture-card safety).
//
// A UVC device that OPENS and NEGOTIATES a format but then delivers no samples
// (the classic single-consumer capture card — Elgato Game Capture / HD60 S+ —
// held by another app, or an HDMI source with no signal) used to sit forever on
// a placeholder tile: open+negotiate succeeded, so no stall policy ever fired.
// This is the native-reader analogue of the shell's CaptureReaderStallPolicy:
// once negotiation succeeds, the reader gives the device a bounded window to
// produce its FIRST frame; past that, it fails LOUD and releases the device so
// the WinUI MediaCapture bridge (the robust default) can take over.
// ---------------------------------------------------------------------------

// Default window (ms) a negotiated UVC device gets to deliver its first frame
// before the reader declares a no-first-frame stall. Long enough to cover a
// slow capture-card handshake, short enough that an operator is not left on a
// blank tile.
constexpr int64_t kUvcNoFirstFrameTimeoutMs = 4000;

// True when a running, negotiated session has produced no frame
// (`frameId == 0`) and has been waiting at least `timeoutMs`. `frameId > 0`
// (a first frame already arrived) is never a stall.
[[nodiscard]] bool uvcNoFirstFrameTimedOut(
    int64_t frameId, int64_t elapsedMs, int64_t timeoutMs = kUvcNoFirstFrameTimeoutMs);

// The LOUD, human-readable warning surfaced on the device (connectionState
// "error") when the watchdog fires — names the device and the likely cause so
// the operator (and the shell fallback log) sees WHY the camera went dark.
[[nodiscard]] std::string uvcNoFirstFrameWarning(const std::string& deviceName, int64_t timeoutMs);

}  // namespace corevideo::modules::uvc
