#include <windows.h>

#include <array>
#include <atomic>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

namespace {

constexpr int kButtonStartCore = 1001;
constexpr int kButtonLoadScene = 1002;
constexpr int kButtonStartOutput = 1003;
constexpr int kButtonHealth = 1004;
constexpr int kButtonSnapshot = 1005;
constexpr int kButtonClear = 1006;
constexpr int kLogEdit = 2001;
constexpr int kStatusLabel = 2002;
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

std::wstring currentDirectory() {
  std::array<wchar_t, MAX_PATH> buffer{};
  const DWORD length = GetCurrentDirectoryW(static_cast<DWORD>(buffer.size()), buffer.data());
  return std::wstring(buffer.data(), buffer.data() + length);
}

std::vector<std::filesystem::path> nativeCoreCandidates() {
  const auto cwd = std::filesystem::path(currentDirectory());
  std::vector<std::filesystem::path> candidates;
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

    append(window, "Launched native media core: " + narrow(exe.wstring()));
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

void sendLoadScene() {
  g_core.send(
      "{\"id\":\"" + nextId("scene") +
      "\",\"type\":\"media-core-sync\",\"commands\":["
      "{\"type\":\"load-scene-graph\",\"sceneId\":\"native-test\",\"routes\":["
      "{\"routeId\":\"host\",\"mode\":\"fixed\",\"participantId\":\"synthetic-speaker-1\",\"audioRole\":\"mix\"},"
      "{\"routeId\":\"guest\",\"mode\":\"active-speaker\",\"participantId\":\"synthetic-speaker-2\",\"audioRole\":\"mix\"}"
      "]},"
      "{\"type\":\"set-overlay-asset\",\"overlayId\":\"lower-third\",\"text\":\"CoreVideo Native Studio\",\"position\":\"lower-third\"}"
      "]}");
}

void sendStartOutput() {
  g_core.send(
      "{\"id\":\"" + nextId("output") +
      "\",\"type\":\"media-core-sync\",\"commands\":["
      "{\"type\":\"start-program-output\",\"destinations\":[\"recording\",\"rtmp\"],\"isoParticipantIds\":[\"synthetic-speaker-1\"]},"
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

  for (int id : {kButtonStartCore, kButtonLoadScene, kButtonStartOutput, kButtonHealth, kButtonSnapshot, kButtonClear}) {
    HWND child = GetDlgItem(window, id);
    MoveWindow(child, x, y, buttonWidth, buttonHeight, TRUE);
    x += buttonWidth + gap;
  }

  MoveWindow(g_status, margin, y + buttonHeight + 10, rect.right - margin * 2, statusHeight, TRUE);
  MoveWindow(g_log, margin, y + buttonHeight + statusHeight + 18, rect.right - margin * 2, rect.bottom - (y + buttonHeight + statusHeight + 28), TRUE);
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

LRESULT CALLBACK windowProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
  switch (message) {
    case WM_CREATE:
      button(window, kButtonStartCore, L"Start Core");
      button(window, kButtonLoadScene, L"Load Scene");
      button(window, kButtonStartOutput, L"Start Output");
      button(window, kButtonHealth, L"Health");
      button(window, kButtonSnapshot, L"Snapshot");
      button(window, kButtonClear, L"Clear Log");
      g_status = CreateWindowExW(0, L"STATIC", L"Native Studio ready. Start Core to connect.", WS_CHILD | WS_VISIBLE, 0, 0, 0, 0, window, reinterpret_cast<HMENU>(static_cast<intptr_t>(kStatusLabel)), GetModuleHandleW(nullptr), nullptr);
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
      layout(window);
      g_core.start(window);
      setStatus(L"Native media core launched. Use Load Scene, Start Output, Health, Snapshot.");
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
        case kButtonLoadScene:
          sendLoadScene();
          return 0;
        case kButtonStartOutput:
          sendStartOutput();
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

    case kAppendLogMessage: {
      auto* text = reinterpret_cast<std::wstring*>(lParam);
      if (text) {
        appendLog(g_log, *text);
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
