#include "ProgramPreview.h"
#include "StudioState.h"

#include <windows.h>

#include <array>
#include <atomic>
#include <filesystem>
#include <fstream>
#include <optional>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

namespace {

constexpr int kButtonStartCore = 1001;
constexpr int kButtonJoinZoom = 1002;
constexpr int kButtonLeaveZoom = 1003;
constexpr int kButtonHealth = 1004;
constexpr int kButtonSnapshot = 1005;
constexpr int kButtonClear = 1006;
constexpr int kButtonMagicScene = 1007;
constexpr int kButtonRecord = 1008;
constexpr int kLogEdit = 2001;
constexpr int kStatusLabel = 2002;
constexpr int kScenesPanel = 2003;
constexpr int kProgramPanel = 2004;
constexpr int kParticipantsPanel = 2005;
constexpr int kHealthPanel = 2006;
constexpr int kMeetingUrlEdit = 2007;
constexpr int kDisplayNameEdit = 2008;
constexpr int kMeetingLabel = 2009;
constexpr int kNameLabel = 2010;
constexpr UINT kAppendLogMessage = WM_APP + 1;

std::wstring widen(const std::string& value) {
  if (value.empty()) {
    return {};
  }
  const int size = MultiByteToWideChar(CP_UTF8, 0, value.data(), static_cast<int>(value.size()), nullptr, 0);
  std::wstring result(static_cast<size_t>(size), L'\0');
  MultiByteToWideChar(CP_UTF8, 0, value.data(), static_cast<int>(value.size()), result.data(), size);
  return result;
}

std::string narrow(const std::wstring& value) {
  if (value.empty()) {
    return {};
  }
  const int size = WideCharToMultiByte(CP_UTF8, 0, value.data(), static_cast<int>(value.size()), nullptr, 0, nullptr, nullptr);
  std::string result(static_cast<size_t>(size), '\0');
  WideCharToMultiByte(CP_UTF8, 0, value.data(), static_cast<int>(value.size()), result.data(), size, nullptr, nullptr);
  return result;
}

std::wstring quotePath(const std::wstring& value) {
  return L"\"" + value + L"\"";
}

std::string jsonEscape(const std::string& value) {
  std::string out;
  out.reserve(value.size() + 8);
  for (const char ch : value) {
    switch (ch) {
      case '"': out += "\\\""; break;
      case '\\': out += "\\\\"; break;
      case '\b': out += "\\b"; break;
      case '\f': out += "\\f"; break;
      case '\n': out += "\\n"; break;
      case '\r': out += "\\r"; break;
      case '\t': out += "\\t"; break;
      default:
        if (static_cast<unsigned char>(ch) < 0x20) {
          out += "?";
        } else {
          out.push_back(ch);
        }
        break;
    }
  }
  return out;
}

std::wstring currentDirectory() {
  std::array<wchar_t, MAX_PATH> buffer{};
  const DWORD length = GetCurrentDirectoryW(static_cast<DWORD>(buffer.size()), buffer.data());
  return std::wstring(buffer.data(), buffer.data() + length);
}

std::filesystem::path repoRoot() {
  return std::filesystem::path(currentDirectory());
}

bool fileExists(const std::filesystem::path& path) {
  std::error_code error;
  return std::filesystem::exists(path, error) && std::filesystem::is_regular_file(path, error);
}

bool directoryExists(const std::filesystem::path& path) {
  std::error_code error;
  return std::filesystem::exists(path, error) && std::filesystem::is_directory(path, error);
}

std::optional<std::filesystem::path> normalizeZoomSdkRoot(const std::filesystem::path& candidate) {
  if (candidate.empty()) {
    return std::nullopt;
  }
  if (fileExists(candidate / L"h" / L"zoom_sdk.h")) {
    return candidate;
  }
  const auto x64 = candidate / L"x64";
  if (fileExists(x64 / L"h" / L"zoom_sdk.h")) {
    return x64;
  }
  return std::nullopt;
}

std::filesystem::path envPath(const wchar_t* name) {
  std::array<wchar_t, 4096> buffer{};
  const DWORD length = GetEnvironmentVariableW(name, buffer.data(), static_cast<DWORD>(buffer.size()));
  if (length == 0 || length >= buffer.size()) {
    return {};
  }
  return std::filesystem::path(std::wstring(buffer.data(), buffer.data() + length));
}

bool envEnabled(const wchar_t* name) {
  std::array<wchar_t, 64> buffer{};
  const DWORD length = GetEnvironmentVariableW(name, buffer.data(), static_cast<DWORD>(buffer.size()));
  if (length == 0 || length >= buffer.size()) {
    return false;
  }
  std::wstring value(buffer.data(), buffer.data() + length);
  std::transform(value.begin(), value.end(), value.begin(), [](wchar_t ch) {
    return static_cast<wchar_t>(towlower(ch));
  });
  return value == L"1" || value == L"true" || value == L"yes" || value == L"on";
}

std::filesystem::path findZoomSdkRoot() {
  const auto root = repoRoot();
  std::vector<std::filesystem::path> candidates{
      envPath(L"COREVIDEO_ZOOM_RUNTIME_DIR"),
      envPath(L"ZOOM_SDK_DIR"),
      root / L"native-core" / L"zoom-runtime" / L"windows" / L"x64",
      root / L"ZoomSDK" / L"zoom-sdk-windows-7.0.5.39292" / L"zoom-sdk-windows-7.0.5.39292" / L"x64",
      root / L"ZoomSDK" / L"zoom-sdk-windows-7.0.5.39292" / L"zoom-sdk-windows-7.0.5.39292",
      root / L"ZoomSDK" / L"zoom-sdk-windows-7.0.5.39292" / L"x64",
      root / L"ZoomSDK" / L"zoom-sdk-windows-7.0.5.39292",
      root / L"ZoomSDK" / L"x64",
      root / L"ZoomSDK",
  };
  for (const auto& candidate : candidates) {
    if (auto normalized = normalizeZoomSdkRoot(candidate)) {
      return *normalized;
    }
  }
  return {};
}

std::filesystem::path findZoomEngine() {
  const auto root = repoRoot();
  const std::vector<std::filesystem::path> candidates{
      envPath(L"COREVIDEO_ZOOM_ENGINE_PATH"),
      root / L"native" / L"build-dev" / L"corevideo-zoom-engine.exe",
      root / L"native" / L"build-dev" / L"Release" / L"corevideo-zoom-engine.exe",
      root / L"native" / L"build" / L"corevideo-zoom-engine.exe",
      root / L"native" / L"build" / L"Release" / L"corevideo-zoom-engine.exe",
  };
  for (const auto& candidate : candidates) {
    if (fileExists(candidate)) {
      return candidate;
    }
  }
  return {};
}

std::wstring zoomReadinessText() {
  const auto sdkRoot = findZoomSdkRoot();
  const auto zoomEngine = findZoomEngine();
  std::wostringstream out;
  out << L"ZOOM SDK\r\n";
  if (sdkRoot.empty()) {
    out << L"SDK: missing\r\n"
        << L"Put SDK at ZoomSDK\\zoom-sdk-windows-7.0.5.39292\\x64 or set ZOOM_SDK_DIR.\r\n"
        << L"Then run scripts\\stage-zoom-sdk.ps1 and scripts\\build-native-dev.ps1.";
    return out.str();
  }

  const std::vector<std::pair<const wchar_t*, std::filesystem::path>> required{
      {L"sdk.dll", sdkRoot / L"bin" / L"sdk.dll"},
      {L"sdk.lib", sdkRoot / L"lib" / L"sdk.lib"},
      {L"zoom_sdk.h", sdkRoot / L"h" / L"zoom_sdk.h"},
      {L"raw video", sdkRoot / L"h" / L"rawdata" / L"zoom_rawdata_api.h"},
      {L"raw renderer", sdkRoot / L"h" / L"rawdata" / L"rawdata_renderer_interface.h"},
      {L"raw audio", sdkRoot / L"h" / L"rawdata" / L"rawdata_audio_helper_interface.h"},
  };
  int missing = 0;
  for (const auto& [label, path] : required) {
    if (!fileExists(path)) {
      ++missing;
    }
  }

  const auto binDir = sdkRoot / L"bin";
  int dllCount = 0;
  if (directoryExists(binDir)) {
    for (const auto& entry : std::filesystem::directory_iterator(binDir)) {
      if (entry.path().extension() == L".dll") {
        ++dllCount;
      }
    }
  }

  out << L"SDK: " << (missing == 0 ? L"ready" : L"blocked") << L"\r\n"
      << L"Root: " << sdkRoot.wstring() << L"\r\n"
      << L"DLLs: " << dllCount << L"\r\n"
      << L"Zoom engine: " << (zoomEngine.empty() ? L"missing" : zoomEngine.wstring()) << L"\r\n";
  if (missing > 0) {
    out << L"Missing required files: " << missing << L"\r\n";
  }
  if (zoomEngine.empty()) {
    out << L"Run scripts\\build-native-dev.ps1 to build real Zoom join/media helper.\r\n";
  }
  if (missing == 0 && !zoomEngine.empty()) {
    out << L"Real Zoom path can be used by native media core.";
  }
  return out.str();
}

std::vector<std::filesystem::path> nativeCoreCandidates() {
  const auto cwd = repoRoot();
  std::vector<std::filesystem::path> candidates;
  if (envEnabled(L"COREVIDEO_STUDIO_USE_DEV_CORE")) {
    candidates.push_back(cwd / L"native" / L"build-dev" / L"corevideo-native.exe");
    candidates.push_back(cwd / L"native" / L"build-dev" / L"Release" / L"corevideo-native.exe");
    candidates.push_back(cwd / L"native" / L"build-dev" / L"Debug" / L"corevideo-native.exe");
  }
  candidates.push_back(cwd / L"native" / L"build" / L"corevideo-native.exe");
  candidates.push_back(cwd / L"native" / L"build" / L"Release" / L"corevideo-native.exe");
  candidates.push_back(cwd / L"native" / L"build" / L"Debug" / L"corevideo-native.exe");

  std::array<wchar_t, MAX_PATH> modulePath{};
  const DWORD moduleLength = GetModuleFileNameW(nullptr, modulePath.data(), static_cast<DWORD>(modulePath.size()));
  if (moduleLength > 0) {
    auto appDir = std::filesystem::path(std::wstring(modulePath.data(), modulePath.data() + moduleLength)).parent_path();
    for (int i = 0; i < 6; ++i) {
      candidates.push_back(appDir / L"native" / L"build" / L"corevideo-native.exe");
      candidates.push_back(appDir / L"native" / L"build" / L"Release" / L"corevideo-native.exe");
      candidates.push_back(appDir / L"native" / L"build" / L"Debug" / L"corevideo-native.exe");
      appDir = appDir.parent_path();
    }
  }
  return candidates;
}

std::filesystem::path findNativeCore() {
  for (const auto& candidate : nativeCoreCandidates()) {
    std::error_code error;
    if (std::filesystem::exists(candidate, error)) {
      return candidate;
    }
  }
  return {};
}

std::wstring g_nativeCorePath;

void configureNativeCoreEnvironment() {
  if (envEnabled(L"COREVIDEO_STUDIO_USE_DEV_CORE")) {
    const auto zoomEngine = findZoomEngine();
    if (!zoomEngine.empty()) {
      SetEnvironmentVariableW(L"COREVIDEO_ZOOM_ENGINE_PATH", zoomEngine.wstring().c_str());
    }
  } else {
    SetEnvironmentVariableW(L"COREVIDEO_ZOOM_ENGINE_PATH", nullptr);
  }

  const auto sdkRoot = findZoomSdkRoot();
  if (!sdkRoot.empty()) {
    SetEnvironmentVariableW(L"COREVIDEO_ZOOM_RUNTIME_DIR", sdkRoot.wstring().c_str());
    const auto binDir = sdkRoot / L"bin";
    if (directoryExists(binDir)) {
      std::array<wchar_t, 32767> pathBuffer{};
      const DWORD length = GetEnvironmentVariableW(L"PATH", pathBuffer.data(), static_cast<DWORD>(pathBuffer.size()));
      std::wstring updated = binDir.wstring();
      updated += L";";
      if (length > 0 && length < pathBuffer.size()) {
        updated.append(pathBuffer.data(), pathBuffer.data() + length);
      }
      SetEnvironmentVariableW(L"PATH", updated.c_str());
    }
  }
  SetEnvironmentVariableW(L"COREVIDEO_ZOOM_JOIN_WAIT_MS", L"45000");
}

class NativeCoreClient {
 public:
  ~NativeCoreClient() {
    stop();
  }

  bool start(HWND window) {
    if (running_) {
      append(window, "Native media core is already running.");
      return true;
    }

    window_ = window;
    const auto exe = findNativeCore();
    if (exe.empty()) {
      append(window, "Could not find native/build/corevideo-native.exe. Build native first.");
      return false;
    }
    configureNativeCoreEnvironment();

    SECURITY_ATTRIBUTES security{};
    security.nLength = sizeof(security);
    security.bInheritHandle = TRUE;

    HANDLE childStdoutRead = nullptr;
    HANDLE childStdoutWrite = nullptr;
    HANDLE childStdinRead = nullptr;
    HANDLE childStdinWrite = nullptr;

    if (!CreatePipe(&childStdoutRead, &childStdoutWrite, &security, 0) ||
        !SetHandleInformation(childStdoutRead, HANDLE_FLAG_INHERIT, 0) ||
        !CreatePipe(&childStdinRead, &childStdinWrite, &security, 0) ||
        !SetHandleInformation(childStdinWrite, HANDLE_FLAG_INHERIT, 0)) {
      append(window, "Failed to create stdio pipes for native media core.");
      closeHandle(childStdoutRead);
      closeHandle(childStdoutWrite);
      closeHandle(childStdinRead);
      closeHandle(childStdinWrite);
      return false;
    }

    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    startup.dwFlags = STARTF_USESTDHANDLES;
    startup.hStdOutput = childStdoutWrite;
    startup.hStdError = childStdoutWrite;
    startup.hStdInput = childStdinRead;

    PROCESS_INFORMATION process{};
    auto command = quotePath(exe.wstring());
    const BOOL created = CreateProcessW(
        nullptr,
        command.data(),
        nullptr,
        nullptr,
        TRUE,
        CREATE_NO_WINDOW,
        nullptr,
        nullptr,
        &startup,
        &process);

    closeHandle(childStdoutWrite);
    closeHandle(childStdinRead);

    if (!created) {
      append(window, "Failed to launch native media core: " + narrow(exe.wstring()));
      closeHandle(childStdoutRead);
      closeHandle(childStdinWrite);
      return false;
    }

    process_ = process.hProcess;
    thread_ = process.hThread;
    stdoutRead_ = childStdoutRead;
    stdinWrite_ = childStdinWrite;
    running_ = true;

    g_nativeCorePath = exe.wstring();
    const bool usingDevCore = envEnabled(L"COREVIDEO_STUDIO_USE_DEV_CORE") &&
                              exe.string().find("build-dev") != std::string::npos;
    append(window, std::string("Launched native media core") + (usingDevCore ? " (dev build)" : "") + ": " + narrow(exe.wstring()));
    reader_ = std::thread([this]() { readLoop(); });
    return true;
  }

  void stop() {
    running_ = false;
    closeHandle(stdinWrite_);
    stdinWrite_ = nullptr;
    if (process_) {
      TerminateProcess(process_, 0);
    }
    if (reader_.joinable()) {
      reader_.join();
    }
    closeHandle(stdoutRead_);
    closeHandle(thread_);
    closeHandle(process_);
    stdoutRead_ = nullptr;
    thread_ = nullptr;
    process_ = nullptr;
  }

  bool send(const std::string& json) {
    if (!running_ || !stdinWrite_) {
      append(window_, "Native media core is not running.");
      return false;
    }
    const std::string line = json + "\n";
    DWORD written = 0;
    const BOOL ok = WriteFile(stdinWrite_, line.data(), static_cast<DWORD>(line.size()), &written, nullptr);
    if (!ok || written != line.size()) {
      append(window_, "Failed to write request to native media core.");
      return false;
    }
    append(window_, "> " + json);
    return true;
  }

 private:
  static void closeHandle(HANDLE handle) {
    if (handle) {
      CloseHandle(handle);
    }
  }

  static void append(HWND window, const std::string& text) {
    if (!window) {
      return;
    }
    auto* payload = new std::wstring(widen(text + "\r\n"));
    PostMessageW(window, kAppendLogMessage, 0, reinterpret_cast<LPARAM>(payload));
  }

  void readLoop() {
    std::string pending;
    std::array<char, 4096> buffer{};
    while (running_) {
      DWORD read = 0;
      if (!ReadFile(stdoutRead_, buffer.data(), static_cast<DWORD>(buffer.size()), &read, nullptr) || read == 0) {
        break;
      }
      pending.append(buffer.data(), buffer.data() + read);
      size_t lineEnd = std::string::npos;
      while ((lineEnd = pending.find('\n')) != std::string::npos) {
        auto line = pending.substr(0, lineEnd);
        pending.erase(0, lineEnd + 1);
        if (!line.empty() && line.back() == '\r') {
          line.pop_back();
        }
        append(window_, "< " + line);
      }
    }
    append(window_, "Native media core process stopped.");
    running_ = false;
  }

  HWND window_ = nullptr;
  HANDLE process_ = nullptr;
  HANDLE thread_ = nullptr;
  HANDLE stdoutRead_ = nullptr;
  HANDLE stdinWrite_ = nullptr;
  std::thread reader_;
  std::atomic<bool> running_{false};
};

NativeCoreClient g_core;
HWND g_log = nullptr;
HWND g_status = nullptr;
HWND g_scenes = nullptr;
HWND g_program = nullptr;
HWND g_participants = nullptr;
HWND g_health = nullptr;
HWND g_meetingUrl = nullptr;
HWND g_displayName = nullptr;
HWND g_meetingLabel = nullptr;
HWND g_nameLabel = nullptr;
ProgramPreview g_preview;
corevideo::studio::StudioState g_studioState;
int g_requestId = 1;

std::string nextId(const std::string& prefix) {
  return prefix + "-" + std::to_string(g_requestId++);
}

void appendLog(HWND log, const std::wstring& text) {
  const int length = GetWindowTextLengthW(log);
  SendMessageW(log, EM_SETSEL, length, length);
  SendMessageW(log, EM_REPLACESEL, FALSE, reinterpret_cast<LPARAM>(text.c_str()));
}

void setStatus(const wchar_t* text) {
  if (g_status) {
    SetWindowTextW(g_status, text);
  }
}

void setStudioText(HWND handle, const wchar_t* text) {
  if (handle) {
    SetWindowTextW(handle, text);
  }
}

std::string editText(HWND handle) {
  if (!handle) {
    return {};
  }
  const int length = GetWindowTextLengthW(handle);
  std::wstring value(static_cast<size_t>(length) + 1, L'\0');
  if (length > 0) {
    GetWindowTextW(handle, value.data(), length + 1);
  }
  value.resize(static_cast<size_t>(length));
  return narrow(value);
}

std::string participantIdOrFallback(size_t index, const std::string& fallback) {
  if (index < g_studioState.participantIds.size() && !g_studioState.participantIds[index].empty()) {
    return g_studioState.participantIds[index];
  }
  return fallback;
}

std::wstring scenesStateText() {
  std::ostringstream out;
  out << "SCENE / OUTPUT\r\n\r\n";
  if (g_studioState.sceneId.empty() || g_studioState.sceneId == "unloaded") {
    out << "Scene: not loaded\r\n";
    out << "Action: Magic Scene\r\n\r\n";
    out << "Templates:\r\n"
        << "  Interview\r\n"
        << "  Speaker + Slides\r\n"
        << "  Panel\r\n"
        << "  Lower Third\r\n"
        << "  Program Recording\r\n";
  } else {
    out << "Scene: " << g_studioState.sceneId << "\r\n";
    if (g_studioState.routeCount > 0) {
      out << "Routes: " << g_studioState.routeCount << "\r\n";
    }
    if (g_studioState.overlayCount > 0) {
      out << "Overlays: " << g_studioState.overlayCount << "\r\n";
    }
    if (g_studioState.captionsEnabled) {
      out << "Captions: on";
      if (!g_studioState.captionStatus.empty()) {
        out << " (" << g_studioState.captionStatus << ")";
      }
      out << "\r\n";
    }
    if (!g_studioState.outputDestinations.empty()) {
      out << "Outputs: ";
      for (std::size_t index = 0; index < g_studioState.outputDestinations.size(); ++index) {
        if (index > 0) {
          out << ", ";
        }
        out << g_studioState.outputDestinations[index];
      }
      out << "\r\n";
    }
    if (!g_studioState.encoderLifecycleStatus.empty()) {
      out << "Encoder lifecycle: " << g_studioState.encoderLifecycleStatus << "\r\n";
    }
    if (!g_studioState.outputStatus.empty()) {
      out << "Output session: " << g_studioState.outputStatus << "\r\n";
    }
    if (g_studioState.activeSenderCount > 0) {
      out << "Active senders: " << g_studioState.activeSenderCount << "\r\n";
    }
    out << "\r\nAction: Record Program to arm recording + RTMP.";
  }
  return widen(out.str());
}

std::wstring studioStateText() {
  std::ostringstream health;
  health << "NATIVE CORE\r\n\r\n";
  if (!g_nativeCorePath.empty()) {
    health << "Core: " << narrow(g_nativeCorePath) << "\r\n";
  }
  health << "Connected: " << (g_studioState.connected ? "yes" : "no") << "\r\n"
         << "Zoom: " << (g_studioState.meetingState.empty() ? "idle" : g_studioState.meetingState) << "\r\n"
         << "Active speaker: " << (g_studioState.activeSpeakerId.empty() ? "pending" : g_studioState.activeSpeakerId) << "\r\n"
         << "Handshake: " << (g_studioState.handshakeSeen ? "yes" : "no") << "\r\n"
         << "Renderer: " << (g_studioState.renderer.empty() ? "pending" : g_studioState.renderer) << "\r\n"
         << "Encoder: " << (g_studioState.encoder.empty() ? "pending" : g_studioState.encoder) << "\r\n"
         << "Health: " << (g_studioState.healthStatus.empty() ? "pending" : g_studioState.healthStatus) << "\r\n"
         << "First frame: " << (g_studioState.firstFrameSeen ? "yes" : "waiting") << "\r\n"
         << "Program: " << (g_studioState.programFrameHealth.empty() ? "pending" : g_studioState.programFrameHealth) << "\r\n"
         << "Output: " << (g_studioState.outputStatus.empty() ? "idle" : g_studioState.outputStatus) << "\r\n"
         << "Frames: " << g_studioState.frameCount << "\r\n"
         << "Senders: " << g_studioState.activeSenderCount;
  if (!g_studioState.recordingHealth.empty()) {
    health << "\r\nRecording health: " << g_studioState.recordingHealth;
  }
  if (!g_studioState.recordingArtifactPath.empty()) {
    health << "\r\nRecording: " << g_studioState.recordingArtifactPath;
  }
  if (!g_studioState.lastErrorText.empty()) {
    health << "\r\nError: " << g_studioState.lastErrorText;
  }
  auto text = widen(health.str());
  text += L"\r\n\r\n";
  text += zoomReadinessText();
  return text;
}

std::string programPreviewWaitingHint() {
  if (g_studioState.firstFrameSeen) {
    return "Program frames are flowing. Preview updates on each program-frame-preview event.";
  }
  if (!g_studioState.sceneId.empty() && g_studioState.sceneId != "unloaded") {
    return "Scene " + g_studioState.sceneId + " is loaded. Start Record Program to produce preview frames.";
  }
  if (!g_studioState.meetingState.empty() && g_studioState.meetingState != "idle") {
    return "Zoom is " + g_studioState.meetingState + ". Use Magic Scene, then Record Program.";
  }
  return "Join Zoom, build Magic Scene, then Record Program to surface program frames.";
}

std::wstring participantsText() {
  std::ostringstream out;
  out << "PARTICIPANTS";
  if (!g_studioState.meetingState.empty()) {
    out << "  (" << g_studioState.meetingState << ")";
  }
  out << "\r\n\r\n";
  if (g_studioState.participantLines.empty()) {
    out << "No Zoom participants yet.\r\n\r\n"
        << "Use Join Zoom to create the current native-core roster.\r\n"
        << "With Zoom SDK adapters configured, this becomes the live meeting roster.";
  } else {
    for (const auto& participant : g_studioState.participantLines) {
      out << participant << "\r\n";
    }
  }
  return widen(out.str());
}

void refreshStatePanels() {
  g_preview.setWaitingHint(programPreviewWaitingHint());
  setStudioText(g_scenes, scenesStateText().c_str());
  setStudioText(g_health, studioStateText().c_str());
  setStudioText(g_participants, participantsText().c_str());
  const auto summary = corevideo::studio::summarizeStudioState(g_studioState);
  if (!summary.empty()) {
    setStatus(widen(summary).c_str());
  }
  if (g_program) {
    InvalidateRect(g_program, nullptr, TRUE);
  }
}

void refreshStaticPanels() {
  refreshStatePanels();
}

void sendJoinZoom() {
  std::string meetingUrl = editText(g_meetingUrl);
  std::string displayName = editText(g_displayName);
  if (meetingUrl.empty()) {
    meetingUrl = "https://zoom.us/j/123456789";
  }
  if (displayName.empty()) {
    displayName = "CoreVideo Producer";
  }
  g_core.send(
      "{\"id\":\"" + nextId("join") +
      "\",\"type\":\"zoom-join\",\"payload\":{"
      "\"meetingUrl\":\"" + jsonEscape(meetingUrl) + "\","
      "\"displayName\":\"" + jsonEscape(displayName) + "\","
      "\"webinar\":false"
      "}}");
}

void sendLeaveZoom() {
  g_core.send("{\"id\":\"" + nextId("leave") + "\",\"type\":\"zoom-leave\"}");
}

void sendMagicScene() {
  const std::string host = jsonEscape(participantIdOrFallback(0, "synthetic-speaker-1"));
  const std::string guest = jsonEscape(participantIdOrFallback(1, "synthetic-speaker-2"));
  g_core.send(
      "{\"id\":\"" + nextId("scene") +
      "\",\"type\":\"media-core-sync\",\"commands\":["
      "{\"type\":\"load-scene-graph\",\"sceneId\":\"magic-interview\",\"routes\":["
      "{\"routeId\":\"host\",\"mode\":\"fixed\",\"participantId\":\"" + host + "\",\"audioRole\":\"mix\"},"
      "{\"routeId\":\"guest\",\"mode\":\"active-speaker\",\"participantId\":\"" + guest + "\",\"audioRole\":\"mix\"}"
      "]},"
      "{\"type\":\"set-overlay-asset\",\"overlayId\":\"lower-third\",\"text\":\"CoreVideo Pro Interview\",\"position\":\"lower-third\"},"
      "{\"type\":\"sync-participant-audio-mix\",\"channels\":["
      "{\"participantId\":\"" + host + "\",\"inputLevel\":72,\"muted\":false,\"noiseSuppression\":true},"
      "{\"participantId\":\"" + guest + "\",\"inputLevel\":48,\"muted\":false,\"noiseSuppression\":true}"
      "]},"
      "{\"type\":\"set-caption-enabled\",\"enabled\":true},"
      "{\"type\":\"push-caption-cue\",\"text\":\"CoreVideo Pro native Studio is producing this Zoom show.\",\"speaker\":\"Producer\",\"atMs\":0}"
      "]}");
}

void sendRecordProgram() {
  const std::string host = jsonEscape(participantIdOrFallback(0, "synthetic-speaker-1"));
  g_core.send(
      "{\"id\":\"" + nextId("output") +
      "\",\"type\":\"media-core-sync\",\"commands\":["
      "{\"type\":\"set-recording-targets\",\"targetFolder\":\"artifacts/native-recordings\",\"filenamePrefix\":\"corevideo-studio\",\"format\":\"mp4\",\"quality\":\"high\",\"isoParticipantIds\":[\"" + host + "\"]},"
      "{\"type\":\"prepare-encoder-session\",\"preparedAtMs\":0,\"reason\":\"native-studio\"},"
      "{\"type\":\"start-encoder-session\",\"startedAtMs\":0},"
      "{\"type\":\"start-program-output\",\"destinations\":[\"recording\",\"rtmp\"],\"isoParticipantIds\":[\"" + host + "\"]},"
      "{\"type\":\"start-recording-session\",\"sessionId\":\"native-studio-test\",\"startedAtMs\":0}"
      "]}");
}

void sendHealth() {
  g_core.send("{\"id\":\"" + nextId("health") + "\",\"type\":\"get-output-health\"}");
}

void sendSnapshot() {
  g_core.send("{\"id\":\"" + nextId("snapshot") + "\",\"type\":\"snapshot\"}");
}

void layout(HWND window) {
  RECT rect{};
  GetClientRect(window, &rect);
  const int margin = 14;
  const int buttonHeight = 34;
  const int buttonWidth = 132;
  const int gap = 8;
  int x = margin;
  const int y = margin;
  const int statusHeight = 28;
  const int logHeight = 170;
  const int inputTop = y + buttonHeight + 10;
  const int inputHeight = 26;
  const int top = inputTop + inputHeight + statusHeight + 20;
  const int bottom = rect.bottom - margin;
  const int contentBottom = bottom - logHeight - gap;
  const int scenesWidth = 210;
  const int rightWidth = 260;
  const int healthHeight = 145;

  for (int id : {kButtonStartCore, kButtonJoinZoom, kButtonLeaveZoom, kButtonMagicScene, kButtonRecord, kButtonHealth, kButtonSnapshot, kButtonClear}) {
    HWND child = GetDlgItem(window, id);
    MoveWindow(child, x, y, buttonWidth, buttonHeight, TRUE);
    x += buttonWidth + gap;
  }

  const int labelWidth = 70;
  const int nameWidth = 180;
  MoveWindow(g_meetingLabel, margin, inputTop + 5, labelWidth, inputHeight, TRUE);
  MoveWindow(g_meetingUrl, margin + labelWidth, inputTop, rect.right - margin * 2 - labelWidth - nameWidth - labelWidth - gap * 2, inputHeight, TRUE);
  MoveWindow(g_nameLabel, rect.right - margin - nameWidth - labelWidth, inputTop + 5, labelWidth, inputHeight, TRUE);
  MoveWindow(g_displayName, rect.right - margin - nameWidth, inputTop, nameWidth, inputHeight, TRUE);
  MoveWindow(g_status, margin, inputTop + inputHeight + 8, rect.right - margin * 2, statusHeight, TRUE);
  MoveWindow(g_scenes, margin, top, scenesWidth, contentBottom - top, TRUE);
  MoveWindow(g_program, margin + scenesWidth + gap, top, rect.right - (margin * 2 + scenesWidth + rightWidth + gap * 2), contentBottom - top, TRUE);
  MoveWindow(g_participants, rect.right - margin - rightWidth, top, rightWidth, contentBottom - top - healthHeight - gap, TRUE);
  MoveWindow(g_health, rect.right - margin - rightWidth, contentBottom - healthHeight, rightWidth, healthHeight, TRUE);
  MoveWindow(g_log, margin, contentBottom + gap, rect.right - margin * 2, logHeight, TRUE);
}

HWND button(HWND parent, int id, const wchar_t* text) {
  return CreateWindowExW(
      0,
      L"BUTTON",
      text,
      WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
      0,
      0,
      0,
      0,
      parent,
      reinterpret_cast<HMENU>(static_cast<intptr_t>(id)),
      GetModuleHandleW(nullptr),
      nullptr);
}

HWND edit(HWND parent, int id, const wchar_t* text) {
  HWND handle = CreateWindowExW(
      WS_EX_CLIENTEDGE,
      L"EDIT",
      text,
      WS_CHILD | WS_VISIBLE | ES_LEFT | ES_AUTOHSCROLL,
      0,
      0,
      0,
      0,
      parent,
      reinterpret_cast<HMENU>(static_cast<intptr_t>(id)),
      GetModuleHandleW(nullptr),
      nullptr);
  SendMessageW(handle, WM_SETFONT, reinterpret_cast<WPARAM>(GetStockObject(DEFAULT_GUI_FONT)), TRUE);
  return handle;
}

HWND panel(HWND parent, int id, const wchar_t* text) {
  HWND handle = CreateWindowExW(
      WS_EX_CLIENTEDGE,
      L"EDIT",
      text,
      WS_CHILD | WS_VISIBLE | ES_LEFT | ES_MULTILINE | ES_READONLY,
      0,
      0,
      0,
      0,
      parent,
      reinterpret_cast<HMENU>(static_cast<intptr_t>(id)),
      GetModuleHandleW(nullptr),
      nullptr);
  SendMessageW(handle, WM_SETFONT, reinterpret_cast<WPARAM>(GetStockObject(DEFAULT_GUI_FONT)), TRUE);
  return handle;
}

HWND programPanel(HWND parent) {
  return CreateWindowExW(
      WS_EX_CLIENTEDGE,
      L"STATIC",
      L"",
      WS_CHILD | WS_VISIBLE | SS_OWNERDRAW,
      0,
      0,
      0,
      0,
      parent,
      reinterpret_cast<HMENU>(static_cast<intptr_t>(kProgramPanel)),
      GetModuleHandleW(nullptr),
      nullptr);
}

LRESULT CALLBACK windowProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
  switch (message) {
    case WM_CREATE:
      button(window, kButtonStartCore, L"Start Core");
      button(window, kButtonJoinZoom, L"Join Zoom");
      button(window, kButtonLeaveZoom, L"Leave Zoom");
      button(window, kButtonMagicScene, L"Magic Scene");
      button(window, kButtonRecord, L"Record Program");
      button(window, kButtonHealth, L"Health");
      button(window, kButtonSnapshot, L"Snapshot");
      button(window, kButtonClear, L"Clear Log");
      g_meetingLabel = CreateWindowExW(0, L"STATIC", L"Meeting", WS_CHILD | WS_VISIBLE, 0, 0, 0, 0, window, reinterpret_cast<HMENU>(static_cast<intptr_t>(kMeetingLabel)), GetModuleHandleW(nullptr), nullptr);
      g_nameLabel = CreateWindowExW(0, L"STATIC", L"Name", WS_CHILD | WS_VISIBLE, 0, 0, 0, 0, window, reinterpret_cast<HMENU>(static_cast<intptr_t>(kNameLabel)), GetModuleHandleW(nullptr), nullptr);
      g_meetingUrl = edit(window, kMeetingUrlEdit, L"https://zoom.us/j/123456789");
      g_displayName = edit(window, kDisplayNameEdit, L"CoreVideo Producer");
      g_status = CreateWindowExW(0, L"STATIC", L"Native Studio ready. Start Core to connect.", WS_CHILD | WS_VISIBLE, 0, 0, 0, 0, window, reinterpret_cast<HMENU>(static_cast<intptr_t>(kStatusLabel)), GetModuleHandleW(nullptr), nullptr);
      g_scenes = panel(window, kScenesPanel, L"");
      g_program = programPanel(window);
      g_participants = panel(window, kParticipantsPanel, L"");
      g_health = panel(window, kHealthPanel, L"");
      g_log = CreateWindowExW(
          WS_EX_CLIENTEDGE,
          L"EDIT",
          L"",
          WS_CHILD | WS_VISIBLE | WS_VSCROLL | ES_LEFT | ES_MULTILINE | ES_AUTOVSCROLL | ES_READONLY,
          0,
          0,
          0,
          0,
          window,
          reinterpret_cast<HMENU>(static_cast<intptr_t>(kLogEdit)),
          GetModuleHandleW(nullptr),
          nullptr);
      SendMessageW(g_log, WM_SETFONT, reinterpret_cast<WPARAM>(GetStockObject(DEFAULT_GUI_FONT)), TRUE);
      SendMessageW(g_status, WM_SETFONT, reinterpret_cast<WPARAM>(GetStockObject(DEFAULT_GUI_FONT)), TRUE);
      SendMessageW(g_meetingLabel, WM_SETFONT, reinterpret_cast<WPARAM>(GetStockObject(DEFAULT_GUI_FONT)), TRUE);
      SendMessageW(g_nameLabel, WM_SETFONT, reinterpret_cast<WPARAM>(GetStockObject(DEFAULT_GUI_FONT)), TRUE);
      refreshStaticPanels();
      layout(window);
      g_core.start(window);
      setStatus(L"Native media core launched. Join Zoom, then use Magic Scene and Record Program.");
      return 0;

    case WM_SIZE:
      layout(window);
      return 0;

    case WM_COMMAND:
      switch (LOWORD(wParam)) {
        case kButtonStartCore:
          if (g_core.start(window)) {
            setStatus(L"Native media core running.");
          }
          return 0;
        case kButtonJoinZoom:
          sendJoinZoom();
          return 0;
        case kButtonLeaveZoom:
          sendLeaveZoom();
          return 0;
        case kButtonMagicScene:
          sendMagicScene();
          return 0;
        case kButtonRecord:
          sendRecordProgram();
          return 0;
        case kButtonHealth:
          sendHealth();
          return 0;
        case kButtonSnapshot:
          sendSnapshot();
          return 0;
        case kButtonClear:
          SetWindowTextW(g_log, L"");
          return 0;
        default:
          break;
      }
      break;

    case WM_DRAWITEM: {
      auto* drawItem = reinterpret_cast<DRAWITEMSTRUCT*>(lParam);
      if (drawItem && drawItem->CtlID == kProgramPanel) {
        g_preview.render(drawItem->hDC, drawItem->rcItem);
        return TRUE;
      }
      break;
    }

    case kAppendLogMessage: {
      auto* text = reinterpret_cast<std::wstring*>(lParam);
      if (text) {
        appendLog(g_log, *text);
        const std::string logLine = narrow(*text);
        const std::string payload = logLine.rfind("< ", 0) == 0 ? logLine.substr(2) : logLine;
        const bool previewUpdated = g_preview.updateFromEventLine(payload);
        if (previewUpdated) {
          if (const auto metadata = g_preview.latestMetadata()) {
            corevideo::studio::notePreviewFrame(g_studioState, metadata->frameNumber, metadata->health);
          }
          InvalidateRect(g_program, nullptr, TRUE);
        }
        corevideo::studio::applyStudioStateLine(g_studioState, payload);
        refreshStatePanels();
        delete text;
      }
      return 0;
    }

    case WM_DESTROY:
      g_core.stop();
      PostQuitMessage(0);
      return 0;

    default:
      break;
  }
  return DefWindowProcW(window, message, wParam, lParam);
}

}  // namespace

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int showCommand) {
  const wchar_t* className = L"CoreVideoStudioNativeWindow";

  WNDCLASSW windowClass{};
  windowClass.lpfnWndProc = windowProc;
  windowClass.hInstance = instance;
  windowClass.lpszClassName = className;
  windowClass.hCursor = LoadCursorW(nullptr, IDC_ARROW);
  windowClass.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);

  RegisterClassW(&windowClass);

  HWND window = CreateWindowExW(
      0,
      className,
      L"CoreVideo Pro - Native Studio",
      WS_OVERLAPPEDWINDOW,
      CW_USEDEFAULT,
      CW_USEDEFAULT,
      1120,
      720,
      nullptr,
      nullptr,
      instance,
      nullptr);

  if (!window) {
    return 1;
  }

  ShowWindow(window, showCommand);
  UpdateWindow(window);

  MSG message{};
  while (GetMessageW(&message, nullptr, 0, 0)) {
    TranslateMessage(&message);
    DispatchMessageW(&message);
  }

  return static_cast<int>(message.wParam);
}
