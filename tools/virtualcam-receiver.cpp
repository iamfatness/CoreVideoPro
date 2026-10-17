// Independent OS receiver. Reads the registered camera through Media Foundation,
// never the publisher mapping. stdout is bounded-duration NDJSON evidence.
#define NOMINMAX
#include <windows.h>
#include <avrt.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <wrl/client.h>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>
#include "modules/DeliveryCounterPattern.h"
using Microsoft::WRL::ComPtr;
using Clock = std::chrono::steady_clock;
static void check(HRESULT hr, const char* operation) {
  if (FAILED(hr)) { char text[160]; std::snprintf(text, sizeof(text), "%s failed: 0x%08lx", operation, static_cast<unsigned long>(hr)); throw std::runtime_error(text); }
}
int main(int argc, char** argv) {
  bool failFast = false, multimediaScheduling = false, validOptions = true;
  const char* dllPath = nullptr;
  for (int i = 2; i < argc; ++i) {
    const std::string option(argv[i]);
    if (option == "--fail-fast") failFast = true;
    else if (option == "--mmcss") multimediaScheduling = true;
    else if (option == "--dll" && i + 1 < argc) dllPath = argv[++i];
    else validOptions = false;
  }
  const bool direct = dllPath != nullptr;
  const int seconds = argc >= 2 && validOptions ? std::atoi(argv[1]) : 0;
  if (seconds < 1 || seconds > 7500) { std::fprintf(stderr, "Usage: corevideo-vcam-receiver SECONDS (1..7500) [--dll ABSOLUTE_PATH] [--fail-fast] [--mmcss]\n"); return 2; }
  const auto deadline = Clock::now() + std::chrono::seconds(seconds + 30);
  std::jthread watchdog([deadline](std::stop_token stop) {
    while (!stop.stop_requested()) {
      if (Clock::now() > deadline) {
        std::fprintf(stderr, "Receiver exceeded bounded capture deadline; evidence incomplete.\n");
        std::fflush(nullptr); ExitProcess(3);
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
  });
  int result = 0;
  struct Observation { uint64_t sample; int64_t arrival, host, pts, identity; };
  // Keep evidence I/O off the capture thread. A redirected printf can block
  // on disk/pipe backpressure and create the receiver stall we are measuring.
  // The two-hour plus warmup CLI limit bounds this at ~19 MiB; never overwrite old rows.
  std::vector<Observation> observations;
  const size_t maximumObservations = static_cast<size_t>(seconds) * 64 + 128;
  bool captureComplete = false;
  bool continuityFailed = false;
  try {
    struct Scheduling {
      HANDLE task = nullptr;
      ~Scheduling() { if (task) AvRevertMmThreadCharacteristics(task); }
    } scheduling;
    if (multimediaScheduling) {
      DWORD index = 0;
      scheduling.task = AvSetMmThreadCharacteristicsW(L"Playback", &index);
      check(scheduling.task ? S_OK : HRESULT_FROM_WIN32(GetLastError()), "receiver MMCSS registration");
      check(AvSetMmThreadPriority(scheduling.task, AVRT_PRIORITY_HIGH) ? S_OK : HRESULT_FROM_WIN32(GetLastError()), "receiver MMCSS priority");
    }
    observations.reserve(maximumObservations);
    struct StopEvent {
      HANDLE handle = nullptr;
      ~StopEvent() { if (handle) CloseHandle(handle); }
    } stopEvent;
    const auto stopName = L"Local\\CoreVideoPixelProbeStop-" + std::to_wstring(GetCurrentProcessId());
    stopEvent.handle = CreateEventW(nullptr, TRUE, FALSE, stopName.c_str());
    check(stopEvent.handle ? S_OK : HRESULT_FROM_WIN32(GetLastError()), "receiver stop event");
    check(CoInitializeEx(nullptr, COINIT_MULTITHREADED), "CoInitializeEx");
    check(MFStartup(MF_VERSION), "MFStartup");
    {
      struct Library { HMODULE handle = nullptr; ~Library() { if (handle) FreeLibrary(handle); } } library;
      ComPtr<IMFActivate> selected;
      if (direct) {
        library.handle = LoadLibraryExA(dllPath, nullptr, LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32);
        if (!library.handle) throw std::runtime_error("Direct diagnostic DLL load failed; use an absolute path.");
        using GetFactory = HRESULT(STDAPICALLTYPE*)(REFCLSID, REFIID, void**);
        const auto factoryFunction = reinterpret_cast<GetFactory>(GetProcAddress(library.handle, "DllGetClassObject"));
        if (!factoryFunction) throw std::runtime_error("Diagnostic DLL has no COM factory.");
        CLSID clsid{};
        check(CLSIDFromString(L"{8B4B2C9E-2C4A-4E1D-9C7A-CDEF01234567}", &clsid), "source CLSID");
        ComPtr<IClassFactory> factory;
        check(factoryFunction(clsid, IID_PPV_ARGS(&factory)), "direct DLL factory");
        check(factory->CreateInstance(nullptr, IID_PPV_ARGS(&selected)), "direct DLL activator");
      } else {
      ComPtr<IMFAttributes> attributes;
      check(MFCreateAttributes(&attributes, 1), "attributes");
      check(attributes->SetGUID(MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE, MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_GUID), "source type");
      IMFActivate** devices = nullptr; UINT32 count = 0;
      check(MFEnumDeviceSources(attributes.Get(), &devices, &count), "enumeration");
      for (UINT32 i = 0; i < count; ++i) {
        wchar_t* name = nullptr; UINT32 length = 0;
        if (SUCCEEDED(devices[i]->GetAllocatedString(MF_DEVSOURCE_ATTRIBUTE_FRIENDLY_NAME, &name, &length)) && name &&
            std::wstring(name, length).find(L"CoreVideo Pro Camera") != std::wstring::npos) selected = devices[i];
        CoTaskMemFree(name); devices[i]->Release();
      }
      CoTaskMemFree(devices);
      if (!selected) throw std::runtime_error("CoreVideo Pro Camera was not enumerated.");
      }
      ComPtr<IMFMediaSource> source;
      check(selected->ActivateObject(IID_PPV_ARGS(&source)), "camera activation");
      ComPtr<IMFSourceReader> reader;
      struct CloseSource {
        ComPtr<IMFSourceReader>& reader; ComPtr<IMFMediaSource>& source;
        ~CloseSource() { reader.Reset(); if (source) source->Shutdown(); }
      } closeSource{reader, source};
      check(MFCreateSourceReaderFromMediaSource(source.Get(), nullptr, &reader), "source reader");
      ComPtr<IMFMediaType> wanted;
      check(MFCreateMediaType(&wanted), "media type");
      check(wanted->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video), "video type");
      check(wanted->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_NV12), "NV12 type");
      check(MFSetAttributeSize(wanted.Get(), MF_MT_FRAME_SIZE, 1920, 1080), "size");
      check(MFSetAttributeRatio(wanted.Get(), MF_MT_FRAME_RATE, 60, 1), "fps");
      check(reader->SetCurrentMediaType(MF_SOURCE_READER_FIRST_VIDEO_STREAM, nullptr, wanted.Get()), "negotiate 1080p60 NV12");
      ComPtr<IMFMediaType> actual;
      check(reader->GetCurrentMediaType(MF_SOURCE_READER_FIRST_VIDEO_STREAM, &actual), "actual type");
      UINT32 width = 0, height = 0, numerator = 0, denominator = 0;
      check(MFGetAttributeSize(actual.Get(), MF_MT_FRAME_SIZE, &width, &height), "actual size");
      check(MFGetAttributeRatio(actual.Get(), MF_MT_FRAME_RATE, &numerator, &denominator), "actual fps");
      if (width != 1920 || height != 1080 || numerator != 60 || denominator != 1) throw std::runtime_error("Negotiated format differs from 1080p60.");
      const auto start = Clock::now(); uint64_t samples = 0;
      // Anchor receiver-monotonic arrivals to UTC so a different camera run
      // cannot be substituted for the measured full-workload interval.
      const auto startUnixUs = std::chrono::duration_cast<std::chrono::microseconds>(
          std::chrono::system_clock::now().time_since_epoch()).count();
      std::printf("{\"schema\":\"camera-pixel-receiver-v1\",\"receiverMode\":\"%s\",\"width\":%u,\"height\":%u,\"fpsNumerator\":%u,\"fpsDenominator\":%u,\"startUnixUs\":%lld,\"receiverScheduling\":\"%s\",\"receiverThreadId\":%lu,\"failFast\":%s}\n", direct ? "direct-dll" : "os-camera", width, height, numerator, denominator, startUnixUs, multimediaScheduling ? "Playback" : "ordinary", GetCurrentThreadId(), failFast ? "true" : "false");
      while (Clock::now() - start < std::chrono::seconds(seconds) &&
             WaitForSingleObject(stopEvent.handle, 0) != WAIT_OBJECT_0) {
        DWORD flags = 0; LONGLONG pts = 0; ComPtr<IMFSample> sample;
        check(reader->ReadSample(MF_SOURCE_READER_FIRST_VIDEO_STREAM, 0, nullptr, &flags, &pts, &sample), "ReadSample");
        if (flags & (MF_SOURCE_READERF_ERROR | MF_SOURCE_READERF_ENDOFSTREAM | MF_SOURCE_READERF_CURRENTMEDIATYPECHANGED)) throw std::runtime_error("Receiver stream error, end or format change.");
        if (!sample) continue;
        const auto arrived = Clock::now();
        const auto arrival = std::chrono::duration_cast<std::chrono::microseconds>(arrived - start).count();
        const auto arrivalHost = std::chrono::duration_cast<std::chrono::microseconds>(arrived.time_since_epoch()).count();
        ComPtr<IMFMediaBuffer> buffer;
        check(sample->ConvertToContiguousBuffer(&buffer), "sample buffer");
        BYTE* bytes = nullptr; DWORD length = 0;
        check(buffer->Lock(&bytes, nullptr, &length), "buffer lock");
        // MF NV12 stride is obtained from the negotiated type, not guessed from
        // the camera's declared width. Packed buffers without the attribute use width.
        UINT32 rawStride = width; (void)actual->GetUINT32(MF_MT_DEFAULT_STRIDE, &rawStride);
        const auto identity = corevideo::modules::decodeDeliveryCounter(bytes, length, width, height, static_cast<LONG>(rawStride));
        check(buffer->Unlock(), "buffer unlock");
        if (observations.size() == maximumObservations) throw std::runtime_error("Receiver evidence capacity exceeded; capture incomplete.");
        observations.push_back({++samples, arrival, arrivalHost, pts, identity ? static_cast<int64_t>(*identity) : -1});
        // Stop after preserving the first failed observation so an external
        // controller can save circular ETW evidence before it rolls over.
        // The ordinary pixel judge still rejects this short/failed capture.
        if (failFast && observations.size() > 1 &&
            observations[observations.size() - 2].arrival >= 30000000) {
          const auto& previous = observations[observations.size() - 2];
          const auto& current = observations.back();
          if (current.identity < 0 || current.identity != previous.identity + 1 ||
              current.arrival - previous.arrival > 33400) {
            continuityFailed = true;
            std::fprintf(stderr, "First receiver failure: sample=%llu identity=%lld previous=%lld intervalUs=%lld hostUs=%lld\n",
                current.sample, current.identity, previous.identity, current.arrival - previous.arrival, current.host);
            break;
          }
        }
      }
      captureComplete = true;
      reader.Reset(); source->Shutdown(); source.Reset(); selected->ShutdownObject();
    }
    MFShutdown(); CoUninitialize();
  } catch (const std::exception& error) { std::fprintf(stderr, "%s\n", error.what()); result = 2; }
  for (const auto& row : observations) {
    std::printf("{\"sample\":%llu,\"arrivalUs\":%lld,\"arrivalHostUs\":%lld,\"pts100ns\":%lld,\"identity\":",
        row.sample, row.arrival, row.host, row.pts);
    if (row.identity >= 0) std::printf("%lld", row.identity); else std::printf("null");
    std::printf("}\n");
  }
  if (captureComplete && result == 0) std::printf("{\"complete\":true,\"samples\":%llu}\n", static_cast<uint64_t>(observations.size()));
  if (std::fflush(stdout) != 0 || std::ferror(stdout)) result = 2;
  watchdog.request_stop();
  return result == 0 && continuityFailed ? 1 : result;
}
