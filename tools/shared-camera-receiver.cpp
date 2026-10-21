// Independent registered OS camera consumer for sessions with another reader.
// Reads delivered SoftwareBitmap pixels, never the publisher mapping or DLL.
#define NOMINMAX
#include <windows.h>
#include <winrt/base.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Media.Capture.h>
#include <winrt/Windows.Media.Capture.Frames.h>
#include <winrt/Windows.Media.MediaProperties.h>
#include <winrt/Windows.Graphics.Imaging.h>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <mutex>
#include <thread>
#include <vector>
#include "modules/DeliveryCounterPattern.h"
using namespace winrt;
using namespace Windows::Media::Capture;
using namespace Windows::Media::Capture::Frames;
using namespace Windows::Graphics::Imaging;
using Clock = std::chrono::steady_clock;
struct __declspec(uuid("5B0D3235-4DBA-4D44-865E-8F1D0E4FD04D")) BitmapBytes : IUnknown {
  virtual HRESULT __stdcall GetBuffer(BYTE** bytes, UINT32* capacity) = 0;
};

int wmain(int argc, wchar_t** argv) {
  const int seconds = argc == 3 ? _wtoi(argv[2]) : 0;
  if (seconds < 1 || seconds > 7500) {
    std::fprintf(stderr, "Usage: corevideo-shared-camera-receiver REGISTERED_DEVICE_ID SECONDS (1..7500)\n");
    return 2;
  }
  const auto deadline = Clock::now() + std::chrono::seconds(seconds + 30);
  std::jthread watchdog([deadline](std::stop_token stop) {
    while (!stop.stop_requested()) {
      if (Clock::now() > deadline) { std::fprintf(stderr, "Receiver timeout; capture incomplete.\n"); ExitProcess(3); }
      std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
  });
  struct Row { int64_t arrival, host, pts, identity; };
  std::vector<Row> rows;
  const size_t capacity = static_cast<size_t>(seconds) * 64 + 128;
  rows.reserve(capacity);
  std::mutex mutex;
  std::atomic<unsigned> failures{0};
  bool complete = false;
  try {
    init_apartment(apartment_type::multi_threaded);
    MediaCapture capture;
    MediaCaptureInitializationSettings settings;
    settings.VideoDeviceId(argv[1]);
    settings.SharingMode(MediaCaptureSharingMode::SharedReadOnly);
    settings.StreamingCaptureMode(StreamingCaptureMode::Video);
    settings.MemoryPreference(MediaCaptureMemoryPreference::Cpu);
    capture.InitializeAsync(settings).get();
    MediaFrameSource source{nullptr};
    for (auto pair : capture.FrameSources())
      if (pair.Value().Info().SourceKind() == MediaFrameSourceKind::Color) { source = pair.Value(); break; }
    if (!source) throw hresult_error(E_FAIL, L"No registered color frame source");
    auto format = source.CurrentFormat();
    const auto rate = format.FrameRate();
    const auto start = Clock::now();
    const auto unixUs = std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    std::printf("{\"schema\":\"camera-pixel-receiver-v1\",\"receiverMode\":\"os-camera\",\"activation\":\"shared-readonly\",\"receiverScheduling\":\"ordinary\",\"startUnixUs\":%lld,\"width\":%u,\"height\":%u,\"fpsNumerator\":%u,\"fpsDenominator\":%u}\n",
        unixUs, format.VideoFormat().Width(), format.VideoFormat().Height(), rate.Numerator(), rate.Denominator());
    // Setup evidence is available before the live interval. Keep per-frame
    // rows buffered so console/file I/O does not run on the measured cadence.
    std::fflush(stdout);
    auto reader = capture.CreateFrameReaderAsync(source,
        Windows::Media::MediaProperties::MediaEncodingSubtypes::Nv12()).get();
    // Realtime/Latest drops are deliberately visible as missing pixel identities.
    reader.AcquisitionMode(MediaFrameReaderAcquisitionMode::Realtime);
    auto token = reader.FrameArrived([&](MediaFrameReader const& sender, auto const&) {
      try {
        // Serialize callback acquisitions as well as rows to preserve observation order.
        std::lock_guard lock(mutex);
        auto frame = sender.TryAcquireLatestFrame();
        if (!frame || !frame.VideoMediaFrame()) return;
        const auto arrived = Clock::now();
        auto bitmap = frame.VideoMediaFrame().SoftwareBitmap();
        if (!bitmap || bitmap.BitmapPixelFormat() != BitmapPixelFormat::Nv12 || !frame.SystemRelativeTime()) {
          ++failures; return;
        }
        auto buffer = bitmap.LockBuffer(BitmapBufferAccessMode::Read);
        auto reference = buffer.CreateReference();
        BYTE* bytes = nullptr; UINT32 length = 0;
        check_hresult(reference.as<BitmapBytes>()->GetBuffer(&bytes, &length));
        auto plane = buffer.GetPlaneDescription(0);
        if (plane.StartIndex < 0 || static_cast<UINT32>(plane.StartIndex) > length) { ++failures; return; }
        auto identity = corevideo::modules::decodeDeliveryCounter(bytes + plane.StartIndex,
            length - plane.StartIndex, bitmap.PixelWidth(), bitmap.PixelHeight(), plane.Stride);
        if (rows.size() >= capacity) { ++failures; return; }
        rows.push_back({std::chrono::duration_cast<std::chrono::microseconds>(arrived - start).count(),
            std::chrono::duration_cast<std::chrono::microseconds>(arrived.time_since_epoch()).count(),
            frame.SystemRelativeTime().Value().count(), identity ? static_cast<int64_t>(*identity) : -1});
        if (rows.size() == 1) {
          std::fprintf(stderr, "[receiver] first-frame identity=%lld\n", rows.back().identity);
          std::fflush(stderr);
        }
      } catch (hresult_error const&) { ++failures; }
    });
    if (reader.StartAsync().get() != MediaFrameReaderStartStatus::Success)
      throw hresult_error(E_FAIL, L"OS frame reader start failed");
    std::this_thread::sleep_for(std::chrono::seconds(seconds));
    reader.StopAsync().get();
    reader.FrameArrived(token);
    reader.Close(); capture.Close();
    complete = true;
  } catch (hresult_error const& error) {
    ++failures;
    std::fprintf(stderr, "OS shared camera error 0x%08x %ls\n", (unsigned)error.code(), error.message().c_str());
  }
  std::lock_guard lock(mutex);
  size_t number = 0;
  for (const auto& row : rows) {
    std::printf("{\"sample\":%zu,\"arrivalUs\":%lld,\"arrivalHostUs\":%lld,\"pts100ns\":%lld,\"identity\":",
        ++number, row.arrival, row.host, row.pts);
    if (row.identity < 0) std::printf("null"); else std::printf("%lld", row.identity);
    std::printf("}\n");
  }
  std::printf("{\"complete\":%s,\"samples\":%zu,\"frameFailures\":%u}\n",
      complete && failures == 0 ? "true" : "false", rows.size(), failures.load());
  return complete && failures == 0 && !rows.empty() ? 0 : 4;
}
