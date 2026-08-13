#pragma once

// Recording artifact naming + small format helpers shared by the platform
// encoder sinks (Media Foundation on Windows, AVFoundation on macOS). Copied
// VERBATIM from MediaFoundationEncoderAdapter.cpp's anonymous namespace so the
// two sinks produce byte-identical folder schemes, ISO file names, and
// manifest shapes (`<prefix>-<yyyymmdd-hhmmss>/`, `ISO-NN-<SafeName>.mp4`).
// The MF adapter still carries its private copies — deduping it must wait for
// a verified Windows build (that configuration has no CI); when that happens,
// delete its copies and include this header. Portable: <chrono>/<ctime>/
// <cstring>/<string>/<vector> only.

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <string>
#include <vector>

namespace corevideo::modules::recording {

// ISO-1: interleave tightly packed I420 planes into NV12 (plane copy only, no
// color math). Runs on the async encoder writer thread — off every lock.
inline void i420ToNv12(const uint8_t* i420, int w, int h, std::vector<uint8_t>& out) {
  const size_t ySize = static_cast<size_t>(w) * static_cast<size_t>(h);
  const size_t chromaW = static_cast<size_t>(w) / 2;
  const size_t chromaH = static_cast<size_t>(h) / 2;
  const size_t uSize = chromaW * chromaH;
  out.resize(ySize + uSize * 2);
  std::memcpy(out.data(), i420, ySize);
  const uint8_t* u = i420 + ySize;
  const uint8_t* v = i420 + ySize + uSize;
  uint8_t* uv = out.data() + ySize;
  for (size_t i = 0; i < uSize; ++i) {
    uv[2 * i] = u[i];
    uv[2 * i + 1] = v[i];
  }
}

// ISO-2: normalize a raw-stem PCM buffer to interleaved STEREO (the ISO
// writers' uniform AAC layout). Mono → duplicated L/R; stereo → passed
// through; >2ch → first two channels.
inline void toStereo(const std::vector<float>& in, int channels, int frameCount,
                     std::vector<float>& out) {
  out.resize(static_cast<size_t>(frameCount) * 2);
  const int ch = channels > 0 ? channels : 1;
  const size_t have = in.size();
  for (int i = 0; i < frameCount; ++i) {
    const size_t base = static_cast<size_t>(i) * ch;
    float l = 0.0f;
    float r = 0.0f;
    if (base < have) {
      l = in[base];
      r = ch >= 2 && base + 1 < have ? in[base + 1] : l;  // mono → both channels
    }
    out[static_cast<size_t>(i) * 2] = l;
    out[static_cast<size_t>(i) * 2 + 1] = r;
  }
}

// Sanitize a roster/display name into a safe MP4 file-name fragment (spec §5).
inline std::string sanitizeForFilename(const std::string& name, const std::string& fallback) {
  std::string out;
  out.reserve(name.size());
  bool lastDash = false;
  for (char c : name) {
    const bool alnum = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9');
    if (alnum || c == '_') {
      out.push_back(c);
      lastDash = false;
    } else if (c == ' ' || c == '-' || c == '.') {
      if (!out.empty() && !lastDash) {
        out.push_back('-');
        lastDash = true;
      }
    }
    // all other characters (incl. path separators, control chars) are dropped
    if (out.size() >= 48) break;
  }
  while (!out.empty() && out.back() == '-') out.pop_back();
  if (out.empty()) {
    for (char c : fallback) {
      const bool alnum = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9');
      if (alnum) out.push_back(c);
      if (out.size() >= 32) break;
    }
  }
  if (out.empty()) out = "source";
  return out;
}

// Per-session subfolder timestamp `yyyymmdd-hhmmss` (local time).
inline std::string sessionTimestampFolder() {
  const auto now = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
  std::tm tmBuf{};
#if defined(_WIN32)
  localtime_s(&tmBuf, &now);
#else
  localtime_r(&now, &tmBuf);
#endif
  char out[32] = {0};
  std::strftime(out, sizeof(out), "%Y%m%d-%H%M%S", &tmBuf);
  return std::string(out);
}

// Minimal JSON string escape for the manifest writer.
inline std::string jsonEscape(const std::string& in) {
  std::string out;
  out.reserve(in.size() + 8);
  for (char c : in) {
    switch (c) {
      case '"': out += "\\\""; break;
      case '\\': out += "\\\\"; break;
      case '\n': out += "\\n"; break;
      case '\r': out += "\\r"; break;
      case '\t': out += "\\t"; break;
      default:
        if (static_cast<unsigned char>(c) < 0x20) {
          char buf[8];
          std::snprintf(buf, sizeof(buf), "\\u%04x", c);
          out += buf;
        } else {
          out += c;
        }
    }
  }
  return out;
}

}  // namespace corevideo::modules::recording
