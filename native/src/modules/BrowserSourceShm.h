#pragma once

// Shared-memory frame protocol between corevideo-browser-host.exe (writer) and the
// core's BrowserSourceHostAdapter (reader). Deliberately the SAME seqlock layout the
// WinUI capture bridge already uses (WinUiCaptureDeviceAdapter), so browser frames
// ride the exact ingest seam the compositor has been compositing since Phase SC:
//
//   [0]  uint32 sequence   (seqlock: even = stable, odd = writer mid-update)
//   [4]  uint32 width
//   [8]  uint32 height
//   [12] uint32 reserved
//   [16..] tightly packed BGRA (width * height * 4), premultiplied alpha
//
// Pure header (no Win32 includes) so the read/write helpers are unit-testable
// against a plain byte buffer.

#include <cstdint>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

namespace corevideo::modules::browsershm {

constexpr std::size_t kHeaderBytes = 16;

constexpr std::size_t frameBytes(int width, int height) {
  return static_cast<std::size_t>(width) * static_cast<std::size_t>(height) * 4;
}

constexpr std::size_t mappingBytes(int width, int height) {
  return kHeaderBytes + frameBytes(width, height);
}

// The Local\ (same-session) kernel object name for one browser source's frame
// buffer. Derived from the host pid + source ordinal so two app instances (or a
// restarted core) never collide.
inline std::string shmName(unsigned long corePid, int sourceOrdinal) {
#ifdef _WIN32
  return "Local\\CoreVideoPro.browser." + std::to_string(corePid) + "." +
         std::to_string(sourceOrdinal);
#else
  // POSIX shm_open names are capped at ~31 CHARACTERS on macOS (SHM_NAME_MAX);
  // a longer name fails with ENAMETOOLONG and the source silently never maps.
  // This project has already lost time to that ceiling once on the plugin<->
  // engine transport, so keep this aggressively short: "/cvpb." + pid + "." +
  // ordinal is 8 + 10 + 1 + 2 = 21 worst case.
  return "/cvpb." + std::to_string(corePid) + "." + std::to_string(sourceOrdinal);
#endif
}

// Guards the ceiling above at compile-ish time for the caller: a name that would
// be rejected is a silent black source, so callers assert rather than discover
// it at runtime.
inline bool shmNameFitsPlatformLimit(const std::string& name) {
#ifdef _WIN32
  return name.size() < 260;
#else
  return name.size() <= 31;
#endif
}

inline uint32_t loadSeq(const uint8_t* base) {
  return *reinterpret_cast<const volatile uint32_t*>(base);
}

inline void storeSeq(uint8_t* base, uint32_t value) {
  *reinterpret_cast<volatile uint32_t*>(base) = value;
}

inline uint32_t loadU32(const uint8_t* base, std::size_t offset) {
  uint32_t value = 0;
  std::memcpy(&value, base + offset, sizeof(value));
  return value;
}

// Writer side: publish one BGRA frame under the seqlock (odd -> payload -> even).
// `mappedBytes` guards against writing past a mapping that is smaller than the
// frame (a size-mismatched writer must drop the frame, never corrupt the buffer).
// Returns false (and writes nothing) when the frame does not fit.
inline bool writeFrame(uint8_t* base,
                       std::size_t mappedBytes,
                       const uint8_t* bgra,
                       int width,
                       int height) {
  if (base == nullptr || bgra == nullptr || width <= 0 || height <= 0) {
    return false;
  }
  const std::size_t payload = frameBytes(width, height);
  if (kHeaderBytes + payload > mappedBytes) {
    return false;
  }
  const uint32_t start = loadSeq(base) | 1u;  // odd: writer mid-update
  storeSeq(base, start);
  std::memcpy(base + 4, &width, sizeof(uint32_t));
  std::memcpy(base + 8, &height, sizeof(uint32_t));
  const uint32_t reserved = 0;
  std::memcpy(base + 12, &reserved, sizeof(uint32_t));
  std::memcpy(base + kHeaderBytes, bgra, payload);
  storeSeq(base, start + 1);  // even: stable
  return true;
}

struct ReadResult {
  bool gotNewFrame = false;
  std::shared_ptr<std::vector<uint8_t>> pixels;  // tightly packed BGRA
  int width = 0;
  int height = 0;
};

// Reader side: copy out the latest stable frame if the sequence advanced past
// `lastSequence` (which is updated on success). Torn reads (writer raced us) and
// odd sequences return gotNewFrame=false; the caller re-serves its held frame.
inline ReadResult readNewFrame(const uint8_t* base,
                               std::size_t mappedBytes,
                               uint32_t& lastSequence) {
  ReadResult result;
  if (base == nullptr || mappedBytes < kHeaderBytes) {
    return result;
  }
  const uint32_t s1 = loadSeq(base);
  if ((s1 & 1u) != 0 || s1 == lastSequence) {
    return result;  // writer mid-update, or nothing new
  }
  const uint32_t w = loadU32(base, 4);
  const uint32_t h = loadU32(base, 8);
  if (w == 0 || h == 0) {
    return result;
  }
  const std::size_t payload = frameBytes(static_cast<int>(w), static_cast<int>(h));
  if (kHeaderBytes + payload > mappedBytes) {
    return result;  // header claims more pixels than the mapping holds
  }
  auto pixels = std::make_shared<std::vector<uint8_t>>(base + kHeaderBytes,
                                                       base + kHeaderBytes + payload);
  const uint32_t s2 = loadSeq(base);
  if (s2 != s1) {
    return result;  // torn: writer published mid-copy
  }
  lastSequence = s1;
  result.gotNewFrame = true;
  result.pixels = std::move(pixels);
  result.width = static_cast<int>(w);
  result.height = static_cast<int>(h);
  return result;
}

}  // namespace corevideo::modules::browsershm
