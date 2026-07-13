// corevideo-browser-host.exe — render-only browser source host (BR-1).
// docs/capture-sources-spec.md §4 (Phase BR) / docs/sources-redesign-spec.md §C.
//
// One process per browser source (the zoom-engine isolation pattern: untrusted web
// content NEVER runs inside corevideo-native.exe). The host:
//
//   1. Verifies the WebView2 Runtime is installed (fails LOUDLY with exit code 3).
//   2. Creates a borderless, tool-window (hidden from Alt-Tab), positioned OFF the
//      virtual desktop, sized exactly --width x --height physical pixels (per-monitor
//      DPI aware V2 + WebView2 RasterizationScale pinned to 1.0).
//   3. Hosts a windowed CoreWebView2Controller in it with a fully transparent
//      DefaultBackgroundColor and hardened settings (no devtools, no context menus,
//      popups denied, downloads cancelled), navigated to --url.
//   4. Captures its OWN window with Windows.Graphics.Capture (same code shape as the
//      core's WgcScreenCaptureAdapter) and publishes BGRA frames — throttled to
//      --fps — into the named shared-memory seqlock buffer (--shm) the core maps
//      (BrowserSourceShm.h). Chromium's native-window occlusion tracker is disabled
//      (--disable-features=CalculateNativeWinOcclusion) so the off-screen page keeps
//      rendering at full rate.
//   5. Reads command lines on stdin: "reload\n" reloads the page; EOF (the core died
//      or dropped the source) exits — no orphan hosts, ever.
//
// Renderer/browser process failures exit the host (exit code 2) so the core's
// supervisor restarts it with capped backoff — crash-to-slate, never a hang.
//
// Self-test flags (standalone render proof, no core needed):
//   --dump-bmp <path> [--dump-after-ms N] [--exit-after-ms M]
// dumps the latest captured frame as a 32-bit BMP and prints machine-checkable
// pixel statistics (mean BGRA + sample points) to stderr.

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef WINVER
#define WINVER 0x0A00
#endif
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0A00
#endif

#include <windows.h>

#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Graphics.Capture.h>
#include <winrt/Windows.Graphics.DirectX.Direct3D11.h>
#include <winrt/Windows.Graphics.DirectX.h>
#include <windows.graphics.capture.interop.h>
#include <windows.graphics.directx.direct3d11.interop.h>

#include <d3d11.h>
#include <dxgi1_2.h>
#include <wrl.h>
#include <wrl/client.h>

#include "WebView2.h"
#include "WebView2EnvironmentOptions.h"

#include "modules/BrowserSourceShm.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

using Microsoft::WRL::Callback;
using Microsoft::WRL::ComPtr;
namespace wgc = winrt::Windows::Graphics::Capture;
namespace wgd = winrt::Windows::Graphics::DirectX;

namespace {

constexpr UINT kMsgReload = WM_APP + 1;
constexpr UINT kMsgQuit = WM_APP + 2;
constexpr UINT_PTR kTimerDump = 1;
constexpr UINT_PTR kTimerExit = 2;
constexpr UINT_PTR kTimerStats = 3;

struct Options {
  std::wstring url;
  int width = 1920;
  int height = 1080;
  int fps = 30;
  std::wstring shmName;
  std::wstring dumpBmpPath;
  int dumpAfterMs = 6000;
  int exitAfterMs = 0;  // 0 = run until stdin EOF / WM_CLOSE
  bool onscreen = false;  // debug: place the window on-screen instead of off-desktop
};

std::wstring widen(const std::string& value) {
  if (value.empty()) {
    return {};
  }
  const int needed = MultiByteToWideChar(CP_UTF8, 0, value.c_str(), -1, nullptr, 0);
  std::wstring result(needed > 0 ? needed - 1 : 0, L'\0');
  if (needed > 1) {
    MultiByteToWideChar(CP_UTF8, 0, value.c_str(), -1, result.data(), needed);
  }
  return result;
}

bool parseOptions(int argc, char** argv, Options& options, std::string& error) {
  for (int i = 1; i < argc; ++i) {
    const std::string arg = argv[i];
    auto next = [&](const char* name) -> const char* {
      if (i + 1 >= argc) {
        error = std::string(name) + " requires a value.";
        return nullptr;
      }
      return argv[++i];
    };
    if (arg == "--url") {
      const char* value = next("--url");
      if (value == nullptr) return false;
      options.url = widen(value);
    } else if (arg == "--width") {
      const char* value = next("--width");
      if (value == nullptr) return false;
      options.width = std::atoi(value);
    } else if (arg == "--height") {
      const char* value = next("--height");
      if (value == nullptr) return false;
      options.height = std::atoi(value);
    } else if (arg == "--fps") {
      const char* value = next("--fps");
      if (value == nullptr) return false;
      options.fps = std::atoi(value);
    } else if (arg == "--shm") {
      const char* value = next("--shm");
      if (value == nullptr) return false;
      options.shmName = widen(value);
    } else if (arg == "--dump-bmp") {
      const char* value = next("--dump-bmp");
      if (value == nullptr) return false;
      options.dumpBmpPath = widen(value);
    } else if (arg == "--dump-after-ms") {
      const char* value = next("--dump-after-ms");
      if (value == nullptr) return false;
      options.dumpAfterMs = std::atoi(value);
    } else if (arg == "--exit-after-ms") {
      const char* value = next("--exit-after-ms");
      if (value == nullptr) return false;
      options.exitAfterMs = std::atoi(value);
    } else if (arg == "--onscreen") {
      options.onscreen = true;
    } else {
      error = "Unknown argument '" + arg + "'.";
      return false;
    }
  }
  if (options.url.empty()) {
    error = "--url is required.";
    return false;
  }
  if (options.width < 16 || options.width > 4096 || options.height < 16 || options.height > 4096) {
    error = "--width/--height must be within 16..4096.";
    return false;
  }
  if (options.fps < 1 || options.fps > 60) {
    error = "--fps must be within 1..60.";
    return false;
  }
  return true;
}

// Shared-memory writer (seqlock producer). Created before the WebView so the core
// can map the buffer as soon as it exists.
class ShmWriter {
 public:
  bool create(const std::wstring& name, int width, int height) {
    if (name.empty()) {
      return true;  // self-test without --shm: capture still runs, nothing published
    }
    bytes_ = corevideo::modules::browsershm::mappingBytes(width, height);
    mapping_ = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE,
                                  static_cast<DWORD>(bytes_ >> 32),
                                  static_cast<DWORD>(bytes_ & 0xffffffffu), name.c_str());
    if (mapping_ == nullptr) {
      std::fprintf(stderr, "[browser-host] FATAL CreateFileMapping('%ls') failed (err=%lu)\n",
                   name.c_str(), GetLastError());
      return false;
    }
    view_ = static_cast<uint8_t*>(MapViewOfFile(mapping_, FILE_MAP_WRITE, 0, 0, bytes_));
    if (view_ == nullptr) {
      std::fprintf(stderr, "[browser-host] FATAL MapViewOfFile('%ls') failed (err=%lu)\n",
                   name.c_str(), GetLastError());
      return false;
    }
    std::memset(view_, 0, corevideo::modules::browsershm::kHeaderBytes);
    return true;
  }

  void publish(const uint8_t* bgra, int width, int height) {
    if (view_ == nullptr) {
      return;
    }
    corevideo::modules::browsershm::writeFrame(view_, bytes_, bgra, width, height);
  }

  ~ShmWriter() {
    if (view_ != nullptr) {
      UnmapViewOfFile(view_);
    }
    if (mapping_ != nullptr) {
      CloseHandle(mapping_);
    }
  }

 private:
  HANDLE mapping_ = nullptr;
  uint8_t* view_ = nullptr;
  std::size_t bytes_ = 0;
};

// WGC self-capture of our own window -> fixed-size BGRA buffer -> SHM publish.
// Same teardown-drain discipline as the core's WgcSession (frameMutex_ held across
// onFrame; stop() drains an in-flight callback before freeing D3D members).
class SelfCapture {
 public:
  bool start(HWND hwnd, int width, int height, int fps, ShmWriter* writer) {
    width_ = width;
    height_ = height;
    writer_ = writer;
    frameIntervalMs_ = 1000 / (fps > 0 ? fps : 30);
    frame_.assign(static_cast<std::size_t>(width_) * height_ * 4, 0);

    if (FAILED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr,
                                 D3D11_CREATE_DEVICE_BGRA_SUPPORT, nullptr, 0, D3D11_SDK_VERSION,
                                 device_.GetAddressOf(), nullptr, context_.GetAddressOf()))) {
      std::fprintf(stderr, "[browser-host] FATAL D3D11CreateDevice failed\n");
      return false;
    }
    ComPtr<IDXGIDevice> dxgiDevice;
    if (FAILED(device_.As(&dxgiDevice))) {
      return false;
    }
    winrt::com_ptr<IInspectable> inspectable;
    if (FAILED(CreateDirect3D11DeviceFromDXGIDevice(
            dxgiDevice.Get(), reinterpret_cast<::IInspectable**>(winrt::put_abi(inspectable))))) {
      std::fprintf(stderr, "[browser-host] FATAL CreateDirect3D11DeviceFromDXGIDevice failed\n");
      return false;
    }
    const auto winrtDevice = inspectable.as<wgd::Direct3D11::IDirect3DDevice>();

    auto interop =
        winrt::get_activation_factory<wgc::GraphicsCaptureItem>().as<IGraphicsCaptureItemInterop>();
    wgc::GraphicsCaptureItem item{nullptr};
    if (FAILED(interop->CreateForWindow(hwnd, winrt::guid_of<wgc::GraphicsCaptureItem>(),
                                        winrt::put_abi(item)))) {
      std::fprintf(stderr, "[browser-host] FATAL GraphicsCaptureItem CreateForWindow failed\n");
      return false;
    }
    framePool_ = wgc::Direct3D11CaptureFramePool::CreateFreeThreaded(
        winrtDevice, wgd::DirectXPixelFormat::B8G8R8A8UIntNormalized, 2, item.Size());
    frameArrived_ = framePool_.FrameArrived(
        winrt::auto_revoke,
        [this](const wgc::Direct3D11CaptureFramePool& pool, const auto&) { onFrame(pool); });
    session_ = framePool_.CreateCaptureSession(item);
    try {
      session_.IsBorderRequired(false);  // no yellow capture border (Win10 21H1+)
    } catch (...) {
    }
    try {
      session_.IsCursorCaptureEnabled(false);
    } catch (...) {
    }
    session_.StartCapture();
    running_.store(true, std::memory_order_release);
    return true;
  }

  void stop() {
    running_.store(false, std::memory_order_release);
    if (frameArrived_) {
      frameArrived_.revoke();
    }
    { std::lock_guard<std::mutex> drain(frameMutex_); }
    if (session_) {
      session_.Close();
      session_ = nullptr;
    }
    if (framePool_) {
      framePool_.Close();
      framePool_ = nullptr;
    }
  }

  [[nodiscard]] int64_t framesPublished() const {
    return framesPublished_.load(std::memory_order_relaxed);
  }

  // Copy of the latest published frame (self-test dump).
  bool latestFrame(std::vector<uint8_t>& outBgra, int& outWidth, int& outHeight) {
    std::lock_guard<std::mutex> lock(latestMutex_);
    if (framesPublished_.load(std::memory_order_relaxed) == 0) {
      return false;
    }
    outBgra = frame_;
    outWidth = width_;
    outHeight = height_;
    return true;
  }

 private:
  void onFrame(const wgc::Direct3D11CaptureFramePool& pool) {
    std::lock_guard<std::mutex> frameLock(frameMutex_);
    if (!running_.load(std::memory_order_acquire)) {
      return;
    }
    auto frame = pool.TryGetNextFrame();
    if (!frame) {
      return;
    }
    // Pace to the commanded fps: WGC delivers on every content change; the core
    // samples latest-wins, so publishing faster than fps is pure waste.
    const int64_t now = nowMs();
    if (now - lastPublishMs_ < frameIntervalMs_) {
      return;
    }
    auto surface = frame.Surface();
    winrt::com_ptr<::Windows::Graphics::DirectX::Direct3D11::IDirect3DDxgiInterfaceAccess> access;
    if (!surface || FAILED(winrt::get_unknown(surface)->QueryInterface(
                        winrt::guid_of<::Windows::Graphics::DirectX::Direct3D11::
                                           IDirect3DDxgiInterfaceAccess>(),
                        access.put_void()))) {
      return;
    }
    ComPtr<ID3D11Texture2D> texture;
    if (FAILED(access->GetInterface(IID_PPV_ARGS(texture.GetAddressOf())))) {
      return;
    }
    D3D11_TEXTURE2D_DESC desc{};
    texture->GetDesc(&desc);
    if (staging_ == nullptr || stagingWidth_ != static_cast<int>(desc.Width) ||
        stagingHeight_ != static_cast<int>(desc.Height)) {
      D3D11_TEXTURE2D_DESC stagingDesc = desc;
      stagingDesc.Usage = D3D11_USAGE_STAGING;
      stagingDesc.BindFlags = 0;
      stagingDesc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
      stagingDesc.MiscFlags = 0;
      staging_.Reset();
      if (FAILED(device_->CreateTexture2D(&stagingDesc, nullptr, staging_.GetAddressOf()))) {
        return;
      }
      stagingWidth_ = static_cast<int>(desc.Width);
      stagingHeight_ = static_cast<int>(desc.Height);
    }
    context_->CopyResource(staging_.Get(), texture.Get());
    D3D11_MAPPED_SUBRESOURCE mapped{};
    if (FAILED(context_->Map(staging_.Get(), 0, D3D11_MAP_READ, 0, &mapped))) {
      return;
    }
    // The captured texture is the window's composed content; crop/clamp into our
    // fixed width_ x height_ buffer (rows beyond the capture stay transparent).
    const int copyWidth = stagingWidth_ < width_ ? stagingWidth_ : width_;
    const int copyHeight = stagingHeight_ < height_ ? stagingHeight_ : height_;
    {
      std::lock_guard<std::mutex> lock(latestMutex_);
      const auto* src = static_cast<const uint8_t*>(mapped.pData);
      for (int row = 0; row < copyHeight; ++row) {
        std::memcpy(frame_.data() + static_cast<std::size_t>(row) * width_ * 4,
                    src + static_cast<std::size_t>(row) * mapped.RowPitch,
                    static_cast<std::size_t>(copyWidth) * 4);
      }
      context_->Unmap(staging_.Get(), 0);
      if (writer_ != nullptr) {
        writer_->publish(frame_.data(), width_, height_);
      }
      framesPublished_.fetch_add(1, std::memory_order_relaxed);
      lastPublishMs_ = now;
    }
  }

  static int64_t nowMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
  }

  ComPtr<ID3D11Device> device_;
  ComPtr<ID3D11DeviceContext> context_;
  ComPtr<ID3D11Texture2D> staging_;
  int stagingWidth_ = 0;
  int stagingHeight_ = 0;
  wgc::Direct3D11CaptureFramePool framePool_{nullptr};
  wgc::GraphicsCaptureSession session_{nullptr};
  wgc::Direct3D11CaptureFramePool::FrameArrived_revoker frameArrived_;
  std::atomic<bool> running_{false};
  std::mutex frameMutex_;   // serializes onFrame against stop()
  std::mutex latestMutex_;  // guards frame_ (publish vs self-test read)
  std::vector<uint8_t> frame_;
  int width_ = 0;
  int height_ = 0;
  ShmWriter* writer_ = nullptr;
  int64_t frameIntervalMs_ = 33;
  int64_t lastPublishMs_ = 0;
  std::atomic<int64_t> framesPublished_{0};
};

struct App {
  Options options;
  HWND hwnd = nullptr;
  ShmWriter shm;
  SelfCapture capture;
  ComPtr<ICoreWebView2Controller> controller;
  ComPtr<ICoreWebView2> webview;
  bool captureStarted = false;
  int exitCode = 0;
  int64_t lastStatsFrames = 0;
};

App* g_app = nullptr;

void dumpBmpAndStats(App& app) {
  std::vector<uint8_t> bgra;
  int width = 0;
  int height = 0;
  if (!app.capture.latestFrame(bgra, width, height)) {
    std::fprintf(stderr, "[browser-host] DUMP no frame captured yet\n");
    return;
  }

  // Machine-checkable pixel statistics.
  uint64_t sum[4] = {0, 0, 0, 0};
  uint64_t nonzero = 0;
  const std::size_t pixels = static_cast<std::size_t>(width) * height;
  for (std::size_t i = 0; i < pixels; ++i) {
    const uint8_t* p = bgra.data() + i * 4;
    sum[0] += p[0];
    sum[1] += p[1];
    sum[2] += p[2];
    sum[3] += p[3];
    if (p[0] != 0 || p[1] != 0 || p[2] != 0) {
      ++nonzero;
    }
  }
  auto sample = [&](int x, int y) {
    const uint8_t* p = bgra.data() + (static_cast<std::size_t>(y) * width + x) * 4;
    std::fprintf(stderr, "[browser-host] DUMP sample (%d,%d) BGRA=%u,%u,%u,%u\n", x, y, p[0], p[1],
                 p[2], p[3]);
  };
  std::fprintf(stderr,
               "[browser-host] DUMP %dx%d nonzeroPixels=%llu/%llu meanBGRA=%.1f,%.1f,%.1f,%.1f\n",
               width, height, static_cast<unsigned long long>(nonzero),
               static_cast<unsigned long long>(pixels), sum[0] / double(pixels),
               sum[1] / double(pixels), sum[2] / double(pixels), sum[3] / double(pixels));
  sample(width / 4, height / 2);
  sample(width / 2, height / 2);
  sample(3 * width / 4, height / 2);

  if (app.options.dumpBmpPath.empty()) {
    return;
  }
  // 32bpp top-down BMP (alpha bytes preserved in the file for inspection).
  BITMAPFILEHEADER fileHeader{};
  BITMAPINFOHEADER infoHeader{};
  const DWORD imageBytes = static_cast<DWORD>(bgra.size());
  fileHeader.bfType = 0x4D42;
  fileHeader.bfOffBits = sizeof(fileHeader) + sizeof(infoHeader);
  fileHeader.bfSize = fileHeader.bfOffBits + imageBytes;
  infoHeader.biSize = sizeof(infoHeader);
  infoHeader.biWidth = width;
  infoHeader.biHeight = -height;  // top-down
  infoHeader.biPlanes = 1;
  infoHeader.biBitCount = 32;
  infoHeader.biCompression = BI_RGB;
  infoHeader.biSizeImage = imageBytes;
  std::ofstream out(app.options.dumpBmpPath.c_str(), std::ios::binary);
  out.write(reinterpret_cast<const char*>(&fileHeader), sizeof(fileHeader));
  out.write(reinterpret_cast<const char*>(&infoHeader), sizeof(infoHeader));
  out.write(reinterpret_cast<const char*>(bgra.data()), static_cast<std::streamsize>(bgra.size()));
  std::fprintf(stderr, "[browser-host] DUMP wrote %ls\n", app.options.dumpBmpPath.c_str());
}

LRESULT CALLBACK wndProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam) {
  App* app = g_app;
  switch (message) {
    case kMsgReload:
      if (app != nullptr && app->webview != nullptr) {
        std::fprintf(stderr, "[browser-host] reload requested\n");
        app->webview->Reload();
      }
      return 0;
    case kMsgQuit:
      DestroyWindow(hwnd);
      return 0;
    case WM_TIMER:
      if (app != nullptr && wParam == kTimerDump) {
        KillTimer(hwnd, kTimerDump);
        dumpBmpAndStats(*app);
        return 0;
      }
      if (app != nullptr && wParam == kTimerExit) {
        KillTimer(hwnd, kTimerExit);
        DestroyWindow(hwnd);
        return 0;
      }
      if (app != nullptr && wParam == kTimerStats) {
        const int64_t frames = app->capture.framesPublished();
        std::fprintf(stderr, "[browser-host] capture fps=%.1f (frames=%lld)\n",
                     (frames - app->lastStatsFrames) / 5.0, static_cast<long long>(frames));
        app->lastStatsFrames = frames;
        return 0;
      }
      break;
    case WM_CLOSE:
      DestroyWindow(hwnd);
      return 0;
    case WM_DESTROY:
      PostQuitMessage(app != nullptr ? app->exitCode : 0);
      return 0;
    default:
      break;
  }
  return DefWindowProcW(hwnd, message, wParam, lParam);
}

std::wstring userDataFolder() {
  wchar_t buffer[MAX_PATH] = {};
  const DWORD length = GetEnvironmentVariableW(L"LOCALAPPDATA", buffer, MAX_PATH);
  std::wstring base = length > 0 && length < MAX_PATH ? buffer : L"C:\\Windows\\Temp";
  return base + L"\\CoreVideoPro\\browser-host";
}

HRESULT configureWebView(App& app) {
  ComPtr<ICoreWebView2Settings> settings;
  if (SUCCEEDED(app.webview->get_Settings(settings.GetAddressOf())) && settings != nullptr) {
    settings->put_AreDevToolsEnabled(FALSE);
    settings->put_AreDefaultContextMenusEnabled(FALSE);
    settings->put_IsStatusBarEnabled(FALSE);
    settings->put_IsZoomControlEnabled(FALSE);
    settings->put_AreDefaultScriptDialogsEnabled(TRUE);
  }

  // Popups: deny (render-only source; a page must never open real windows).
  app.webview->add_NewWindowRequested(
      Callback<ICoreWebView2NewWindowRequestedEventHandler>(
          [](ICoreWebView2*, ICoreWebView2NewWindowRequestedEventArgs* args) -> HRESULT {
            args->put_Handled(TRUE);
            std::fprintf(stderr, "[browser-host] popup DENIED\n");
            return S_OK;
          })
          .Get(),
      nullptr);

  // Downloads: cancel (ICoreWebView2_4+; older runtimes simply skip this hardening).
  ComPtr<ICoreWebView2_4> webview4;
  if (SUCCEEDED(app.webview.As(&webview4)) && webview4 != nullptr) {
    webview4->add_DownloadStarting(
        Callback<ICoreWebView2DownloadStartingEventHandler>(
            [](ICoreWebView2*, ICoreWebView2DownloadStartingEventArgs* args) -> HRESULT {
              args->put_Cancel(TRUE);
              args->put_Handled(TRUE);
              std::fprintf(stderr, "[browser-host] download CANCELLED\n");
              return S_OK;
            })
            .Get(),
        nullptr);
  }

  // Browser/renderer process death -> exit loudly; the core supervisor restarts us
  // with backoff and shows the slate meanwhile.
  app.webview->add_ProcessFailed(
      Callback<ICoreWebView2ProcessFailedEventHandler>(
          [](ICoreWebView2*, ICoreWebView2ProcessFailedEventArgs* args) -> HRESULT {
            COREWEBVIEW2_PROCESS_FAILED_KIND kind{};
            args->get_ProcessFailedKind(&kind);
            std::fprintf(stderr, "[browser-host] FATAL WebView2 process failed (kind=%d) — exiting\n",
                         static_cast<int>(kind));
            if (g_app != nullptr) {
              g_app->exitCode = 2;
              PostMessageW(g_app->hwnd, kMsgQuit, 0, 0);
            }
            return S_OK;
          })
          .Get(),
      nullptr);

  app.webview->add_NavigationCompleted(
      Callback<ICoreWebView2NavigationCompletedEventHandler>(
          [](ICoreWebView2*, ICoreWebView2NavigationCompletedEventArgs* args) -> HRESULT {
            BOOL success = FALSE;
            args->get_IsSuccess(&success);
            COREWEBVIEW2_WEB_ERROR_STATUS status{};
            args->get_WebErrorStatus(&status);
            std::fprintf(stderr, "[browser-host] navigation %s (webErrorStatus=%d)\n",
                         success ? "completed" : "FAILED", static_cast<int>(status));
            return S_OK;
          })
          .Get(),
      nullptr);

  return S_OK;
}

}  // namespace

int main(int argc, char** argv) {
  Options options;
  std::string parseError;
  if (!parseOptions(argc, argv, options, parseError)) {
    std::fprintf(stderr, "[browser-host] FATAL bad arguments: %s\n", parseError.c_str());
    std::fprintf(stderr,
                 "usage: corevideo-browser-host --url <url> [--width N --height N --fps N] "
                 "[--shm Local\\name] [--dump-bmp p.bmp --dump-after-ms N --exit-after-ms N]\n");
    return 1;
  }

  // Exact-pixel rendering: per-monitor DPI aware, so window size == frame size.
  SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
  const HRESULT comInit = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
  if (FAILED(comInit)) {
    std::fprintf(stderr, "[browser-host] FATAL CoInitializeEx failed (0x%08lx)\n", comInit);
    return 1;
  }

  // WebView2 Runtime present? Fail LOUDLY if not (owner installs the evergreen
  // runtime once; it ships with Windows 11).
  {
    wchar_t* version = nullptr;
    const HRESULT probed = GetAvailableCoreWebView2BrowserVersionString(nullptr, &version);
    if (FAILED(probed) || version == nullptr) {
      std::fprintf(stderr,
                   "[browser-host] FATAL WebView2 Runtime is NOT installed (hr=0x%08lx). Install "
                   "the Evergreen WebView2 Runtime: https://developer.microsoft.com/microsoft-edge/webview2/\n",
                   probed);
      return 3;
    }
    std::fprintf(stderr, "[browser-host] WebView2 Runtime %ls\n", version);
    CoTaskMemFree(version);
  }

  App app;
  app.options = options;
  g_app = &app;

  if (!app.shm.create(options.shmName, options.width, options.height)) {
    return 4;
  }

  WNDCLASSW windowClass{};
  windowClass.lpfnWndProc = wndProc;
  windowClass.hInstance = GetModuleHandleW(nullptr);
  windowClass.lpszClassName = L"CoreVideoBrowserHost";
  RegisterClassW(&windowClass);

  // Off the virtual desktop, tool-window (no Alt-Tab entry), never activated.
  // Chromium's occlusion tracker is disabled via browser args below so the page
  // keeps rendering at full rate even though nobody can see this window.
  const int virtualRight =
      GetSystemMetrics(SM_XVIRTUALSCREEN) + GetSystemMetrics(SM_CXVIRTUALSCREEN);
  const int x = options.onscreen ? 64 : virtualRight + 256;
  const int y = options.onscreen ? 64 : 64;
  app.hwnd = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE, windowClass.lpszClassName,
                             L"CoreVideo Browser Source", WS_POPUP, x, y, options.width,
                             options.height, nullptr, nullptr, windowClass.hInstance, nullptr);
  if (app.hwnd == nullptr) {
    std::fprintf(stderr, "[browser-host] FATAL CreateWindowExW failed (err=%lu)\n", GetLastError());
    return 1;
  }
  ShowWindow(app.hwnd, SW_SHOWNOACTIVATE);

  // stdin command channel: "reload\n" reloads; EOF (core died / source removed)
  // exits. Detached reader thread; the pipe close unblocks getline.
  std::thread stdinThread([hwnd = app.hwnd] {
    std::string line;
    while (std::getline(std::cin, line)) {
      while (!line.empty() && (line.back() == '\r' || line.back() == '\n')) {
        line.pop_back();
      }
      if (line == "reload") {
        PostMessageW(hwnd, kMsgReload, 0, 0);
      } else if (line == "quit") {
        PostMessageW(hwnd, kMsgQuit, 0, 0);
      }
    }
    std::fprintf(stderr, "[browser-host] stdin closed — exiting\n");
    PostMessageW(hwnd, kMsgQuit, 0, 0);
  });
  stdinThread.detach();

  // WebView2 environment: occlusion tracking OFF (offscreen window must keep
  // rendering), background throttling OFF (overlays animate at full rate).
  auto envOptions = Microsoft::WRL::Make<CoreWebView2EnvironmentOptions>();
  envOptions->put_AdditionalBrowserArguments(
      L"--disable-features=CalculateNativeWinOcclusion,IntensiveWakeUpThrottling "
      L"--disable-background-timer-throttling --disable-renderer-backgrounding");

  const std::wstring dataFolder = userDataFolder();
  HRESULT createHr = CreateCoreWebView2EnvironmentWithOptions(
      nullptr, dataFolder.c_str(), envOptions.Get(),
      Callback<ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler>(
          [&app](HRESULT result, ICoreWebView2Environment* environment) -> HRESULT {
            if (FAILED(result) || environment == nullptr) {
              std::fprintf(stderr,
                           "[browser-host] FATAL CreateCoreWebView2Environment failed (0x%08lx)\n",
                           result);
              app.exitCode = 3;
              PostMessageW(app.hwnd, kMsgQuit, 0, 0);
              return S_OK;
            }
            environment->CreateCoreWebView2Controller(
                app.hwnd,
                Callback<ICoreWebView2CreateCoreWebView2ControllerCompletedHandler>(
                    [&app](HRESULT result, ICoreWebView2Controller* controller) -> HRESULT {
                      if (FAILED(result) || controller == nullptr) {
                        std::fprintf(
                            stderr,
                            "[browser-host] FATAL CreateCoreWebView2Controller failed (0x%08lx)\n",
                            result);
                        app.exitCode = 3;
                        PostMessageW(app.hwnd, kMsgQuit, 0, 0);
                        return S_OK;
                      }
                      app.controller = controller;
                      app.controller->get_CoreWebView2(app.webview.GetAddressOf());

                      // Transparent page background -> real BGRA alpha.
                      ComPtr<ICoreWebView2Controller2> controller2;
                      if (SUCCEEDED(app.controller.As(&controller2)) && controller2 != nullptr) {
                        COREWEBVIEW2_COLOR transparent{0, 0, 0, 0};
                        controller2->put_DefaultBackgroundColor(transparent);
                      }
                      // Pin rasterization to 1.0 so CSS pixels == physical pixels
                      // regardless of the monitor the offscreen window nominally
                      // belongs to.
                      ComPtr<ICoreWebView2Controller3> controller3;
                      if (SUCCEEDED(app.controller.As(&controller3)) && controller3 != nullptr) {
                        controller3->put_ShouldDetectMonitorScaleChanges(FALSE);
                        controller3->put_RasterizationScale(1.0);
                        controller3->put_BoundsMode(COREWEBVIEW2_BOUNDS_MODE_USE_RAW_PIXELS);
                      }
                      RECT bounds{0, 0, app.options.width, app.options.height};
                      app.controller->put_Bounds(bounds);
                      app.controller->put_IsVisible(TRUE);

                      configureWebView(app);
                      app.webview->Navigate(app.options.url.c_str());
                      std::fprintf(stderr, "[browser-host] navigating %dx%d@%dfps\n",
                                   app.options.width, app.options.height, app.options.fps);

                      // Start the self-capture once the WebView exists (its child
                      // windows are part of our window's composition tree).
                      if (!app.captureStarted) {
                        app.captureStarted = app.capture.start(app.hwnd, app.options.width,
                                                               app.options.height, app.options.fps,
                                                               &app.shm);
                        if (!app.captureStarted) {
                          std::fprintf(stderr,
                                       "[browser-host] FATAL self-capture failed to start\n");
                          app.exitCode = 5;
                          PostMessageW(app.hwnd, kMsgQuit, 0, 0);
                        }
                      }
                      return S_OK;
                    })
                    .Get());
            return S_OK;
          })
          .Get());
  if (FAILED(createHr)) {
    std::fprintf(stderr, "[browser-host] FATAL CreateCoreWebView2EnvironmentWithOptions failed (0x%08lx)\n",
                 createHr);
    return 3;
  }

  if (!options.dumpBmpPath.empty()) {
    SetTimer(app.hwnd, kTimerDump, static_cast<UINT>(options.dumpAfterMs), nullptr);
  }
  if (options.exitAfterMs > 0) {
    SetTimer(app.hwnd, kTimerExit, static_cast<UINT>(options.exitAfterMs), nullptr);
  }
  SetTimer(app.hwnd, kTimerStats, 5000, nullptr);

  MSG message{};
  while (GetMessageW(&message, nullptr, 0, 0) > 0) {
    TranslateMessage(&message);
    DispatchMessageW(&message);
  }

  app.capture.stop();
  if (app.controller != nullptr) {
    app.controller->Close();
  }
  g_app = nullptr;
  std::fprintf(stderr, "[browser-host] exit (code=%d, frames=%lld)\n", app.exitCode,
               static_cast<long long>(app.capture.framesPublished()));
  return app.exitCode;
}
