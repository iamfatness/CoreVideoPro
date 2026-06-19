#pragma once

#include "modules/Interfaces.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <numeric>
#include <sstream>
#include <string>
#include <vector>

namespace corevideo::modules {

inline int clampAudioInt(int value, int minValue, int maxValue) {
  return std::max(minValue, std::min(maxValue, value));
}

inline double clampAudioDouble(double value, double minValue, double maxValue) {
  if (!std::isfinite(value)) {
    return minValue;
  }
  return std::max(minValue, std::min(maxValue, value));
}

inline int64_t clampAudioInt64(int64_t value, int64_t minValue, int64_t maxValue) {
  return std::max(minValue, std::min(maxValue, value));
}

struct AudioDspTimingReference {
  bool hasPreviousTimestamp = false;
  int64_t previousTimestampMs = 0;
  bool hasMixReferenceTimestamp = false;
  int64_t mixReferenceTimestampMs = 0;
};

struct BoundedAudioFrame {
  std::string participantId;
  int sampleRate = 48000;
  int channels = 1;
  int64_t timestampMs = 0;
  int sampleCount = 960;
  double rmsLevel = 0.0;
  double peakLevel = 0.0;
  double noiseFloorDb = -60.0;
  bool voiceActive = true;
  bool invalidShape = false;
};

inline BoundedAudioFrame boundAudioFrame(const AudioFrame& frame) {
  BoundedAudioFrame bounded;
  bounded.participantId = frame.participantId.empty() ? "unknown-audio-source" : frame.participantId;
  bounded.sampleRate = clampAudioInt(frame.sampleRate, 8000, 192000);
  bounded.channels = clampAudioInt(frame.channels, 1, 8);
  bounded.timestampMs = clampAudioInt64(frame.timestampMs, 0, 24LL * 60 * 60 * 1000);
  bounded.sampleCount = clampAudioInt(frame.sampleCount, 0, bounded.sampleRate * bounded.channels);
  bounded.rmsLevel = clampAudioDouble(frame.rmsLevel, 0.0, 1.0);
  bounded.peakLevel = clampAudioDouble(frame.peakLevel, 0.0, 1.0);
  bounded.noiseFloorDb = frame.noiseFloorDb < 0.0 ? clampAudioDouble(frame.noiseFloorDb, -96.0, -18.0) : -60.0;
  bounded.voiceActive = frame.voiceActive;
  bounded.invalidShape = frame.sampleRate != bounded.sampleRate || frame.channels != bounded.channels || frame.sampleCount != bounded.sampleCount || frame.timestampMs != bounded.timestampMs;
  return bounded;
}

inline std::uint32_t audioDspHash(const AudioFrame& frame) {
  std::uint32_t hash = 2166136261u;
  const auto mixByte = [&](std::uint8_t value) {
    hash ^= value;
    hash *= 16777619u;
  };
  for (const char value : frame.participantId) {
    mixByte(static_cast<std::uint8_t>(value));
  }
  const auto mixNumber = [&](std::int64_t value) {
    for (int index = 0; index < 8; ++index) {
      mixByte(static_cast<std::uint8_t>((value >> (index * 8)) & 0xff));
    }
  };
  mixNumber(frame.timestampMs);
  mixNumber(frame.sampleRate);
  mixNumber(frame.channels);
  mixNumber(frame.sampleCount);
  return hash;
}

inline double deterministicRmsLevel(const AudioFrame& frame, std::uint32_t hash) {
  if (frame.rmsLevel > 0.0) {
    return clampAudioDouble(frame.rmsLevel, 0.0, 1.0);
  }
  const int sampleRateBias = frame.sampleRate >= 48000 ? 5 : 0;
  const int channelBias = frame.channels > 1 ? 4 : 0;
  const int bucket = static_cast<int>(hash % 58u);
  return clampAudioDouble((18 + bucket + sampleRateBias + channelBias) / 100.0, 0.0, 0.96);
}

inline AudioParticipantMixMetrics analyzeAudioParticipantFrame(const AudioFrame& frame, const AudioDspTimingReference& timing = {}) {
  const BoundedAudioFrame bounded = boundAudioFrame(frame);
  const std::uint32_t hash = audioDspHash(frame);
  const double rmsLevel = frame.rmsLevel > 0.0 ? bounded.rmsLevel : deterministicRmsLevel(frame, hash);
  const double peakLevel = frame.peakLevel > 0.0 ? clampAudioDouble(bounded.peakLevel, rmsLevel, 1.0) : clampAudioDouble(rmsLevel + 0.12 + ((hash >> 8) % 18u) / 100.0, 0.0, 1.0);
  const double noiseFloorDb = frame.noiseFloorDb < 0.0 ? bounded.noiseFloorDb : -72.0 + static_cast<double>((hash >> 16) % 22u);
  const int inputLevel = frame.voiceActive ? clampAudioInt(static_cast<int>(std::lround(rmsLevel * 100.0)), 0, 100) : 0;
  const int nominalSamplesPerPacket = std::max(1, bounded.sampleRate / 50);
  const bool silenceDetected = !bounded.voiceActive || (frame.rmsLevel > 0.0 && rmsLevel <= 0.005) || (frame.peakLevel > 0.0 && peakLevel <= 0.01);
  const bool clippingDetected = frame.peakLevel > 1.0 || peakLevel >= 0.98 || frame.rmsLevel > 1.0;
  int64_t timingDriftMs = 0;
  if (timing.hasPreviousTimestamp) {
    timingDriftMs = clampAudioInt64(bounded.timestampMs - (timing.previousTimestampMs + 20), -500, 500);
  }
  const int64_t avSyncOffsetMs = timing.hasMixReferenceTimestamp ? clampAudioInt64(bounded.timestampMs - timing.mixReferenceTimestampMs, -500, 500) : 0;
  const bool underrunDetected = bounded.invalidShape || bounded.sampleCount < nominalSamplesPerPacket / 2 || timingDriftMs > 40;

  int gainDb = 0;
  const int targetDelta = 68 - inputLevel;
  if (!frame.voiceActive) {
    gainDb = -60;
  } else if (targetDelta > 28) {
    gainDb = 6;
  } else if (targetDelta > 14) {
    gainDb = 3;
  } else if (targetDelta < -12) {
    gainDb = -4;
  } else if (targetDelta < -4) {
    gainDb = -2;
  }

  const bool noiseSuppression = frame.voiceActive && (noiseFloorDb > -48.0 || inputLevel < 32);
  int outputLevel = frame.voiceActive ? clampAudioInt(inputLevel + gainDb * 4 - (noiseSuppression ? 2 : 0), 0, 100) : 0;
  const bool limiterActive = frame.voiceActive && (peakLevel >= 0.92 || outputLevel >= 88);
  if (limiterActive) {
    outputLevel = std::min(outputLevel, 88);
  }

  AudioParticipantMixMetrics metrics;
  metrics.participantId = frame.participantId.empty() ? "unknown-audio-source" : frame.participantId;
  metrics.inputLevel = inputLevel;
  metrics.outputLevel = outputLevel;
  metrics.gainDb = gainDb;
  metrics.rmsLevel = rmsLevel;
  metrics.peakLevel = peakLevel;
  metrics.noiseFloorDb = noiseFloorDb;
  metrics.noiseSuppressionActive = noiseSuppression;
  metrics.limiterActive = limiterActive;
  metrics.underrunDetected = underrunDetected;
  metrics.clippingDetected = clippingDetected;
  metrics.silenceDetected = silenceDetected;
  metrics.muted = !frame.voiceActive;
  metrics.avSyncOffsetMs = avSyncOffsetMs;
  metrics.timingDriftMs = timingDriftMs;
  metrics.framesMixed = 1;
  if (metrics.muted) {
    metrics.status = "muted";
  } else if (limiterActive) {
    metrics.status = "limited";
  } else if (noiseSuppression) {
    metrics.status = "cleaning";
  } else if (gainDb > 0) {
    metrics.status = "boosting";
  } else if (gainDb < 0) {
    metrics.status = "ducking";
  } else {
    metrics.status = "balanced";
  }
  return metrics;
}

inline AudioMixMetrics summarizeAudioMixMetrics(std::vector<AudioParticipantMixMetrics> participants, int64_t mixedFrameCount) {
  AudioMixMetrics session;
  session.mixedFrameCount = mixedFrameCount;
  session.participantCount = static_cast<int>(participants.size());
  session.participants = std::move(participants);
  if (session.participants.empty()) {
    return session;
  }

  int audibleCount = 0;
  int outputTotal = 0;
  int boostedCount = 0;
  int duckedCount = 0;
  int cleanedCount = 0;
  int limitedCount = 0;
  int mutedCount = 0;
  for (const auto& participant : session.participants) {
    session.limiterActive = session.limiterActive || participant.limiterActive;
    if (participant.underrunDetected) ++session.underrunCount;
    if (participant.clippingDetected) ++session.clippingCount;
    if (participant.silenceDetected) ++session.silenceCount;
    session.maxAbsAvSyncOffsetMs =
        std::max<std::int64_t>(session.maxAbsAvSyncOffsetMs, std::llabs(participant.avSyncOffsetMs));
    if (participant.muted) {
      ++mutedCount;
      continue;
    }
    outputTotal += participant.outputLevel;
    ++audibleCount;
    if (participant.gainDb > 0) ++boostedCount;
    if (participant.gainDb < 0) ++duckedCount;
    if (participant.noiseSuppressionActive) ++cleanedCount;
    if (participant.limiterActive) ++limitedCount;
  }

  session.masterLevel = audibleCount > 0 ? clampAudioInt((outputTotal / audibleCount) + 8, 0, 100) : 0;
  session.limiterActive = session.limiterActive || session.masterLevel >= 88;
  session.loudnessLufs = audibleCount > 0 ? clampAudioDouble(-24.0 + session.masterLevel * 0.12, -60.0, -12.0) : -60.0;

  if (cleanedCount > 0) {
    session.warnings.emplace_back("Noise suppression active on one or more participant sources.");
  }
  if (limitedCount > 0) {
    session.warnings.emplace_back("Limiter engaged on one or more participant sources.");
  }
  if (session.underrunCount > 0) {
    session.warnings.emplace_back("Audio packet underrun or timing gap detected in native DSP mix.");
  }
  if (session.clippingCount > 0) {
    session.warnings.emplace_back("Audio clipping detected before native DSP limiting.");
  }
  if (session.silenceCount == session.participantCount && session.participantCount > 0) {
    session.warnings.emplace_back("All participant audio sources are silent.");
  }

  std::ostringstream summary;
  if (boostedCount > 0) summary << boostedCount << " boosted";
  if (duckedCount > 0) summary << (summary.tellp() > 0 ? ", " : "") << duckedCount << " ducked";
  if (cleanedCount > 0) summary << (summary.tellp() > 0 ? ", " : "") << cleanedCount << " cleaned";
  if (limitedCount > 0) summary << (summary.tellp() > 0 ? ", " : "") << limitedCount << " limited";
  if (mutedCount > 0) summary << (summary.tellp() > 0 ? ", " : "") << mutedCount << " muted";
  session.summary = summary.tellp() > 0 ? summary.str() + " in native DSP mix" : "Native DSP mix balanced";
  session.status = session.warnings.empty() ? "live" : "warning";
  return session;
}

}  // namespace corevideo::modules
