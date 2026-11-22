#pragma once
// Media clip fixtures for the AVFoundation decoder tests. Every fixture is
// written at test time with AVAssetWriter; nothing binary is committed.
#include <cstdint>
#include <string>
#include <vector>

namespace corevideo::testing {

struct FlatSegment { int frames; uint8_t b, g, r, a = 0xff; };

// H.264 (or ProRes 4444 when `prores4444`) MP4/MOV from BGRA source buffers.
// Frame i is stamped i/fps and the session ends at frames/fps, so the asset's
// duration is exactly frames/fps. Returns "" on success, else why.
std::string writeFlatColourClip(const std::string& path, int width, int height, int fps,
                                const std::vector<FlatSegment>& segments, bool prores4444 = false);

// H.264 MP4 (64x64) from a '420v' (BT.709 limited) source with four 32x32
// quadrants of KNOWN Y/Cb/Cr: TL (16,128,128) black, TR (235,128,128) white,
// BL (173,42,26) BT.709 green, BR (63,102,240) BT.709 red.
std::string writeBt709BarsClip(const std::string& path, int frames, int fps);

// H.264 + AAC MP4: `seconds` of video (flat grey 64x64 @30) and a sine at `toneHz`, 48 kHz stereo,
// amplitude 0.5.
std::string writeToneClip(const std::string& path, int seconds, double toneHz);

// H.264 MP4 whose STORAGE is `width`x`height` with four quadrants (storage orientation):
// TL RED BGR(0x20,0x20,0xE0), TR GREEN BGR(0x20,0xE0,0x20), BL BLUE BGR(0xE0,0x20,0x20),
// BR WHITE BGR(0xD0,0xD0,0xD0), and a track `preferredTransform` of `rotationDegrees` clockwise
// (0, 90, 180 or 270), the way a phone records portrait video. 30 fps, `frames` frames.
std::string writeQuadrantClip(const std::string& path, int width, int height, int rotationDegrees, int frames = 15);

// The same quadrant clip with options. `alpha` below 0xff writes ProRes 4444 in a MOV with every
// pixel at that STRAIGHT alpha (quadrant colours unchanged); `prores` forces ProRes 4444 at any
// alpha. A non-empty `ptsMs` stamps frame i at ptsMs[i] milliseconds instead of i/30 (a
// variable-frame-rate clip; one entry per frame). Use ProRes for VFR: measured on macOS 27.2, the
// H.264 writer merges two samples 1 ms apart (15 decoded frames for 16 written); ProRes keeps all 16.
struct QuadrantClip { int frames = 15; uint8_t alpha = 0xff; bool prores = false; std::vector<int> ptsMs; };
std::string writeQuadrantClip(const std::string& path, int width, int height, int rotationDegrees,
                              const QuadrantClip& options);

// An AUDIO-ONLY file (no video track): `millis` of a sine at `toneHz`, 48 kHz stereo, amplitude 0.5.
// `.wav` paths are written as 16-bit LPCM WAVE, anything else as AAC in an M4A container.
std::string writeToneOnlyAudio(const std::string& path, int millis, double toneHz);

// A flat-colour PNG (straight-alpha BGRA source) written with ImageIO.
std::string writeFlatColourPng(const std::string& path, int width, int height,
                               uint8_t b, uint8_t g, uint8_t r, uint8_t a = 0xff);

}  // namespace corevideo::testing
