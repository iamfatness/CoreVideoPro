#include "StudioState.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <limits>
#include <sstream>
#include <string_view>

namespace corevideo::studio {
namespace {

bool isSpace(char ch) {
  return std::isspace(static_cast<unsigned char>(ch)) != 0;
}

void skipSpace(const std::string& text, std::size_t& pos) {
  while (pos < text.size() && isSpace(text[pos])) {
    ++pos;
  }
}

std::string trim(const std::string& value) {
  std::size_t begin = 0;
  while (begin < value.size() && isSpace(value[begin])) {
    ++begin;
  }
  std::size_t end = value.size();
  while (end > begin && isSpace(value[end - 1])) {
    --end;
  }
  return value.substr(begin, end - begin);
}

bool consumeLiteral(const std::string& text, std::size_t& pos, const char* literal) {
  const std::string_view needle(literal);
  if (pos + needle.size() > text.size()) {
    return false;
  }
  if (text.compare(pos, needle.size(), needle) != 0) {
    return false;
  }
  pos += needle.size();
  return true;
}

std::optional<std::string> readJsonStringAt(const std::string& text, std::size_t& pos) {
  skipSpace(text, pos);
  if (pos >= text.size() || text[pos] != '"') {
    return std::nullopt;
  }
  ++pos;

  std::string result;
  while (pos < text.size()) {
    const char ch = text[pos++];
    if (ch == '"') {
      return result;
    }
    if (ch != '\\') {
      result.push_back(ch);
      continue;
    }
    if (pos >= text.size()) {
      return std::nullopt;
    }
    const char escaped = text[pos++];
    switch (escaped) {
      case '"': result.push_back('"'); break;
      case '\\': result.push_back('\\'); break;
      case '/': result.push_back('/'); break;
      case 'b': result.push_back('\b'); break;
      case 'f': result.push_back('\f'); break;
      case 'n': result.push_back('\n'); break;
      case 'r': result.push_back('\r'); break;
      case 't': result.push_back('\t'); break;
      case 'u':
        if (pos + 4 > text.size()) {
          return std::nullopt;
        }
        pos += 4;
        result.push_back('?');
        break;
      default:
        result.push_back(escaped);
        break;
    }
  }
  return std::nullopt;
}

std::optional<double> readJsonNumberAt(const std::string& text, std::size_t& pos) {
  skipSpace(text, pos);
  const std::size_t begin = pos;
  if (pos < text.size() && text[pos] == '-') {
    ++pos;
  }
  while (pos < text.size() && std::isdigit(static_cast<unsigned char>(text[pos]))) {
    ++pos;
  }
  if (pos < text.size() && text[pos] == '.') {
    ++pos;
    while (pos < text.size() && std::isdigit(static_cast<unsigned char>(text[pos]))) {
      ++pos;
    }
  }
  if (pos < text.size() && (text[pos] == 'e' || text[pos] == 'E')) {
    ++pos;
    if (pos < text.size() && (text[pos] == '+' || text[pos] == '-')) {
      ++pos;
    }
    while (pos < text.size() && std::isdigit(static_cast<unsigned char>(text[pos]))) {
      ++pos;
    }
  }
  if (begin == pos || (begin + 1 == pos && text[begin] == '-')) {
    return std::nullopt;
  }

  const std::string token = text.substr(begin, pos - begin);
  char* end = nullptr;
  const double value = std::strtod(token.c_str(), &end);
  if (end == token.c_str()) {
    return std::nullopt;
  }
  return value;
}

std::optional<bool> readJsonBoolAt(const std::string& text, std::size_t& pos) {
  skipSpace(text, pos);
  if (consumeLiteral(text, pos, "true")) {
    return true;
  }
  if (consumeLiteral(text, pos, "false")) {
    return false;
  }
  return std::nullopt;
}

std::size_t skipJsonValue(const std::string& text, std::size_t pos);

std::size_t skipJsonString(const std::string& text, std::size_t pos) {
  if (pos >= text.size() || text[pos] != '"') {
    return std::string::npos;
  }
  ++pos;
  while (pos < text.size()) {
    const char ch = text[pos++];
    if (ch == '"') {
      return pos;
    }
    if (ch == '\\') {
      if (pos >= text.size()) {
        return std::string::npos;
      }
      if (text[pos] == 'u') {
        pos += 5;
      } else {
        ++pos;
      }
    }
  }
  return std::string::npos;
}

std::size_t skipJsonBracketed(const std::string& text, std::size_t pos, char open, char close) {
  if (pos >= text.size() || text[pos] != open) {
    return std::string::npos;
  }

  int depth = 0;
  while (pos < text.size()) {
    const char ch = text[pos];
    if (ch == '"') {
      pos = skipJsonString(text, pos);
      if (pos == std::string::npos) {
        return std::string::npos;
      }
      continue;
    }
    if (ch == open) {
      ++depth;
    } else if (ch == close) {
      --depth;
      if (depth == 0) {
        return pos + 1;
      }
    }
    ++pos;
  }
  return std::string::npos;
}

std::size_t skipJsonValue(const std::string& text, std::size_t pos) {
  skipSpace(text, pos);
  if (pos >= text.size()) {
    return std::string::npos;
  }
  if (text[pos] == '"') {
    return skipJsonString(text, pos);
  }
  if (text[pos] == '{') {
    return skipJsonBracketed(text, pos, '{', '}');
  }
  if (text[pos] == '[') {
    return skipJsonBracketed(text, pos, '[', ']');
  }
  while (pos < text.size() && text[pos] != ',' && text[pos] != '}' && text[pos] != ']') {
    ++pos;
  }
  return pos;
}

std::optional<std::size_t> findJsonValue(const std::string& json, const std::string& key) {
  std::size_t pos = 0;
  while (pos < json.size()) {
    if (json[pos] != '"') {
      ++pos;
      continue;
    }

    std::size_t keyPos = pos;
    auto parsedKey = readJsonStringAt(json, keyPos);
    if (!parsedKey) {
      return std::nullopt;
    }
    pos = keyPos;
    skipSpace(json, keyPos);
    if (keyPos >= json.size() || json[keyPos] != ':') {
      continue;
    }
    ++keyPos;
    skipSpace(json, keyPos);
    if (*parsedKey == key) {
      return keyPos;
    }

    const std::size_t valueEnd = skipJsonValue(json, keyPos);
    if (valueEnd == std::string::npos) {
      return std::nullopt;
    }
    pos = valueEnd;
  }
  return std::nullopt;
}

std::optional<std::string> extractJsonBracketed(const std::string& json, const std::string& key, char open, char close) {
  auto value = findJsonValue(json, key);
  if (!value || *value >= json.size() || json[*value] != open) {
    return std::nullopt;
  }
  const std::size_t end = skipJsonBracketed(json, *value, open, close);
  if (end == std::string::npos) {
    return std::nullopt;
  }
  return json.substr(*value, end - *value);
}

void assignIfPresent(std::string& target, const std::optional<std::string>& value) {
  if (value && !value->empty()) {
    target = *value;
  }
}

void assignCountIfPresent(std::uint64_t& target, const std::optional<double>& value) {
  if (value && std::isfinite(*value) && *value >= 0) {
    target = static_cast<std::uint64_t>(*value);
  }
}

void assignIntIfPresent(int& target, const std::optional<double>& value) {
  if (value && std::isfinite(*value)) {
    const double bounded = std::clamp(*value, static_cast<double>(std::numeric_limits<int>::min()), static_cast<double>(std::numeric_limits<int>::max()));
    target = static_cast<int>(bounded);
  }
}

std::optional<std::string> firstObject(const std::string& line, const std::initializer_list<const char*> keys) {
  for (const char* key : keys) {
    if (auto object = extractJsonObject(line, key)) {
      return object;
    }
  }
  return std::nullopt;
}

std::optional<std::string> firstString(const std::string& line, const std::initializer_list<const char*> keys) {
  for (const char* key : keys) {
    if (auto value = extractJsonString(line, key); value && !value->empty()) {
      return value;
    }
  }
  return std::nullopt;
}

std::optional<double> firstNumber(const std::string& line, const std::initializer_list<const char*> keys) {
  for (const char* key : keys) {
    if (auto value = extractJsonNumber(line, key)) {
      return value;
    }
  }
  return std::nullopt;
}

void applyProfile(StudioState& state, const std::string& profile) {
  assignIfPresent(state.renderer, extractJsonString(profile, "renderer"));
  assignIfPresent(state.encoder, extractJsonString(profile, "encoder"));

  auto capabilities = extractJsonStringArray(profile, "capabilities");
  if (!capabilities.empty()) {
    state.capabilities = std::move(capabilities);
    state.capabilityCount = state.capabilities.size();
  }
}

void applyRecordingHealth(StudioState& state) {
  if (state.recordingArtifactPath.empty() && state.recordingFrameCount == 0 && !state.recordingMetadataValid) {
    state.recordingHealth.clear();
    return;
  }

  std::ostringstream out;
  if (state.recordingMetadataValid) {
    out << "ok";
  } else if (!state.recordingArtifactPath.empty() || state.recordingFrameCount > 0) {
    out << "pending";
  } else {
    out << "idle";
  }
  if (state.recordingFrameCount > 0) {
    out << ", " << state.recordingFrameCount << " frames";
  }
  if (state.recordingDurationMs > 0) {
    out << ", " << (state.recordingDurationMs / 1000) << "s";
  }
  if (state.recordingBytesWritten > 0) {
    out << ", " << (state.recordingBytesWritten / 1024) << " KB";
  }
  state.recordingHealth = out.str();
}

void markFirstFrameIfNeeded(StudioState& state) {
  if (!state.firstFrameSeen && (state.frameCount > 0 || state.previewFrameNumber > 0)) {
    state.firstFrameSeen = true;
  }
}

void applyHealth(StudioState& state, const std::string& health) {
  assignIfPresent(state.healthStatus, extractJsonString(health, "status"));
  assignIfPresent(state.renderer, extractJsonString(health, "renderer"));
  assignIfPresent(state.encoder, extractJsonString(health, "encoder"));
  assignIfPresent(state.programFrameHealth, extractJsonString(health, "programFrameHealth"));
  assignIfPresent(state.recordingArtifactPath, extractJsonString(health, "recordingArtifactPath"));
  assignCountIfPresent(state.frameCount, extractJsonNumber(health, "frameCount"));
  assignCountIfPresent(state.recordingBytesWritten, extractJsonNumber(health, "recordingBytesWritten"));
  assignCountIfPresent(state.recordingDurationMs, extractJsonNumber(health, "recordingDurationMs"));
  assignCountIfPresent(state.recordingFrameCount, extractJsonNumber(health, "recordingFrameCount"));
  if (auto metadataValid = extractJsonBool(health, "recordingMetadataValid")) {
    state.recordingMetadataValid = *metadataValid;
  }
  applyRecordingHealth(state);
  markFirstFrameIfNeeded(state);
}

void applyProgramFrame(StudioState& state, const std::string& programFrame) {
  assignIfPresent(state.programFrameHealth, extractJsonString(programFrame, "health"));
  assignIfPresent(state.renderer, extractJsonString(programFrame, "renderer"));
  assignCountIfPresent(state.frameCount, firstNumber(programFrame, {"frameNumber", "programFrameCount", "frameCount"}));
  markFirstFrameIfNeeded(state);
}

void applyEncoderSession(StudioState& state, const std::string& encoderSession) {
  assignIfPresent(state.outputStatus, extractJsonString(encoderSession, "status"));
  assignIfPresent(state.recordingArtifactPath, extractJsonString(encoderSession, "recordingArtifactPath"));
  assignCountIfPresent(state.frameCount, extractJsonNumber(encoderSession, "programFrameCount"));
  assignCountIfPresent(state.recordingBytesWritten, extractJsonNumber(encoderSession, "recordingBytesWritten"));
  assignCountIfPresent(state.recordingDurationMs, extractJsonNumber(encoderSession, "recordingDurationMs"));
  assignCountIfPresent(state.recordingFrameCount, extractJsonNumber(encoderSession, "recordingFrameCount"));
  if (auto metadataValid = extractJsonBool(encoderSession, "recordingMetadataValid")) {
    state.recordingMetadataValid = *metadataValid;
  }
  if (auto lifecycle = extractJsonObject(encoderSession, "lifecycle")) {
    assignIfPresent(state.encoderLifecycleStatus, extractJsonString(*lifecycle, "status"));
    if (auto transition = extractJsonString(*lifecycle, "lastTransition"); transition && !transition->empty()) {
      state.lastSummaryText = *transition;
    }
  }
  applyRecordingHealth(state);
  markFirstFrameIfNeeded(state);
}

void applyOutputSenderSession(StudioState& state, const std::string& outputSenderSession) {
  assignIfPresent(state.outputStatus, extractJsonString(outputSenderSession, "status"));
  assignIntIfPresent(state.activeSenderCount, extractJsonNumber(outputSenderSession, "activeSenderCount"));
}

void applyRecording(StudioState& state, const std::string& recording) {
  assignIfPresent(state.recordingArtifactPath, firstString(recording, {"artifactPath", "programPath", "path"}));
  assignIfPresent(state.lastErrorText, firstString(recording, {"error", "lastFailure"}));
  assignIfPresent(state.lastSummaryText, firstString(recording, {"warning", "lastRecovery"}));
  assignCountIfPresent(state.recordingFrameCount, firstNumber(recording, {"programFramesWritten", "recordingFrameCount", "frameCount"}));
  assignCountIfPresent(state.recordingDurationMs, extractJsonNumber(recording, "durationMs"));
  if (auto metadataValid = extractJsonBool(recording, "metadataValid")) {
    state.recordingMetadataValid = *metadataValid;
  }
  applyRecordingHealth(state);
}

void applySceneOutput(StudioState& state, const std::string& snapshot) {
  assignIfPresent(state.sceneId, extractJsonString(snapshot, "sceneId"));
  if (auto routeCount = extractJsonNumber(snapshot, "routeCount")) {
    assignIntIfPresent(state.routeCount, routeCount);
  }
  if (auto overlayCount = extractJsonNumber(snapshot, "overlayCount")) {
    assignIntIfPresent(state.overlayCount, overlayCount);
  }
  auto outputs = extractJsonStringArray(snapshot, "outputs");
  if (!outputs.empty()) {
    state.outputDestinations = std::move(outputs);
  }
  if (auto captionTrack = extractJsonObject(snapshot, "captionTrack")) {
    if (auto enabled = extractJsonBool(*captionTrack, "enabled")) {
      state.captionsEnabled = *enabled;
    }
    assignIfPresent(state.captionStatus, extractJsonString(*captionTrack, "status"));
  }
}

void applySnapshotLikeObject(StudioState& state, const std::string& snapshot) {
  applySceneOutput(state, snapshot);
  assignIfPresent(state.encoder, extractJsonString(snapshot, "encoder"));
  assignIfPresent(state.renderer, firstString(snapshot, {"compositorRenderer", "renderer"}));
  assignIfPresent(state.healthStatus, extractJsonString(snapshot, "status"));
  assignCountIfPresent(state.frameCount, firstNumber(snapshot, {"programFrameCount", "frameCount"}));

  if (auto profile = extractJsonObject(snapshot, "profile")) {
    applyProfile(state, *profile);
  }
  if (auto health = extractJsonObject(snapshot, "health")) {
    applyHealth(state, *health);
  }
  if (auto programFrame = extractJsonObject(snapshot, "programFrame")) {
    applyProgramFrame(state, *programFrame);
  }
  if (auto encoderSession = extractJsonObject(snapshot, "encoderSession")) {
    applyEncoderSession(state, *encoderSession);
  }
  if (auto outputSenderSession = extractJsonObject(snapshot, "outputSenderSession")) {
    applyOutputSenderSession(state, *outputSenderSession);
  }
  if (auto recording = extractJsonObject(snapshot, "recording")) {
    applyRecording(state, *recording);
  }
  markFirstFrameIfNeeded(state);
}

void applyError(StudioState& state, const std::string& line) {
  if (auto error = extractJsonObject(line, "error")) {
    if (auto message = extractJsonString(*error, "message"); message && !message->empty()) {
      state.lastErrorText = *message;
      return;
    }
    if (auto code = extractJsonString(*error, "code"); code && !code->empty()) {
      state.lastErrorText = *code;
      return;
    }
  }
  assignIfPresent(state.lastErrorText, firstString(line, {"lastError", "error"}));
}

}  // namespace

std::optional<std::string> extractJsonString(const std::string& json, const std::string& key) {
  auto value = findJsonValue(json, key);
  if (!value) {
    return std::nullopt;
  }
  std::size_t pos = *value;
  if (auto parsed = readJsonStringAt(json, pos)) {
    return parsed;
  }

  auto number = readJsonNumberAt(json, pos);
  if (number) {
    std::ostringstream out;
    out << *number;
    return out.str();
  }

  pos = *value;
  if (auto boolean = readJsonBoolAt(json, pos)) {
    return *boolean ? "true" : "false";
  }
  return std::nullopt;
}

std::optional<double> extractJsonNumber(const std::string& json, const std::string& key) {
  auto value = findJsonValue(json, key);
  if (!value) {
    return std::nullopt;
  }

  std::size_t pos = *value;
  if (auto number = readJsonNumberAt(json, pos)) {
    return number;
  }

  pos = *value;
  auto stringValue = readJsonStringAt(json, pos);
  if (!stringValue) {
    return std::nullopt;
  }
  const std::string token = trim(*stringValue);
  if (token.empty()) {
    return std::nullopt;
  }
  char* end = nullptr;
  const double number = std::strtod(token.c_str(), &end);
  if (end == token.c_str()) {
    return std::nullopt;
  }
  return number;
}

std::optional<bool> extractJsonBool(const std::string& json, const std::string& key) {
  auto value = findJsonValue(json, key);
  if (!value) {
    return std::nullopt;
  }

  std::size_t pos = *value;
  if (auto boolean = readJsonBoolAt(json, pos)) {
    return boolean;
  }

  pos = *value;
  if (auto text = readJsonStringAt(json, pos)) {
    if (*text == "true" || *text == "1") {
      return true;
    }
    if (*text == "false" || *text == "0") {
      return false;
    }
  }
  return std::nullopt;
}

std::vector<std::string> extractJsonStringArray(const std::string& json, const std::string& key) {
  std::vector<std::string> result;
  auto array = extractJsonArray(json, key);
  if (!array) {
    return result;
  }

  std::size_t pos = 1;
  while (pos < array->size()) {
    skipSpace(*array, pos);
    if (pos >= array->size() || (*array)[pos] == ']') {
      break;
    }
    if (auto value = readJsonStringAt(*array, pos)) {
      result.push_back(*value);
    } else {
      const std::size_t valueEnd = skipJsonValue(*array, pos);
      if (valueEnd == std::string::npos || valueEnd == pos) {
        break;
      }
      pos = valueEnd;
    }
    skipSpace(*array, pos);
    if (pos < array->size() && (*array)[pos] == ',') {
      ++pos;
    }
  }
  return result;
}

std::vector<std::string> extractJsonObjectArray(const std::string& json, const std::string& key) {
  std::vector<std::string> result;
  auto array = extractJsonArray(json, key);
  if (!array) {
    return result;
  }

  std::size_t pos = 1;
  while (pos < array->size()) {
    skipSpace(*array, pos);
    if (pos >= array->size() || (*array)[pos] == ']') {
      break;
    }
    if ((*array)[pos] == '{') {
      const std::size_t end = skipJsonBracketed(*array, pos, '{', '}');
      if (end == std::string::npos) {
        break;
      }
      result.push_back(array->substr(pos, end - pos));
      pos = end;
    } else {
      const std::size_t valueEnd = skipJsonValue(*array, pos);
      if (valueEnd == std::string::npos || valueEnd == pos) {
        break;
      }
      pos = valueEnd;
    }
    skipSpace(*array, pos);
    if (pos < array->size() && (*array)[pos] == ',') {
      ++pos;
    }
  }
  return result;
}

std::optional<std::string> extractJsonObject(const std::string& json, const std::string& key) {
  return extractJsonBracketed(json, key, '{', '}');
}

std::optional<std::string> extractJsonArray(const std::string& json, const std::string& key) {
  return extractJsonBracketed(json, key, '[', ']');
}

StudioState parseStudioStateLine(const std::string& line) {
  StudioState state;
  applyStudioStateLine(state, line);
  return state;
}

void applyStudioStateLine(StudioState& state, const std::string& line) {
  const std::string json = trim(line);
  if (json.empty() || json.front() != '{') {
    return;
  }

  state.coreRunning = true;
  state.connected = true;

  const auto ok = extractJsonBool(json, "ok");
  if (ok && !*ok) {
    applyError(state, json);
  }

  const auto type = extractJsonString(json, "type");
  const auto id = extractJsonString(json, "id");
  if ((type && *type == "handshake") || (id && *id == "handshake")) {
    state.handshakeSeen = true;
  }

  if (auto profile = extractJsonObject(json, "profile")) {
    if (state.handshakeSeen || (type && *type == "handshake")) {
      state.handshakeSeen = true;
    }
    applyProfile(state, *profile);
  }

  if (auto health = extractJsonObject(json, "health")) {
    applyHealth(state, *health);
  }

  if (auto snapshot = firstObject(json, {"snapshot", "session"})) {
    applySnapshotLikeObject(state, *snapshot);
  }

  const auto snapshotObject = firstObject(json, {"snapshot", "session"});
  const std::string& participantSource = snapshotObject ? *snapshotObject : json;
  auto participants = extractJsonObjectArray(participantSource, "participants");
  if (!participants.empty()) {
    state.participantIds.clear();
    state.participantLines.clear();
    for (const auto& participant : participants) {
      const auto participantId = firstString(participant, {"userId", "sdkUserId", "participantId"}).value_or("");
      const auto name = extractJsonString(participant, "displayName").value_or(participantId.empty() ? "Participant" : participantId);
      const auto role = extractJsonString(participant, "role").value_or("Guest");
      const auto muted = extractJsonBool(participant, "muted").value_or(false);
      const auto videoOn = extractJsonBool(participant, "videoOn").value_or(true);
      const auto talking = extractJsonBool(participant, "talking").value_or(false);
      const auto level = extractJsonNumber(participant, "audioLevel").value_or(0);
      if (!participantId.empty()) {
        state.participantIds.push_back(participantId);
      }
      std::ostringstream lineOut;
      lineOut << name << "  [" << role << "]  "
              << (videoOn ? "video" : "video off") << "  "
              << (muted ? "muted" : "audio " + std::to_string(static_cast<int>(level)));
      if (talking) {
        lineOut << "  speaking";
      }
      state.participantLines.push_back(lineOut.str());
    }
  }

  if (auto programFrame = extractJsonObject(json, "programFrame")) {
    applyProgramFrame(state, *programFrame);
  }

  if (auto outputSenderSession = extractJsonObject(json, "outputSenderSession")) {
    applyOutputSenderSession(state, *outputSenderSession);
  }

  if (auto status = firstString(json, {"status", "healthStatus"}); status && state.healthStatus.empty()) {
    state.healthStatus = *status;
  }
  assignIfPresent(state.meetingState, firstString(participantSource, {"meetingState"}));
  assignIfPresent(state.activeSpeakerId, firstString(participantSource, {"activeSpeakerId"}));
  assignIfPresent(state.renderer, extractJsonString(json, "renderer"));
  assignIfPresent(state.encoder, extractJsonString(json, "encoder"));
  assignIfPresent(state.programFrameHealth, extractJsonString(json, "programFrameHealth"));
  assignIfPresent(state.recordingArtifactPath, extractJsonString(json, "recordingArtifactPath"));
  assignCountIfPresent(state.frameCount, firstNumber(json, {"frameCount", "programFrameCount", "frameNumber"}));
  assignIntIfPresent(state.activeSenderCount, extractJsonNumber(json, "activeSenderCount"));

  if (auto summary = firstString(json, {"summary", "lastTransition", "message"}); summary && !summary->empty()) {
    state.lastSummaryText = *summary;
  }

  if (!snapshotObject) {
    applySceneOutput(state, json);
  }
  applyRecordingHealth(state);
  markFirstFrameIfNeeded(state);
  applyError(state, json);
}

void notePreviewFrame(StudioState& state, std::uint64_t frameNumber, const std::string& health) {
  state.previewFrameNumber = frameNumber;
  if (!health.empty()) {
    state.programFrameHealth = health;
  }
  if (frameNumber > state.frameCount) {
    state.frameCount = frameNumber;
  }
  markFirstFrameIfNeeded(state);
}

std::string summarizeStudioState(const StudioState& state) {
  if (!state.lastErrorText.empty()) {
    return "Error: " + state.lastErrorText;
  }

  std::ostringstream out;
  out << (state.connected ? "Connected" : "Disconnected");
  if (!state.meetingState.empty()) {
    out << ", Zoom " << state.meetingState;
  }
  if (state.handshakeSeen) {
    out << ", handshake";
  }
  if (!state.healthStatus.empty()) {
    out << ", health " << state.healthStatus;
  }
  if (!state.outputStatus.empty()) {
    out << ", output " << state.outputStatus;
  }
  if (!state.renderer.empty()) {
    out << ", renderer " << state.renderer;
  }
  if (!state.encoder.empty()) {
    out << ", encoder " << state.encoder;
  }
  if (state.firstFrameSeen) {
    out << ", first frame";
    if (state.frameCount > 0) {
      out << " #" << state.frameCount;
    }
  } else if (state.frameCount > 0) {
    out << ", frames " << state.frameCount;
  }
  if (!state.programFrameHealth.empty()) {
    out << ", program " << state.programFrameHealth;
  }
  if (!state.recordingHealth.empty()) {
    out << ", recording " << state.recordingHealth;
  }
  if (state.activeSenderCount > 0) {
    out << ", senders " << state.activeSenderCount;
  }
  if (!state.sceneId.empty() && state.sceneId != "unloaded") {
    out << ", scene " << state.sceneId;
  }
  if (!state.lastSummaryText.empty()) {
    out << ". " << state.lastSummaryText;
  }
  return out.str();
}

}  // namespace corevideo::studio
