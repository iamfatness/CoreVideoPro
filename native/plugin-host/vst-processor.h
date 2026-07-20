#pragma once

// VST3 plugin instantiation + stereo block processing (VST spec P2c).
//
// Header-only machinery over the raw COM ABI in vst-abi.h. The entry point
// takes an IPluginFactory* — production hands it the factory from a loaded
// .vst3 module, unit tests inject a fake factory built from the same raw
// vtables (no LoadLibrary), which is how the ABI plumbing is proven without
// the VST3 SDK.
//
// Lifecycle implemented (the canonical VST3 processor sequence):
//   createInstance(cid, IComponent) → setIoMode → initialize(host context)
//   → queryInterface(IAudioProcessor) → canProcessSampleSize(kSample32)
//   → setBusArrangements(stereo main in/out) → setupProcessing(kRealtime,
//     48kHz, maxBlock) → activateBus(main in/out) → setActive(true)
//   → setProcessing(true) → process(ProcessData) per block.
//
// Failure honesty: every step failure lands in lastError() and start()
// returns false — the caller bypasses (audio untouched) and reports, never
// fakes a "processing" status. processInterleavedStereo only writes back to
// the caller's buffer as its LAST step, so a failed process call leaves the
// caller's audio bit-identical (bypass semantics by construction).
//
// Real-time discipline: all buffers are allocated once in start(); process
// calls do no allocation.

#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

#include "vst-abi.h"

namespace corevideo::vsthost {

// ---------------------------------------------------------------------------
// Minimal host context (IHostApplication + FUnknown). Static lifetime, no
// real ref counting. Plugins receive it in IPluginBase::initialize; the only
// services offered are getName and interface navigation to itself — every
// other request honestly returns kNoInterface / kNotImplemented.
// ---------------------------------------------------------------------------
struct HostApplicationContext {
  vst3::IHostApplicationVtbl* vtbl;
};

namespace detail {

inline vst3::tresult CVST_API hostContextQueryInterface(void* self, const char* iid, void** obj) {
  if (obj == nullptr) {
    return vst3::kNoInterface;
  }
  if (vst3::tuidEqual(iid, vst3::kFUnknownIid.bytes) ||
      vst3::tuidEqual(iid, vst3::kIHostApplicationIid.bytes)) {
    *obj = self;
    return vst3::kResultOk;
  }
  *obj = nullptr;
  return vst3::kNoInterface;
}

inline uint32_t CVST_API staticAddRef(void*) { return 1; }
inline uint32_t CVST_API staticRelease(void*) { return 1; }

inline vst3::tresult CVST_API hostContextGetName(void*, char16_t* name) {
  if (name == nullptr) {
    return vst3::kResultFalse;
  }
  const char16_t kName[] = u"CoreVideo Pro";
  std::memcpy(name, kName, sizeof(kName));
  return vst3::kResultOk;
}

inline vst3::tresult CVST_API hostContextCreateInstance(void*, char*, char*, void** obj) {
  if (obj != nullptr) {
    *obj = nullptr;
  }
  return vst3::kNotImplemented;  // no IMessage/IAttributeList services in v1
}

inline vst3::IHostApplicationVtbl* hostApplicationVtbl() {
  static vst3::IHostApplicationVtbl vtbl = {
      &hostContextQueryInterface, &staticAddRef, &staticRelease,
      &hostContextGetName, &hostContextCreateInstance,
  };
  return &vtbl;
}

// Empty IParameterChanges for ProcessData — safer than null pointers, which
// some plugins dereference unconditionally.
inline std::string utf16ToUtf8Ascii(const char16_t* value, size_t capacity) {
  std::string result;
  for (size_t index = 0; value != nullptr && index < capacity && value[index] != 0; ++index) {
    result.push_back(value[index] <= 0x7f ? static_cast<char>(value[index]) : '?');
  }
  return result;
}

struct ParameterValueQueue {
  vst3::IParamValueQueueVtbl* vtbl;
  vst3::ParamID id = 0;
  int32_t pointCount = 0;
  int32_t sampleOffset = 0;
  double value = 0.0;
};

inline vst3::tresult CVST_API paramQueueQueryInterface(void* self, const char*, void** obj) {
  if (obj == nullptr) return vst3::kNoInterface;
  *obj = self;
  return vst3::kResultOk;
}
inline vst3::ParamID CVST_API paramQueueId(void* self) { return static_cast<ParameterValueQueue*>(self)->id; }
inline int32_t CVST_API paramQueueCount(void* self) { return static_cast<ParameterValueQueue*>(self)->pointCount; }
inline vst3::tresult CVST_API paramQueueGetPoint(void* self, int32_t index, int32_t* offset, double* value) {
  auto* queue = static_cast<ParameterValueQueue*>(self);
  if (index != 0 || queue->pointCount == 0 || offset == nullptr || value == nullptr) return vst3::kResultFalse;
  *offset = queue->sampleOffset;
  *value = queue->value;
  return vst3::kResultOk;
}
inline vst3::tresult CVST_API paramQueueAddPoint(void* self, int32_t offset, double value, int32_t* index) {
  auto* queue = static_cast<ParameterValueQueue*>(self);
  queue->sampleOffset = offset;
  queue->value = value;
  queue->pointCount = 1;
  if (index != nullptr) *index = 0;
  return vst3::kResultOk;
}
inline vst3::IParamValueQueueVtbl* parameterValueQueueVtbl() {
  static vst3::IParamValueQueueVtbl vtbl = {
      &paramQueueQueryInterface, &staticAddRef, &staticRelease,
      &paramQueueId, &paramQueueCount, &paramQueueGetPoint, &paramQueueAddPoint};
  return &vtbl;
}

struct ParameterChanges {
  vst3::IParameterChangesVtbl* vtbl;
  std::array<ParameterValueQueue, 32> queues{};
  int32_t count = 0;
};

inline vst3::tresult CVST_API paramChangesQueryInterface(void* self, const char* iid, void** obj) {
  if (obj == nullptr) {
    return vst3::kNoInterface;
  }
  if (vst3::tuidEqual(iid, vst3::kFUnknownIid.bytes) ||
      vst3::tuidEqual(iid, vst3::kIParameterChangesIid.bytes)) {
    *obj = self;
    return vst3::kResultOk;
  }
  *obj = nullptr;
  return vst3::kNoInterface;
}

inline int32_t CVST_API paramChangesCount(void* self) { return static_cast<ParameterChanges*>(self)->count; }
inline void* CVST_API paramChangesGetData(void* self, int32_t index) {
  auto* changes = static_cast<ParameterChanges*>(self);
  return index >= 0 && index < changes->count ? &changes->queues[static_cast<size_t>(index)] : nullptr;
}
inline void* CVST_API paramChangesAddData(void* self, const vst3::ParamID* id, int32_t* index) {
  auto* changes = static_cast<ParameterChanges*>(self);
  if (id == nullptr) return nullptr;
  for (int32_t i = 0; i < changes->count; ++i) {
    if (changes->queues[static_cast<size_t>(i)].id == *id) {
      if (index != nullptr) *index = i;
      return &changes->queues[static_cast<size_t>(i)];
    }
  }
  if (changes->count >= static_cast<int32_t>(changes->queues.size())) return nullptr;
  const int32_t next = changes->count++;
  auto& queue = changes->queues[static_cast<size_t>(next)];
  queue.vtbl = parameterValueQueueVtbl();
  queue.id = *id;
  queue.pointCount = 0;
  if (index != nullptr) *index = next;
  return &queue;
}

inline vst3::IParameterChangesVtbl* parameterChangesVtbl() {
  static vst3::IParameterChangesVtbl vtbl = {
      &paramChangesQueryInterface, &staticAddRef, &staticRelease,
      &paramChangesCount, &paramChangesGetData, &paramChangesAddData,
  };
  return &vtbl;
}

// ---------------------------------------------------------------------------
// A2: minimal raw-ABI memory IBStream. IComponent::getState writes the
// plugin's state INTO it; setState reads a saved blob back OUT of it. Stack
// lifetime, no real ref counting (plugins hold it only for the call), honest
// partial-read semantics (kResultOk with a short byte count = EOF, the VST3
// stream contract).
// ---------------------------------------------------------------------------
struct MemoryStream {
  vst3::IBStreamVtbl* vtbl;
  std::vector<uint8_t>* data = nullptr;
  int64_t cursor = 0;
};

inline vst3::tresult CVST_API memoryStreamQueryInterface(void* self, const char* iid, void** obj) {
  if (obj == nullptr) return vst3::kNoInterface;
  if (vst3::tuidEqual(iid, vst3::kFUnknownIid.bytes) ||
      vst3::tuidEqual(iid, vst3::kIBStreamIid.bytes)) {
    *obj = self;
    return vst3::kResultOk;
  }
  *obj = nullptr;
  return vst3::kNoInterface;
}

inline vst3::tresult CVST_API memoryStreamRead(void* self, void* buffer, int32_t numBytes, int32_t* numBytesRead) {
  auto* stream = static_cast<MemoryStream*>(self);
  if (stream->data == nullptr || buffer == nullptr || numBytes < 0) {
    if (numBytesRead != nullptr) *numBytesRead = 0;
    return vst3::kResultFalse;
  }
  const int64_t available = static_cast<int64_t>(stream->data->size()) - stream->cursor;
  const int64_t count = available < 0 ? 0 : (available < numBytes ? available : numBytes);
  if (count > 0) {
    std::memcpy(buffer, stream->data->data() + stream->cursor, static_cast<size_t>(count));
    stream->cursor += count;
  }
  if (numBytesRead != nullptr) *numBytesRead = static_cast<int32_t>(count);
  return vst3::kResultOk;
}

inline vst3::tresult CVST_API memoryStreamWrite(void* self, void* buffer, int32_t numBytes, int32_t* numBytesWritten) {
  auto* stream = static_cast<MemoryStream*>(self);
  if (stream->data == nullptr || buffer == nullptr || numBytes < 0) {
    if (numBytesWritten != nullptr) *numBytesWritten = 0;
    return vst3::kResultFalse;
  }
  const auto end = static_cast<size_t>(stream->cursor) + static_cast<size_t>(numBytes);
  if (stream->data->size() < end) {
    stream->data->resize(end);
  }
  if (numBytes > 0) {
    std::memcpy(stream->data->data() + stream->cursor, buffer, static_cast<size_t>(numBytes));
    stream->cursor += numBytes;
  }
  if (numBytesWritten != nullptr) *numBytesWritten = numBytes;
  return vst3::kResultOk;
}

inline vst3::tresult CVST_API memoryStreamSeek(void* self, int64_t pos, int32_t mode, int64_t* result) {
  auto* stream = static_cast<MemoryStream*>(self);
  if (stream->data == nullptr) return vst3::kResultFalse;
  int64_t target = 0;
  switch (mode) {
    case vst3::kIBSeekSet: target = pos; break;
    case vst3::kIBSeekCur: target = stream->cursor + pos; break;
    case vst3::kIBSeekEnd: target = static_cast<int64_t>(stream->data->size()) + pos; break;
    default: return vst3::kResultFalse;
  }
  if (target < 0) target = 0;
  stream->cursor = target;
  if (result != nullptr) *result = target;
  return vst3::kResultOk;
}

inline vst3::tresult CVST_API memoryStreamTell(void* self, int64_t* pos) {
  auto* stream = static_cast<MemoryStream*>(self);
  if (pos == nullptr) return vst3::kResultFalse;
  *pos = stream->cursor;
  return vst3::kResultOk;
}

inline vst3::IBStreamVtbl* memoryStreamVtbl() {
  static vst3::IBStreamVtbl vtbl = {
      &memoryStreamQueryInterface, &staticAddRef, &staticRelease,
      &memoryStreamRead, &memoryStreamWrite, &memoryStreamSeek, &memoryStreamTell,
  };
  return &vtbl;
}

inline MemoryStream makeMemoryStream(std::vector<uint8_t>* data) {
  MemoryStream stream;
  stream.vtbl = memoryStreamVtbl();
  stream.data = data;
  return stream;
}

inline std::string toLowerAscii(const std::string& value) {
  std::string lowered;
  lowered.reserve(value.size());
  for (const char character : value) {
    lowered.push_back(character >= 'A' && character <= 'Z' ? static_cast<char>(character - 'A' + 'a')
                                                           : character);
  }
  return lowered;
}

}  // namespace detail

// Finds the audio-effect class matching `className` (case-insensitive exact,
// then substring; empty selects the first audio class). Returns false + a
// reason when nothing matches.
inline bool findAudioClass(vst3::IPluginFactory* factory, const std::string& className,
                           vst3::PClassInfo* outInfo, std::string* outError) {
  if (factory == nullptr || factory->vtbl == nullptr) {
    if (outError != nullptr) *outError = "null plugin factory";
    return false;
  }
  const std::string wanted = detail::toLowerAscii(className);
  const int32_t classCount = factory->vtbl->countClasses(factory);
  vst3::PClassInfo firstAudio{};
  vst3::PClassInfo substringMatch{};
  bool haveFirst = false;
  bool haveSubstring = false;
  for (int32_t index = 0; index < classCount; ++index) {
    vst3::PClassInfo info{};
    if (factory->vtbl->getClassInfo(factory, index, &info) != vst3::kResultOk) {
      continue;
    }
    const std::string category(info.category, strnlen(info.category, sizeof(info.category)));
    if (category != "Audio Module Class") {
      continue;
    }
    if (!haveFirst) {
      firstAudio = info;
      haveFirst = true;
    }
    const std::string name = detail::toLowerAscii(
        std::string(info.name, strnlen(info.name, sizeof(info.name))));
    if (!wanted.empty() && name == wanted) {
      *outInfo = info;
      return true;
    }
    if (!wanted.empty() && !haveSubstring && name.find(wanted) != std::string::npos) {
      substringMatch = info;
      haveSubstring = true;
    }
  }
  if (wanted.empty() && haveFirst) {
    *outInfo = firstAudio;
    return true;
  }
  if (haveSubstring) {
    *outInfo = substringMatch;
    return true;
  }
  if (outError != nullptr) {
    *outError = haveFirst ? ("no audio class named '" + className + "' in module")
                          : "module exposes no Audio Module Class";
  }
  return false;
}

struct VstProcessorConfig {
  double sampleRate = 48000.0;
  int32_t maxBlockFrames = 4096;  // >= kHostBlockMaxSamples/2 (host-transport.h)
};

class VstPluginInstance;

struct ComponentHandler {
  vst3::IComponentHandlerVtbl* vtbl;
  VstPluginInstance* owner = nullptr;
};

struct PlugFrame {
  vst3::IPlugFrameVtbl* vtbl;
  VstPluginInstance* owner = nullptr;
};

class VstPluginInstance {
 public:
  VstPluginInstance() {
    hostContext_.vtbl = detail::hostApplicationVtbl();
    static vst3::IComponentHandlerVtbl componentHandlerVtbl = {
        [](void* self, const char* iid, void** obj) -> vst3::tresult {
          if (obj == nullptr) return vst3::kNoInterface;
          if (vst3::tuidEqual(iid, vst3::kFUnknownIid.bytes) ||
              vst3::tuidEqual(iid, vst3::kIComponentHandlerIid.bytes)) {
            *obj = self;
            return vst3::kResultOk;
          }
          *obj = nullptr;
          return vst3::kNoInterface;
        },
        &detail::staticAddRef, &detail::staticRelease,
        [](void*, vst3::ParamID) -> vst3::tresult { return vst3::kResultOk; },
        [](void* self, vst3::ParamID id, double value) -> vst3::tresult {
          auto* handler = static_cast<ComponentHandler*>(self);
          return handler->owner != nullptr && handler->owner->queueParameterChange(id, value)
                     ? vst3::kResultOk : vst3::kResultFalse;
        },
        [](void*, vst3::ParamID) -> vst3::tresult { return vst3::kResultOk; },
        [](void*, int32_t) -> vst3::tresult { return vst3::kResultOk; }};
    static vst3::IPlugFrameVtbl plugFrameVtbl = {
        [](void* self, const char* iid, void** obj) -> vst3::tresult {
          if (obj == nullptr) return vst3::kNoInterface;
          if (vst3::tuidEqual(iid, vst3::kFUnknownIid.bytes) ||
              vst3::tuidEqual(iid, vst3::kIPlugFrameIid.bytes)) {
            *obj = self;
            return vst3::kResultOk;
          }
          *obj = nullptr;
          return vst3::kNoInterface;
        },
        &detail::staticAddRef, &detail::staticRelease,
        [](void* self, vst3::IPlugView*, vst3::ViewRect* rect) -> vst3::tresult {
          auto* frame = static_cast<PlugFrame*>(self);
          return frame->owner != nullptr ? frame->owner->resizeEditor(rect) : vst3::kResultFalse;
        }};
    componentHandler_.vtbl = &componentHandlerVtbl;
    componentHandler_.owner = this;
    plugFrame_.vtbl = &plugFrameVtbl;
    plugFrame_.owner = this;
  }
  ~VstPluginInstance() { shutdown(); }
  VstPluginInstance(const VstPluginInstance&) = delete;
  VstPluginInstance& operator=(const VstPluginInstance&) = delete;

  [[nodiscard]] bool started() const { return started_; }
  [[nodiscard]] const std::string& lastError() const { return lastError_; }
  [[nodiscard]] const std::string& activeClassName() const { return activeClassName_; }
  [[nodiscard]] uint32_t latencySamples() const { return latencySamples_; }

  bool queueParameterChange(vst3::ParamID id, double normalized) {
    normalized = std::max(0.0, std::min(1.0, normalized));
    int32_t queueIndex = -1;
    auto* queue = static_cast<detail::ParameterValueQueue*>(
        inputParameterChanges_.vtbl->addParameterData(&inputParameterChanges_, &id, &queueIndex));
    return queue != nullptr && queue->vtbl->addPoint(queue, 0, normalized, nullptr) == vst3::kResultOk;
  }

  // ---- A2 param bridge (raw IEditController calls; null-safe) -------------
  [[nodiscard]] bool hasController() const { return controller_ != nullptr; }

  [[nodiscard]] int32_t parameterCount() const {
    return controller_ != nullptr ? controller_->vtbl->getParameterCount(controller_) : 0;
  }

  bool parameterInfoAt(int32_t index, vst3::ParameterInfo* info) const {
    return controller_ != nullptr && info != nullptr &&
           controller_->vtbl->getParameterInfo(controller_, index, info) == vst3::kResultOk;
  }

  [[nodiscard]] double paramNormalized(vst3::ParamID id) const {
    return controller_ != nullptr ? controller_->vtbl->getParamNormalized(controller_, id) : 0.0;
  }

  // Host authority for slider edits: the CONTROLLER takes the value (so an
  // open editor's knob follows) and the same value is queued as an input
  // parameter change for the next process() call (so the DSP hears it — the
  // controller and processor are separate objects in the VST3 model).
  bool setParamNormalized(vst3::ParamID id, double normalized) {
    if (controller_ == nullptr) return false;
    normalized = std::max(0.0, std::min(1.0, normalized));
    controller_->vtbl->setParamNormalized(controller_, id, normalized);
    queueParameterChange(id, normalized);
    return true;
  }

  // Plugin-formatted display string for a normalized value ("" when the
  // plugin declines). ASCII-folded like every other block string.
  [[nodiscard]] std::string paramDisplay(vst3::ParamID id, double normalized) const {
    if (controller_ == nullptr) return {};
    char16_t text[128] = {};
    if (controller_->vtbl->getParamStringByValue(controller_, id, normalized, text) != vst3::kResultOk) {
      return {};
    }
    return detail::utf16ToUtf8Ascii(text, 128);
  }

  // ---- A2 state persistence (IComponent::get/setState over MemoryStream) --
  bool getComponentState(std::vector<uint8_t>* out, std::string* outError) {
    if (component_ == nullptr || out == nullptr) {
      if (outError != nullptr) *outError = "no component loaded";
      return false;
    }
    out->clear();
    detail::MemoryStream stream = detail::makeMemoryStream(out);
    const vst3::tresult result = component_->vtbl->getState(component_, &stream);
    if (result != vst3::kResultOk) {
      if (outError != nullptr) *outError = "IComponent::getState failed (0x" + hex(result) + ")";
      return false;
    }
    return true;
  }

  bool setComponentState(const std::vector<uint8_t>& data, std::string* outError) {
    if (component_ == nullptr) {
      if (outError != nullptr) *outError = "no component loaded";
      return false;
    }
    std::vector<uint8_t> mutableData = data;  // stream API is non-const
    detail::MemoryStream stream = detail::makeMemoryStream(&mutableData);
    const vst3::tresult result = component_->vtbl->setState(component_, &stream);
    if (result != vst3::kResultOk) {
      if (outError != nullptr) *outError = "IComponent::setState failed (0x" + hex(result) + ")";
      return false;
    }
    // The controller mirrors the restored processor state so the editor and
    // the published param values reflect it (VST3 setComponentState contract).
    if (controller_ != nullptr) {
      stream.cursor = 0;
      controller_->vtbl->setComponentState(controller_, &stream);
    }
    return true;
  }

#ifdef _WIN32
  bool showEditor(const std::string& title);
  void closeEditor();
  void pumpEditorMessages();
  vst3::tresult resizeEditor(vst3::ViewRect* requested);
#else
  bool showEditor(const std::string&) { return false; }
  void closeEditor() {}
  void pumpEditorMessages() {}
  vst3::tresult resizeEditor(vst3::ViewRect*) { return vst3::kNotImplemented; }
#endif
  // True while an editor window exists (the serve loop uses the open→closed
  // transition to publish the idle editor status after a user close).
  [[nodiscard]] bool editorOpen() const { return editorWindow_ != nullptr; }

  bool start(vst3::IPluginFactory* factory, const std::string& className,
             const VstProcessorConfig& config = {}) {
    using namespace corevideo::vst3;
    if (started_) {
      return true;
    }
    lastError_.clear();

    PClassInfo classInfo{};
    std::string findError;
    if (!findAudioClass(factory, className, &classInfo, &findError)) {
      return fail(findError);
    }
    activeClassName_ = std::string(classInfo.name, strnlen(classInfo.name, sizeof(classInfo.name)));

    void* rawComponent = nullptr;
    const tresult created =
        factory->vtbl->createInstance(factory, classInfo.cid, kIComponentIid.bytes, &rawComponent);
    if (created != kResultOk || rawComponent == nullptr) {
      return fail("createInstance('" + activeClassName_ + "') failed (0x" + hex(created) + ")");
    }
    component_ = static_cast<IComponent*>(rawComponent);

    // Canonical order: setIoMode BEFORE initialize; many plugins return
    // kNotImplemented — that is fine, the call itself is the contract.
    component_->vtbl->setIoMode(component_, kIoAdvanced);

    const tresult initialized = component_->vtbl->initialize(component_, &hostContext_);
    if (initialized != kResultOk) {
      return fail("IComponent::initialize failed (0x" + hex(initialized) + ")");
    }
    initialized_ = true;

    // The edit controller owns the plug-in's professional native GUI and
    // reports parameter gestures back through IComponentHandler. Keep it in
    // this isolated process beside the exact processor instance it controls.
    void* rawController = nullptr;
    if (component_->vtbl->queryInterface(component_, kIEditControllerIid.bytes, &rawController) == kResultOk &&
        rawController != nullptr) {
      controller_ = static_cast<IEditController*>(rawController);
    } else {
      char controllerClassId[16] = {};
      if (component_->vtbl->getControllerClassId(component_, controllerClassId) == kResultOk &&
          factory->vtbl->createInstance(factory, controllerClassId, kIEditControllerIid.bytes,
                                        &rawController) == kResultOk && rawController != nullptr) {
        controller_ = static_cast<IEditController*>(rawController);
        if (controller_->vtbl->initialize(controller_, &hostContext_) == kResultOk) {
          controllerInitialized_ = true;
        } else {
          controller_->vtbl->release(controller_);
          controller_ = nullptr;
        }
      }
    }
    if (controller_ != nullptr) {
      controller_->vtbl->setComponentHandler(controller_, &componentHandler_);
    }

    void* rawProcessor = nullptr;
    if (component_->vtbl->queryInterface(component_, kIAudioProcessorIid.bytes, &rawProcessor) != kResultOk ||
        rawProcessor == nullptr) {
      return fail("plugin exposes no IAudioProcessor");
    }
    processor_ = static_cast<IAudioProcessor*>(rawProcessor);

    if (processor_->vtbl->canProcessSampleSize(processor_, kSample32) != kResultOk) {
      return fail("plugin cannot process 32-bit float samples");
    }

    // Bus topology: require a stereo main input and output (insert-effect
    // shape). Aux buses (sidechains) keep their default arrangement and are
    // fed silence.
    inputBusCount_ = component_->vtbl->getBusCount(component_, kAudio, kInput);
    outputBusCount_ = component_->vtbl->getBusCount(component_, kAudio, kOutput);
    if (inputBusCount_ < 1 || outputBusCount_ < 1) {
      return fail("plugin is not an insert effect (audio buses in=" + std::to_string(inputBusCount_) +
                  " out=" + std::to_string(outputBusCount_) + ")");
    }

    std::vector<SpeakerArrangement> inputArrangements(static_cast<size_t>(inputBusCount_), kSpeakerStereo);
    std::vector<SpeakerArrangement> outputArrangements(static_cast<size_t>(outputBusCount_), kSpeakerStereo);
    for (int32_t bus = 1; bus < inputBusCount_; ++bus) {
      processor_->vtbl->getBusArrangement(processor_, kInput, bus, &inputArrangements[static_cast<size_t>(bus)]);
    }
    for (int32_t bus = 1; bus < outputBusCount_; ++bus) {
      processor_->vtbl->getBusArrangement(processor_, kOutput, bus, &outputArrangements[static_cast<size_t>(bus)]);
    }
    processor_->vtbl->setBusArrangements(processor_, inputArrangements.data(), inputBusCount_,
                                         outputArrangements.data(), outputBusCount_);
    // Regardless of the setBusArrangements verdict, what matters is the
    // arrangement the plugin actually settled on for the MAIN buses.
    SpeakerArrangement mainIn = 0;
    SpeakerArrangement mainOut = 0;
    processor_->vtbl->getBusArrangement(processor_, kInput, 0, &mainIn);
    processor_->vtbl->getBusArrangement(processor_, kOutput, 0, &mainOut);
    if (mainIn != kSpeakerStereo || mainOut != kSpeakerStereo) {
      return fail("plugin refused stereo main buses (in=0x" + hex(static_cast<int64_t>(mainIn)) +
                  " out=0x" + hex(static_cast<int64_t>(mainOut)) + ")");
    }

    ProcessSetup setup{};
    setup.processMode = kRealtime;
    setup.symbolicSampleSize = kSample32;
    setup.maxSamplesPerBlock = config.maxBlockFrames;
    setup.sampleRate = config.sampleRate;
    const tresult setupResult = processor_->vtbl->setupProcessing(processor_, &setup);
    if (setupResult != kResultOk) {
      return fail("setupProcessing failed (0x" + hex(setupResult) + ")");
    }
    maxBlockFrames_ = config.maxBlockFrames;

    // Activate the main buses; aux buses stay in their default state.
    // Verdicts are advisory here (some plugins report kResultFalse for
    // already-active buses) — the process() call is the real gate.
    component_->vtbl->activateBus(component_, kAudio, kInput, 0, 1);
    component_->vtbl->activateBus(component_, kAudio, kOutput, 0, 1);

    const tresult active = component_->vtbl->setActive(component_, 1);
    if (active != kResultOk) {
      return fail("setActive(true) failed (0x" + hex(active) + ")");
    }
    active_ = true;

    // Many plugins return kNotImplemented from setProcessing — tolerated.
    processor_->vtbl->setProcessing(processor_, 1);
    processing_ = true;

    latencySamples_ = processor_->vtbl->getLatencySamples(processor_);

    allocateBuffers();
    if (inputChannels_.empty() || inputChannels_[0].size() < 2 ||
        outputChannels_.empty() || outputChannels_[0].size() < 2) {
      return fail("main bus reported fewer than 2 channels after stereo arrangement");
    }
    started_ = true;
    return true;
  }

  // In-place processing of interleaved stereo float PCM (the host-transport
  // block format). Returns false — with the caller's audio untouched — on any
  // failure; sets lastError() once per distinct failure.
  bool processInterleavedStereo(float* pcm, int32_t sampleCount) {
    using namespace corevideo::vst3;
    if (!started_ || pcm == nullptr || sampleCount < 2) {
      return false;
    }
    int32_t frames = sampleCount / 2;
    if (frames > maxBlockFrames_) {
      frames = maxBlockFrames_;  // never overrun the negotiated max block
    }

    float* inLeft = inputChannels_[0][0];
    float* inRight = inputChannels_[0][1];
    for (int32_t frame = 0; frame < frames; ++frame) {
      inLeft[frame] = pcm[frame * 2];
      inRight[frame] = pcm[frame * 2 + 1];
    }

    ProcessData data{};
    data.processMode = kRealtime;
    data.symbolicSampleSize = kSample32;
    data.numSamples = frames;
    data.numInputs = inputBusCount_;
    data.numOutputs = outputBusCount_;
    data.inputs = inputBuses_.data();
    data.outputs = outputBuses_.data();
    data.inputParameterChanges = &inputParameterChanges_;
    data.outputParameterChanges = &outputParameterChanges_;

    const tresult processed = processor_->vtbl->process(processor_, &data);
    inputParameterChanges_.count = 0;
    if (processed != kResultOk) {
      fail("process() returned 0x" + hex(processed));
      return false;
    }

    // Finite-output guard (same posture as the probe spec): NaN/Inf never
    // reaches the mix — bypass instead.
    const float* outLeft = outputChannels_[0][0];
    const float* outRight = outputChannels_[0][1];
    for (int32_t frame = 0; frame < frames; ++frame) {
      if (!std::isfinite(outLeft[frame]) || !std::isfinite(outRight[frame])) {
        fail("plugin produced non-finite output");
        return false;
      }
    }

    for (int32_t frame = 0; frame < frames; ++frame) {
      pcm[frame * 2] = outLeft[frame];
      pcm[frame * 2 + 1] = outRight[frame];
    }
    return true;
  }

  void shutdown() {
    started_ = false;
    closeEditor();
    if (processor_ != nullptr) {
      if (processing_) {
        processor_->vtbl->setProcessing(processor_, 0);
        processing_ = false;
      }
    }
    if (component_ != nullptr && active_) {
      component_->vtbl->setActive(component_, 0);
      active_ = false;
    }
    if (component_ != nullptr && initialized_) {
      component_->vtbl->terminate(component_);
      initialized_ = false;
    }
    if (controller_ != nullptr) {
      controller_->vtbl->setComponentHandler(controller_, nullptr);
      if (controllerInitialized_) controller_->vtbl->terminate(controller_);
      controller_->vtbl->release(controller_);
      controller_ = nullptr;
      controllerInitialized_ = false;
    }
    if (processor_ != nullptr) {
      processor_->vtbl->release(processor_);
      processor_ = nullptr;
    }
    if (component_ != nullptr) {
      component_->vtbl->release(component_);
      component_ = nullptr;
    }
  }

 private:
  bool fail(const std::string& message) {
    lastError_ = message;
    return false;
  }

  static std::string hex(int64_t value) {
    char buffer[24];
    std::snprintf(buffer, sizeof(buffer), "%llx", static_cast<unsigned long long>(value));
    return buffer;
  }

  void allocateBuffers() {
    using namespace corevideo::vst3;
    const auto setupBuses = [this](int32_t busCount, int32_t direction,
                                   std::vector<AudioBusBuffers>& buses,
                                   std::vector<std::vector<float*>>& channels) {
      buses.assign(static_cast<size_t>(busCount), AudioBusBuffers{});
      channels.assign(static_cast<size_t>(busCount), {});
      for (int32_t bus = 0; bus < busCount; ++bus) {
        BusInfo info{};
        int32_t channelCount = 2;
        if (component_->vtbl->getBusInfo(component_, kAudio, direction, bus, &info) == kResultOk &&
            info.channelCount > 0) {
          channelCount = info.channelCount;
        }
        auto& busChannels = channels[static_cast<size_t>(bus)];
        busChannels.resize(static_cast<size_t>(channelCount), nullptr);
        for (int32_t channel = 0; channel < channelCount; ++channel) {
          // unique_ptr<float[]> storage: the arrays never move even when the
          // storage vector reallocates, so the channel pointers stay valid.
          channelStorage_.push_back(std::make_unique<float[]>(static_cast<size_t>(maxBlockFrames_)));
          busChannels[static_cast<size_t>(channel)] = channelStorage_.back().get();
        }
        auto& busBuffers = buses[static_cast<size_t>(bus)];
        busBuffers.numChannels = channelCount;
        busBuffers.channelBuffers32 = busChannels.data();
        // Aux buses stay silent forever — flag them so plugins can skip work.
        busBuffers.silenceFlags = bus == 0 ? 0 : ~static_cast<uint64_t>(0);
      }
    };
    channelStorage_.clear();
    setupBuses(inputBusCount_, vst3::kInput, inputBuses_, inputChannels_);
    setupBuses(outputBusCount_, vst3::kOutput, outputBuses_, outputChannels_);
  }

  HostApplicationContext hostContext_{};
  detail::ParameterChanges inputParameterChanges_{detail::parameterChangesVtbl()};
  detail::ParameterChanges outputParameterChanges_{detail::parameterChangesVtbl()};

  vst3::IComponent* component_ = nullptr;
  vst3::IAudioProcessor* processor_ = nullptr;
  vst3::IEditController* controller_ = nullptr;
  vst3::IPlugView* editorView_ = nullptr;
  ComponentHandler componentHandler_{};
  PlugFrame plugFrame_{};
  void* editorWindow_ = nullptr;
  bool initialized_ = false;
  bool active_ = false;
  bool processing_ = false;
  bool controllerInitialized_ = false;
  bool editorAttached_ = false;
  bool started_ = false;
  int32_t inputBusCount_ = 0;
  int32_t outputBusCount_ = 0;
  int32_t maxBlockFrames_ = 0;
  uint32_t latencySamples_ = 0;
  std::string activeClassName_;
  std::string lastError_;

  std::vector<vst3::AudioBusBuffers> inputBuses_;
  std::vector<vst3::AudioBusBuffers> outputBuses_;
  std::vector<std::vector<float*>> inputChannels_;
  std::vector<std::vector<float*>> outputChannels_;
  std::vector<std::unique_ptr<float[]>> channelStorage_;
};

#ifdef _WIN32
inline LRESULT CALLBACK vstEditorWindowProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam) {
  auto* owner = reinterpret_cast<VstPluginInstance*>(::GetWindowLongPtrW(hwnd, GWLP_USERDATA));
  if (message == WM_NCCREATE) {
    const auto* create = reinterpret_cast<CREATESTRUCTW*>(lParam);
    owner = static_cast<VstPluginInstance*>(create->lpCreateParams);
    ::SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(owner));
  }
  switch (message) {
    case WM_CLOSE:
      // A1 lifecycle: closing the window DETACHES cleanly — removed() before
      // DestroyWindow (the VST3 contract) — instead of hiding with the view
      // still attached. The serve loop notices editorOpen() flipped and
      // publishes the idle editor status so the shell chip stays truthful.
      if (owner != nullptr) {
        owner->closeEditor();
      } else {
        ::DestroyWindow(hwnd);
      }
      return 0;
    case WM_SETFOCUS:
      return 0;
    default:
      return ::DefWindowProcW(hwnd, message, wParam, lParam);
  }
}

// Best-effort raise for a window created by a BACKGROUND process. The serve
// host is spawned by the core with no console and no foreground rights, so
// SetForegroundWindow is routinely denied — Phase 0 diagnosis showed the
// editor opening VISIBLE but foreground=0 at a CW_USEDEFAULT cascade
// position, i.e. BEHIND the maximized operator console: the owner-visible
// symptom "no plugin UI ever appears". A topmost pulse puts the window at the
// top of the z-order even without activation; when the OS still refuses
// foreground, flash the taskbar button so the operator always gets a visible
// signal. Deliberately no AttachThreadInput/input-hook tricks — never steal
// foreground aggressively.
inline void raiseEditorWindowBestEffort(HWND hwnd) {
  ::SetWindowPos(hwnd, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_SHOWWINDOW);
  ::SetWindowPos(hwnd, HWND_NOTOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_SHOWWINDOW);
  ::SetForegroundWindow(hwnd);
  if (::GetForegroundWindow() != hwnd) {
    FLASHWINFO flash{};
    flash.cbSize = sizeof(flash);
    flash.hwnd = hwnd;
    flash.dwFlags = FLASHW_ALL | FLASHW_TIMERNOFG;
    ::FlashWindowEx(&flash);
  }
}

inline bool VstPluginInstance::showEditor(const std::string& title) {
  if (controller_ == nullptr) {
    lastError_ = "plugin exposes no edit controller";
    return false;
  }
  if (editorWindow_ != nullptr) {
    // Second "Open controls": focus the EXISTING window (restoring a minimized
    // one) — never a double createView/attached.
    HWND existing = static_cast<HWND>(editorWindow_);
    ::ShowWindow(existing, ::IsIconic(existing) ? SW_RESTORE : SW_SHOW);
    raiseEditorWindowBestEffort(existing);
    return true;
  }
  editorView_ = static_cast<vst3::IPlugView*>(controller_->vtbl->createView(controller_, "editor"));
  if (editorView_ == nullptr || editorView_->vtbl == nullptr) {
    lastError_ = "plugin does not provide a native editor";
    editorView_ = nullptr;
    return false;
  }
  if (editorView_->vtbl->isPlatformTypeSupported(editorView_, "HWND") != vst3::kResultOk) {
    lastError_ = "plugin editor does not support HWND";
    editorView_->vtbl->release(editorView_);
    editorView_ = nullptr;
    return false;
  }
  vst3::ViewRect viewRect{};
  if (editorView_->vtbl->getSize(editorView_, &viewRect) != vst3::kResultOk) {
    viewRect.right = 800;
    viewRect.bottom = 600;
  }
  const int width = std::max(160, viewRect.right - viewRect.left);
  const int height = std::max(120, viewRect.bottom - viewRect.top);
  static const wchar_t* kClassName = L"CoreVideoProVst3EditorHost";
  static ATOM atom = 0;
  if (atom == 0) {
    WNDCLASSW wc{};
    wc.lpfnWndProc = &vstEditorWindowProc;
    wc.hInstance = ::GetModuleHandleW(nullptr);
    wc.hCursor = ::LoadCursorW(nullptr, MAKEINTRESOURCEW(32512));
    wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
    wc.lpszClassName = kClassName;
    atom = ::RegisterClassW(&wc);
    if (atom == 0 && ::GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
      lastError_ = "could not register editor host window";
      editorView_->vtbl->release(editorView_);
      editorView_ = nullptr;
      return false;
    }
  }
  RECT outer{0, 0, width, height};
  const DWORD style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX;
  ::AdjustWindowRect(&outer, style, FALSE);
  // Sane placement instead of the CW_USEDEFAULT cascade: centered on the
  // primary monitor's work area (clamped so the title bar always stays
  // reachable). The operator's console is usually maximized there — center is
  // where an opened panel is expected.
  RECT workArea{0, 0, 1920, 1080};
  ::SystemParametersInfoW(SPI_GETWORKAREA, 0, &workArea, 0);
  const int outerWidth = outer.right - outer.left;
  const int outerHeight = outer.bottom - outer.top;
  const int posX = std::max<int>(workArea.left,
                                 workArea.left + (workArea.right - workArea.left - outerWidth) / 2);
  const int posY = std::max<int>(workArea.top,
                                 workArea.top + (workArea.bottom - workArea.top - outerHeight) / 2);
  std::wstring wideTitle(title.begin(), title.end());
  HWND hwnd = ::CreateWindowExW(0, kClassName, wideTitle.c_str(), style,
                                posX, posY, outerWidth, outerHeight,
                                nullptr, nullptr, ::GetModuleHandleW(nullptr), this);
  if (hwnd == nullptr) {
    lastError_ = "could not create editor host window";
    editorView_->vtbl->release(editorView_);
    editorView_ = nullptr;
    return false;
  }
  editorWindow_ = hwnd;
  editorView_->vtbl->setFrame(editorView_, &plugFrame_);
  if (editorView_->vtbl->attached(editorView_, hwnd, "HWND") != vst3::kResultOk) {
    lastError_ = "plugin editor failed to attach";
    closeEditor();
    return false;
  }
  editorAttached_ = true;
  ::ShowWindow(hwnd, SW_SHOWNORMAL);
  ::UpdateWindow(hwnd);
  raiseEditorWindowBestEffort(hwnd);
  return true;
}

inline void VstPluginInstance::closeEditor() {
  if (editorView_ != nullptr && editorAttached_) {
    editorView_->vtbl->removed(editorView_);
    editorAttached_ = false;
  }
  if (editorView_ != nullptr) {
    editorView_->vtbl->setFrame(editorView_, nullptr);
    editorView_->vtbl->release(editorView_);
    editorView_ = nullptr;
  }
  if (editorWindow_ != nullptr) {
    ::DestroyWindow(static_cast<HWND>(editorWindow_));
    editorWindow_ = nullptr;
  }
}

inline void VstPluginInstance::pumpEditorMessages() {
  MSG message{};
  while (::PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
    ::TranslateMessage(&message);
    ::DispatchMessageW(&message);
  }
}

inline vst3::tresult VstPluginInstance::resizeEditor(vst3::ViewRect* requested) {
  if (requested == nullptr || editorWindow_ == nullptr || editorView_ == nullptr) return vst3::kResultFalse;
  const int width = std::max(160, requested->right - requested->left);
  const int height = std::max(120, requested->bottom - requested->top);
  RECT outer{0, 0, width, height};
  const DWORD style = static_cast<DWORD>(::GetWindowLongPtrW(static_cast<HWND>(editorWindow_), GWL_STYLE));
  ::AdjustWindowRect(&outer, style, FALSE);
  ::SetWindowPos(static_cast<HWND>(editorWindow_), nullptr, 0, 0,
                 outer.right - outer.left, outer.bottom - outer.top,
                 SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
  return editorView_->vtbl->onSize(editorView_, requested);
}
#endif

}  // namespace corevideo::vsthost
