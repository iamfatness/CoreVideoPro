#pragma once
// macOS OH Framer (roadmap M9.2 Task 9). Apple + COREVIDEO_WITH_MAC_VIRTUALCAM only.
#include <cstdint>
#include <string>
#include <vector>

namespace corevideo::modules {
// Decodes the built-in OH framer PNG (webcamFramerPng) to tightly packed
// STRAIGHT (non-premultiplied) RGBA8, exactly the bytes WIC's 32bppRGBA
// conversion hands the D3D11 framer: raw samples, no colour management, no
// premultiply. Returns an empty string on success, otherwise the failed
// operation's name for the "OH Framer: <operation> failed." warning.
std::string decodeWebcamFramerRgba(std::vector<uint8_t>& rgba, int& width, int& height);
}
