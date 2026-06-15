#include "modules/Interfaces.h"

#include <algorithm>
#include <iterator>
#include <memory>
#include <sstream>
#include <string>
#include <utility>

#if !COREVIDEO_STUB && COREVIDEO_ENABLE_DEV_ADAPTERS && (COREVIDEO_WITH_DECKLINK || COREVIDEO_WITH_AJA)
#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <combaseapi.h>
#else
#include <dlfcn.h>
#endif
#endif

namespace corevideo::modules {
namespace {

int clampAudioOffset(int offsetMs) {
  return std::max(-500, std::min(500, offsetMs));
}

class MutableCaptureDeviceList final : public ICaptureDevice {
 public:
  explicit MutableCaptureDeviceList(std::vector<CaptureDeviceInfo> devices) : devices_(std::move(devices)) {}

  std::vector<CaptureDeviceInfo> enumerate() const override {
    return devices_;
  }

  std::vector<CaptureDeviceInfo> selectInput(const std::string& deviceId, const std::string& inputId) override {
    for (auto& device : devices_) {
      if (device.id != deviceId) {
        continue;
      }
      if (std::find(device.inputIds.begin(), device.inputIds.end(), inputId) != device.inputIds.end()) {
        device.selectedInputId = inputId;
      }
    }
    return enumerate();
  }

  std::vector<CaptureDeviceInfo> setAudioSyncOffset(const std::string& deviceId, int offsetMs) override {
    for (auto& device : devices_) {
      if (device.id == deviceId) {
        device.audioSyncOffsetMs = clampAudioOffset(offsetMs);
      }
    }
    return enumerate();
  }

  std::vector<CaptureDeviceInfo> connect(const std::string& deviceId) override {
    for (auto& device : devices_) {
      if (device.id == deviceId && device.connectionState != "connected") {
        device.connectionState = "connected";
        device.signalPresent = true;
        device.warning.clear();
      }
    }
    return enumerate();
  }

 private:
  std::vector<CaptureDeviceInfo> devices_;
};

#if !COREVIDEO_STUB && COREVIDEO_ENABLE_DEV_ADAPTERS && (COREVIDEO_WITH_DECKLINK || COREVIDEO_WITH_AJA)
bool runtimeAvailable(const char* const* candidates, size_t count) {
#if defined(_WIN32)
  for (size_t index = 0; index < count; ++index) {
    if (HMODULE module = LoadLibraryA(candidates[index])) {
      FreeLibrary(module);
      return true;
    }
  }
  return false;
#else
  for (size_t index = 0; index < count; ++index) {
    if (void* module = dlopen(candidates[index], RTLD_LAZY | RTLD_LOCAL)) {
      dlclose(module);
      return true;
    }
  }
  return false;
#endif
}

CaptureDeviceInfo unavailableDevice(const std::string& id,
                                    const std::string& name,
                                    const std::string& vendor,
                                    const std::string& warning) {
  CaptureDeviceInfo device;
  device.id = id;
  device.name = name;
  device.kind = "video";
  device.vendor = vendor;
  device.inputIds = {"sdi-1"};
  device.inputLabels = {"SDI 1"};
  device.inputHasEmbeddedAudio = {true};
  device.selectedInputId = "sdi-1";
  device.width = 1920;
  device.height = 1080;
  device.frameRate = 60;
  device.connectionState = "error";
  device.signalPresent = false;
  device.warning = warning;
  return device;
}

#if defined(_WIN32) && COREVIDEO_WITH_DECKLINK
// Minimal DeckLink COM surface for runtime enumeration without vendoring the full SDK.
struct IDeckLink;
struct IDeckLinkIterator;

static const CLSID kDeckLinkIteratorClsid = {
    0xBA6B6F66, 0x6DD9, 0x4D91, {0xAA, 0x60, 0x5E, 0x86, 0xB9, 0x99, 0x48, 0xF0}};
static const IID kDeckLinkIteratorIid = {
    0x50FB36CD, 0x3063, 0x4B73, {0xBB, 0x63, 0x40, 0x34, 0x92, 0xD3, 0x9E, 0x7C}};
static const IID kDeckLinkIid = {0x62BFF75D, 0x6569, 0x499e, {0xAE, 0xFF, 0x73, 0xB7, 0xDC, 0xED, 0x44, 0x8D}};

struct IDeckLinkIterator : public IUnknown {
  virtual HRESULT STDMETHODCALLTYPE Next(IDeckLink** deckLinkInstance) = 0;
};

struct IDeckLink : public IUnknown {
  virtual HRESULT STDMETHODCALLTYPE GetModelName(BSTR* modelName) = 0;
  virtual HRESULT STDMETHODCALLTYPE GetDisplayName(BSTR* displayName) = 0;
};

std::string bstrToUtf8(BSTR value) {
  if (!value) {
    return "";
  }
  const int length = WideCharToMultiByte(CP_UTF8, 0, value, -1, nullptr, 0, nullptr, nullptr);
  if (length <= 0) {
    return "";
  }
  std::string result(static_cast<size_t>(length - 1), '\0');
  WideCharToMultiByte(CP_UTF8, 0, value, -1, result.data(), length, nullptr, nullptr);
  return result;
}

std::vector<CaptureDeviceInfo> enumerateDeckLinkDevices() {
  const char* candidates[] = {"DeckLinkAPI64.dll", "DeckLinkAPI.dll"};
  if (!runtimeAvailable(candidates, std::size(candidates))) {
    return {unavailableDevice(
        "decklink-runtime-missing",
        "Blackmagic DeckLink",
        "blackmagic",
        "DeckLink SDK runtime not found. Install Blackmagic Desktop Video to enumerate hardware.")};
  }

  std::vector<CaptureDeviceInfo> devices;
  HRESULT comInit = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
  const bool comInitialized = comInit == S_OK || comInit == S_FALSE;

  IDeckLinkIterator* iterator = nullptr;
  if (FAILED(CoCreateInstance(kDeckLinkIteratorClsid, nullptr, CLSCTX_ALL, kDeckLinkIteratorIid,
                              reinterpret_cast<void**>(&iterator))) ||
      !iterator) {
    if (comInitialized) {
      CoUninitialize();
    }
    return {unavailableDevice(
        "decklink-iterator-unavailable",
        "Blackmagic DeckLink",
        "blackmagic",
        "DeckLink runtime is installed, but the iterator could not be created. Restart Desktop Video.")};
  }

  int index = 0;
  for (;;) {
    IDeckLink* deckLink = nullptr;
    if (iterator->Next(&deckLink) != S_OK || !deckLink) {
      break;
    }

    BSTR modelName = nullptr;
    BSTR displayName = nullptr;
    deckLink->GetModelName(&modelName);
    deckLink->GetDisplayName(&displayName);

    CaptureDeviceInfo device;
    device.id = "decklink-" + std::to_string(index + 1);
    device.name = bstrToUtf8(displayName);
    if (device.name.empty()) {
      device.name = bstrToUtf8(modelName);
    }
    if (device.name.empty()) {
      device.name = "Blackmagic DeckLink";
    }
    device.kind = "video";
    device.vendor = "blackmagic";
    device.inputIds = {"sdi-1", "hdmi-1"};
    device.inputLabels = {"SDI 1", "HDMI"};
    device.inputHasEmbeddedAudio = {true, true};
    device.selectedInputId = "sdi-1";
    device.width = 1920;
    device.height = 1080;
    device.frameRate = 60;
    device.connectionState = "detected";
    device.signalPresent = false;
    device.warning = "DeckLink device detected. Connect the input and choose SDI or HDMI.";

    if (modelName) {
      SysFreeString(modelName);
    }
    if (displayName) {
      SysFreeString(displayName);
    }
    deckLink->Release();

    devices.push_back(std::move(device));
    ++index;
  }

  iterator->Release();
  if (comInitialized) {
    CoUninitialize();
  }

  if (devices.empty()) {
    return {unavailableDevice(
        "decklink-none-detected",
        "Blackmagic DeckLink",
        "blackmagic",
        "DeckLink runtime is installed, but no capture devices were detected.")};
  }

  return devices;
}
#else
std::vector<CaptureDeviceInfo> enumerateDeckLinkDevices() {
  const char* candidates[] = {"DeckLinkAPI.dll", "DeckLinkAPI64.dll"};
#if !defined(_WIN32)
  const char* altCandidates[] = {"libDeckLinkAPI.so", "DeckLinkAPI.framework/DeckLinkAPI"};
  if (!runtimeAvailable(altCandidates, std::size(altCandidates))) {
    return {unavailableDevice(
        "decklink-runtime-missing",
        "Blackmagic DeckLink",
        "blackmagic",
        "DeckLink SDK runtime not found. Install Blackmagic Desktop Video to enumerate hardware.")};
  }
#else
  if (!runtimeAvailable(candidates, std::size(candidates))) {
    return {unavailableDevice(
        "decklink-runtime-missing",
        "Blackmagic DeckLink",
        "blackmagic",
        "DeckLink SDK runtime not found. Install Blackmagic Desktop Video to enumerate hardware.")};
  }
#endif
  return {unavailableDevice(
      "decklink-dev-1",
      "Blackmagic DeckLink",
      "blackmagic",
      "DeckLink SDK runtime detected; COM enumeration is enabled on Windows dev builds.")};
}
#endif

std::vector<CaptureDeviceInfo> enumerateAjaDevices() {
#if defined(_WIN32)
  const char* candidates[] = {"ntv2lib.dll", "ajantv2.dll"};
#else
  const char* candidates[] = {"libajantv2.so", "AJA_NTV2.framework/AJA_NTV2"};
#endif
  if (!runtimeAvailable(candidates, std::size(candidates))) {
    return {unavailableDevice(
        "aja-runtime-missing",
        "AJA NTV2 Device",
        "aja",
        "AJA NTV2 runtime not found. Install the AJA Desktop Software/SDK to enumerate hardware.")};
  }

  CaptureDeviceInfo device;
  device.id = "aja-dev-1";
  device.name = "AJA NTV2 Device";
  device.kind = "video";
  device.vendor = "aja";
  device.inputIds = {"sdi-1", "sdi-2"};
  device.inputLabels = {"SDI 1", "SDI 2"};
  device.inputHasEmbeddedAudio = {true, false};
  device.selectedInputId = "sdi-1";
  device.width = 1920;
  device.height = 1080;
  device.frameRate = 30;
  device.connectionState = "detected";
  device.signalPresent = false;
  device.warning = "AJA NTV2 runtime detected; card enumeration is enabled on this dev machine.";
  return {device};
}
#endif

}  // namespace

std::unique_ptr<ICaptureDevice> createDeckLinkCaptureDevice() {
#if !COREVIDEO_STUB && COREVIDEO_ENABLE_DEV_ADAPTERS && COREVIDEO_WITH_DECKLINK
  auto devices = enumerateDeckLinkDevices();
  if (devices.empty()) {
    return nullptr;
  }
  return std::make_unique<MutableCaptureDeviceList>(std::move(devices));
#else
  return nullptr;
#endif
}

std::unique_ptr<ICaptureDevice> createAjaCaptureDevice() {
#if !COREVIDEO_STUB && COREVIDEO_ENABLE_DEV_ADAPTERS && COREVIDEO_WITH_AJA
  auto devices = enumerateAjaDevices();
  if (devices.empty()) {
    return nullptr;
  }
  return std::make_unique<MutableCaptureDeviceList>(std::move(devices));
#else
  return nullptr;
#endif
}

}  // namespace corevideo::modules