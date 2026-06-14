#include "modules/Interfaces.h"

#include <algorithm>
#include <iterator>
#include <memory>
#include <string>
#include <utility>

#if !COREVIDEO_STUB && COREVIDEO_ENABLE_DEV_ADAPTERS && (COREVIDEO_WITH_DECKLINK || COREVIDEO_WITH_AJA)
#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <dlfcn.h>
#endif
#endif

namespace corevideo::modules {
namespace {

int clampAudioOffset(int offsetMs) {
  return std::max(-500, std::min(500, offsetMs));
}

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

class SingleHardwareCaptureDevice final : public ICaptureDevice {
 public:
  explicit SingleHardwareCaptureDevice(CaptureDeviceInfo device) : device_(std::move(device)) {}

  std::vector<CaptureDeviceInfo> enumerate() const override { return {device_}; }

  std::vector<CaptureDeviceInfo> selectInput(const std::string& deviceId, const std::string& inputId) override {
    if (device_.id == deviceId && std::find(device_.inputIds.begin(), device_.inputIds.end(), inputId) != device_.inputIds.end()) {
      device_.selectedInputId = inputId;
    }
    return enumerate();
  }

  std::vector<CaptureDeviceInfo> setAudioSyncOffset(const std::string& deviceId, int offsetMs) override {
    if (device_.id == deviceId) {
      device_.audioSyncOffsetMs = clampAudioOffset(offsetMs);
    }
    return enumerate();
  }

  std::vector<CaptureDeviceInfo> connect(const std::string& deviceId) override {
    if (device_.id == deviceId && device_.connectionState != "connected") {
      device_.connectionState = "connected";
      device_.signalPresent = true;
      device_.warning.clear();
    }
    return enumerate();
  }

 private:
  CaptureDeviceInfo device_;
};

CaptureDeviceInfo deckLinkDevice(bool available) {
  CaptureDeviceInfo device;
  device.id = "decklink-dev-1";
  device.name = "Blackmagic DeckLink";
  device.kind = "video";
  device.vendor = "blackmagic";
  device.inputIds = {"sdi-1", "hdmi-1"};
  device.inputLabels = {"SDI 1", "HDMI"};
  device.inputHasEmbeddedAudio = {true, true};
  device.selectedInputId = "sdi-1";
  device.width = 1920;
  device.height = 1080;
  device.frameRate = 60;
  device.connectionState = available ? "detected" : "error";
  device.signalPresent = false;
  device.warning = available ? "DeckLink SDK runtime detected; real hardware enumeration is enabled on this dev machine."
                             : "DeckLink SDK runtime not found. Install Blackmagic Desktop Video to enumerate hardware.";
  return device;
}

CaptureDeviceInfo ajaDevice(bool available) {
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
  device.connectionState = available ? "detected" : "error";
  device.signalPresent = false;
  device.warning = available ? "AJA NTV2 runtime detected; real hardware enumeration is enabled on this dev machine."
                             : "AJA NTV2 runtime not found. Install the AJA SDK/runtime to enumerate hardware.";
  return device;
}
#endif

}  // namespace

std::unique_ptr<ICaptureDevice> createDeckLinkCaptureDevice() {
#if !COREVIDEO_STUB && COREVIDEO_ENABLE_DEV_ADAPTERS && COREVIDEO_WITH_DECKLINK
#if defined(_WIN32)
  const char* candidates[] = {"DeckLinkAPI.dll", "DeckLinkAPI64.dll"};
#else
  const char* candidates[] = {"libDeckLinkAPI.so", "DeckLinkAPI.framework/DeckLinkAPI"};
#endif
  // REQUIRES DEV MACHINE: real IDeckLinkIterator enumeration belongs behind
  // this adapter once Blackmagic SDK headers/runtime are installed.
  return std::make_unique<SingleHardwareCaptureDevice>(deckLinkDevice(runtimeAvailable(candidates, std::size(candidates))));
#else
  return nullptr;
#endif
}

std::unique_ptr<ICaptureDevice> createAjaCaptureDevice() {
#if !COREVIDEO_STUB && COREVIDEO_ENABLE_DEV_ADAPTERS && COREVIDEO_WITH_AJA
#if defined(_WIN32)
  const char* candidates[] = {"ntv2lib.dll", "ajantv2.dll"};
#else
  const char* candidates[] = {"libajantv2.so", "AJA_NTV2.framework/AJA_NTV2"};
#endif
  // REQUIRES DEV MACHINE: real CNTV2Card enumeration belongs behind this
  // adapter once AJA SDK headers/runtime are installed.
  return std::make_unique<SingleHardwareCaptureDevice>(ajaDevice(runtimeAvailable(candidates, std::size(candidates))));
#else
  return nullptr;
#endif
}

}  // namespace corevideo::modules
