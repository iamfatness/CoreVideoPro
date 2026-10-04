// Independent OS receiver. Reads the registered camera through Media Foundation,
// never the publisher mapping. stdout is bounded-duration NDJSON evidence.
#define NOMINMAX
#include <windows.h>
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
#include "modules/DeliveryCounterPattern.h"
using Microsoft::WRL::ComPtr;
using Clock = std::chrono::steady_clock;
static void check(HRESULT hr, const char* operation) {
  if (FAILED(hr)) { char text[160]; std::snprintf(text, sizeof(text), "%s failed: 0x%08lx", operation, static_cast<unsigned long>(hr)); throw std::runtime_error(text); }
}
int main(int argc, char** argv) {
  const bool direct = argc == 4 && std::string(argv[2]) == "--dll";
  const int seconds = argc == 2 || direct ? std::atoi(argv[1]) : 0;
  if (seconds < 1 || seconds > 7200) { std::fprintf(stderr, "Usage: corevideo-vcam-receiver SECONDS (1..7200) [--dll ABSOLUTE_PATH]\n"); return 2; }
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
  try {
    check(CoInitializeEx(nullptr, COINIT_MULTITHREADED), "CoInitializeEx");
    check(MFStartup(MF_VERSION), "MFStartup");
    {
      struct Library { HMODULE handle = nullptr; ~Library() { if (handle) FreeLibrary(handle); } } library;
      ComPtr<IMFActivate> selected;
      if (direct) {
        library.handle = LoadLibraryExA(argv[3], nullptr, LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32);
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
      std::printf("{\"schema\":\"camera-pixel-receiver-v1\",\"receiverMode\":\"%s\",\"width\":%u,\"height\":%u,\"fpsNumerator\":%u,\"fpsDenominator\":%u}\n", direct ? "direct-dll" : "os-camera", width, height, numerator, denominator);
      const auto start = Clock::now(); uint64_t samples = 0;
      while (Clock::now() - start < std::chrono::seconds(seconds)) {
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
        std::printf("{\"sample\":%llu,\"arrivalUs\":%lld,\"arrivalHostUs\":%lld,\"pts100ns\":%lld,\"identity\":", ++samples, arrival, arrivalHost, pts);
        if (identity) std::printf("%u", *identity); else std::printf("null");
        std::printf("}\n");
      }
      std::printf("{\"complete\":true,\"samples\":%llu}\n", samples);
      reader.Reset(); source->Shutdown(); source.Reset(); selected->ShutdownObject();
    }
    MFShutdown(); CoUninitialize();
  } catch (const std::exception& error) { std::fprintf(stderr, "%s\n", error.what()); result = 2; }
  watchdog.request_stop();
  return result;
}
