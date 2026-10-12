#pragma once
#include "core/AudioOutputObservation.h"

namespace corevideo::core {

// Conservative retained payload accounting, including container spare capacity.
// Static reason strings are shared constants, not owned observation payloads.
inline std::size_t observationExtraBytes(const std::string& value) { return value.capacity() + 1 + 32; }
inline std::size_t observationExtraBytes(const contracts::OutputLifecycle& value) {
  return observationExtraBytes(value.sessionId) + observationExtraBytes(value.state) +
      observationExtraBytes(value.health) + (value.error ? observationExtraBytes(*value.error) : 0);
}
inline std::size_t observationExtraBytes(const modules::IsoStreamStatus& value) {
  return observationExtraBytes(value.sourceId) + observationExtraBytes(value.displayName) +
      observationExtraBytes(value.path) + observationExtraBytes(value.kind) +
      observationExtraBytes(value.encoderPath) + observationExtraBytes(value.fallbackReason) +
      observationExtraBytes(value.warning);
}
inline std::size_t observationExtraBytes(const modules::AudioParticipantMixMetrics& value) {
  return observationExtraBytes(value.participantId) + observationExtraBytes(value.status);
}
inline std::size_t observationExtraBytes(const modules::OutputSender& value) {
  std::size_t bytes = 0;
  for (const auto* field : {&value.senderId, &value.destination, &value.status, &value.warning,
      &value.destinationHealth, &value.lastResultCode, &value.lastError,
      &value.sendArtifactPath, &value.runtimeDetail}) bytes += observationExtraBytes(*field);
  if (value.lifecycle) bytes += observationExtraBytes(*value.lifecycle);
  if (value.supervisor) bytes += observationExtraBytes(value.supervisor->failureClass) + observationExtraBytes(value.supervisor->reason);
  if (value.asyncWorker) bytes += observationExtraBytes(value.asyncWorker->operation) + observationExtraBytes(value.asyncWorker->stage);
  if (value.streamClock) bytes += observationExtraBytes(value.streamClock->muxInput);
  return bytes;
}
template <typename T>
inline std::size_t observationExtraBytes(const std::vector<T>& values) {
  std::size_t bytes = values.capacity() * sizeof(T) + 32;
  for (const auto& value : values) bytes += observationExtraBytes(value);
  return bytes;
}
inline std::size_t audioObservationBytes(const AudioOutputObservation& value) {
  std::size_t bytes = sizeof(value);
  const auto& e = value.encoder;
  for (const auto* field : {&e.recordingSessionDir, &e.recordingManifestPath, &e.encoderName,
      &e.codec, &e.recordingSessionId, &e.recordingStatus, &e.recordingTargetFolder,
      &e.recordingFilenamePrefix, &e.recordingFormat, &e.recordingQuality, &e.recordingArtifactPath,
      &e.recordingContainerFormat, &e.recordingVideoCodec, &e.recordingAudioCodec,
      &e.recordingWarning, &e.recordingError}) bytes += observationExtraBytes(*field);
  bytes += observationExtraBytes(e.destinations) + observationExtraBytes(e.isoParticipantIds) + observationExtraBytes(e.isoStreams);
  if (e.lifecycle) bytes += observationExtraBytes(*e.lifecycle);
  bytes += observationExtraBytes(value.sender.status) + observationExtraBytes(value.sender.senders) + observationExtraBytes(value.sender.warnings);
  bytes += observationExtraBytes(value.mixer.status) + observationExtraBytes(value.mixer.participants) +
      observationExtraBytes(value.mixer.warnings) + observationExtraBytes(value.mixer.summary);
  return bytes;
}

}  // namespace corevideo::core
