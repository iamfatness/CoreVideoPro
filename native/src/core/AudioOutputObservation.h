#pragma once

#include "modules/Interfaces.h"
#include "rpc/Json.h"
#include "core/PublishedObservationStore.h"
#include "core/SystemMemoryPolicy.h"
#include <algorithm>

namespace corevideo::core {

// Measured owner facts only. No PCM, GPU leases, devices or desired settings.
struct AudioOutputObservation {
  struct BusTap {
    std::string busId;
    std::size_t samples = 0;
    double peakDbfs = -120, rmsDbfs = -120;
    bool empty() const { return samples == 0; }
    std::size_t size() const { return samples; }
  };
  std::vector<BusTap> busTaps;
  modules::OutputSession encoder;
  modules::OutputSenderSession sender;
  modules::AudioMixMetrics mixer;
  double momentaryLufs = -120.0;
  double shortTermLufs = -120.0;
  double integratedLufs = -120.0;
  double truePeakDbfs = -120.0;
  int windowMs = 0;
  int fallbackMonitorFrames = 0;
  int monitorSampleRate = 48000, monitorChannels = 2;
  SystemMemorySample memory;
  SystemMemoryLevel memoryLevel = SystemMemoryLevel::Unknown;
  std::int64_t memorySampledAtNs = 0;
};

using AudioObservationStore = PublishedObservationStore<AudioOutputObservation>;

inline rpc::Json audioObservationMetadata(const AudioObservationStore::View& view,
                                        const std::string& epoch,
                                        AudioObservationStore::Clock::time_point now = AudioObservationStore::Clock::now()) {
  const auto ageMs = view.payload ? (std::max)(0LL, static_cast<long long>(
      std::chrono::duration_cast<std::chrono::milliseconds>(now - view.payload->publishedAt).count())) : 0LL;
  return rpc::Json::Object{
#if defined(_WIN32) && defined(_MSC_VER)
      // MSVC steady_clock and .NET Stopwatch both wrap system QPC. Consumers
      // can include assembly, pipe and dispatch time in observation age.
      {"clockDomain", "windows-qpc-ns-v1"},
#endif
      {"epoch", epoch}, {"generation", 1},
      {"sequence", static_cast<double>(view.payload ? view.payload->sequence : 0)},
      {"observedRevision", static_cast<double>(view.payload ? view.payload->revision : 0)},
      {"ageMs", static_cast<double>(ageMs)},
      {"status", !view.payload ? "unobserved" : ageMs <= 250 ? "fresh" : ageMs <= 1000 ? "aging" : "stale"},
      {"refused", static_cast<double>(view.refused)}, {"capacity", 4},
      {"budgetBytes", 512 * 1024},
      {"retainedMetadataBytes", static_cast<double>(view.residentBytes)},
      {"publishedAtNs", view.payload ? static_cast<double>(std::chrono::duration_cast<std::chrono::nanoseconds>(view.payload->publishedAt.time_since_epoch()).count()) : 0.0},
      {"version", "audio-owner-observation-v1"},
  };
}

}  // namespace corevideo::core
