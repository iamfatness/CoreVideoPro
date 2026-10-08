#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace corevideo::modules {
// A separate recording retention bound, using the same 512 MiB ceiling as
// frame-preparation ownership. It is not a reservation from that GPU pool.
// Account raw references conservatively: file FIFO + dispatch handoff + in-flight.
struct RecordingQueuePolicy {
  static constexpr uint64_t budgetBytes = 512ull * 1024 * 1024;
  static constexpr int dispatchDepth = 6; // conservative: Program 6, each ISO 4
  static bool accepts(int depth, int width, int height, bool programNv12,
                      const std::vector<std::string>& sources) {
    if (depth < 4 || depth > 30 || width <= 0 || height <= 0 || width > 8192 || height > 8192 || sources.size() > 8) return false;
    const uint64_t pixels = static_cast<uint64_t>(width) * height;
    uint64_t perFrame = pixels * (programNv12 ? 3 : 8) / 2 + 320 * 180 * 4;
    for (const auto& id : sources) perFrame += pixels * (id.rfind("zoom:", 0) == 0 ? 3 : 8) / 2;
    // Audio is canonical stereo float at 48k, bounded separately per file.
    const uint64_t audio = static_cast<uint64_t>(sources.size() + 1) * 96 * 960 * 2 * sizeof(float) * 2;
    return perFrame * (depth + dispatchDepth + 1) + audio <= budgetBytes;
  }
};
}
