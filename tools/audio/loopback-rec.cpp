// Minimal WASAPI loopback recorder: captures what a render endpoint is
// playing (i.e., exactly what the operator hears) to raw float32 interleaved.
// Usage: loopback-rec.exe <device-name-substring> <seconds> <out.f32>
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <audioclient.h>
#include <mmdeviceapi.h>
#include <functiondiscoverykeys_devpkey.h>

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

static std::string wideToUtf8(const wchar_t* wide) {
  if (!wide) return {};
  const int needed = WideCharToMultiByte(CP_UTF8, 0, wide, -1, nullptr, 0, nullptr, nullptr);
  if (needed <= 1) return {};
  std::string out(static_cast<size_t>(needed - 1), '\0');
  WideCharToMultiByte(CP_UTF8, 0, wide, -1, &out[0], needed, nullptr, nullptr);
  return out;
}

int main(int argc, char** argv) {
  if (argc < 4) {
    std::fprintf(stderr, "usage: loopback-rec <device-substring> <seconds> <out.f32>\n");
    return 2;
  }
  const std::string wanted = argv[1];
  const int seconds = std::atoi(argv[2]);
  const char* outPath = argv[3];

  CoInitializeEx(nullptr, COINIT_MULTITHREADED);
  IMMDeviceEnumerator* enumerator = nullptr;
  if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                              __uuidof(IMMDeviceEnumerator), reinterpret_cast<void**>(&enumerator)))) {
    std::fprintf(stderr, "enumerator failed\n");
    return 1;
  }

  IMMDeviceCollection* collection = nullptr;
  enumerator->EnumAudioEndpoints(eRender, DEVICE_STATE_ACTIVE, &collection);
  UINT count = 0;
  collection->GetCount(&count);
  IMMDevice* device = nullptr;
  std::string matchedName;
  for (UINT i = 0; i < count; ++i) {
    IMMDevice* candidate = nullptr;
    if (FAILED(collection->Item(i, &candidate))) continue;
    IPropertyStore* store = nullptr;
    std::string name;
    if (SUCCEEDED(candidate->OpenPropertyStore(STGM_READ, &store))) {
      PROPVARIANT value;
      PropVariantInit(&value);
      if (SUCCEEDED(store->GetValue(PKEY_Device_FriendlyName, &value)) && value.vt == VT_LPWSTR) {
        name = wideToUtf8(value.pwszVal);
      }
      PropVariantClear(&value);
      store->Release();
    }
    if (!device && name.find(wanted) != std::string::npos) {
      device = candidate;
      matchedName = name;
      continue;
    }
    candidate->Release();
  }
  if (!device) {
    std::fprintf(stderr, "no render endpoint matching '%s'\n", wanted.c_str());
    return 1;
  }
  std::fprintf(stderr, "recording loopback of: %s\n", matchedName.c_str());

  IAudioClient* client = nullptr;
  if (FAILED(device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, reinterpret_cast<void**>(&client)))) {
    std::fprintf(stderr, "activate failed\n");
    return 1;
  }
  WAVEFORMATEX* format = nullptr;
  client->GetMixFormat(&format);
  std::fprintf(stderr, "format: %luHz %dch %d-bit\n", format->nSamplesPerSec, format->nChannels,
               format->wBitsPerSample);
  if (FAILED(client->Initialize(AUDCLNT_SHAREMODE_SHARED, AUDCLNT_STREAMFLAGS_LOOPBACK,
                                10'000'000, 0, format, nullptr))) {
    std::fprintf(stderr, "initialize(loopback) failed\n");
    return 1;
  }
  IAudioCaptureClient* capture = nullptr;
  if (FAILED(client->GetService(__uuidof(IAudioCaptureClient), reinterpret_cast<void**>(&capture)))) {
    std::fprintf(stderr, "capture service failed\n");
    return 1;
  }
  client->Start();

  FILE* out = std::fopen(outPath, "wb");
  const int channels = format->nChannels;
  const bool isFloat =
      format->wFormatTag == WAVE_FORMAT_IEEE_FLOAT ||
      (format->wFormatTag == WAVE_FORMAT_EXTENSIBLE &&
       reinterpret_cast<WAVEFORMATEXTENSIBLE*>(format)->SubFormat == KSDATAFORMAT_SUBTYPE_IEEE_FLOAT);
  const ULONGLONG endTick = GetTickCount64() + static_cast<ULONGLONG>(seconds) * 1000ULL;
  long long frames = 0;
  long long gapEvents = 0;
  while (GetTickCount64() < endTick) {
    UINT32 packet = 0;
    capture->GetNextPacketSize(&packet);
    if (packet == 0) {
      Sleep(2);
      continue;
    }
    BYTE* data = nullptr;
    UINT32 got = 0;
    DWORD flags = 0;
    if (FAILED(capture->GetBuffer(&data, &got, &flags, nullptr, nullptr))) break;
    if (flags & AUDCLNT_BUFFERFLAGS_DATA_DISCONTINUITY) ++gapEvents;
    if (isFloat) {
      if (flags & AUDCLNT_BUFFERFLAGS_SILENT) {
        std::vector<float> zeros(static_cast<size_t>(got) * channels, 0.0f);
        std::fwrite(zeros.data(), sizeof(float), zeros.size(), out);
      } else {
        std::fwrite(data, sizeof(float), static_cast<size_t>(got) * channels, out);
      }
    }
    frames += got;
    capture->ReleaseBuffer(got);
  }
  client->Stop();
  std::fclose(out);
  std::fprintf(stderr, "captured %lld frames (%.1fs), %lld discontinuity flags\n", frames,
               frames / static_cast<double>(format->nSamplesPerSec), gapEvents);
  return 0;
}
