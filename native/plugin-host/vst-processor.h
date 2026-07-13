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

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

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
struct EmptyParameterChanges {
  vst3::IParameterChangesVtbl* vtbl;
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

inline int32_t CVST_API paramChangesCount(void*) { return 0; }
inline void* CVST_API paramChangesGetData(void*, int32_t) { return nullptr; }
inline void* CVST_API paramChangesAddData(void*, const vst3::ParamID*, int32_t*) { return nullptr; }

inline vst3::IParameterChangesVtbl* emptyParameterChangesVtbl() {
  static vst3::IParameterChangesVtbl vtbl = {
      &paramChangesQueryInterface, &staticAddRef, &staticRelease,
      &paramChangesCount, &paramChangesGetData, &paramChangesAddData,
  };
  return &vtbl;
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

class VstPluginInstance {
 public:
  VstPluginInstance() { hostContext_.vtbl = detail::hostApplicationVtbl(); }
  ~VstPluginInstance() { shutdown(); }
  VstPluginInstance(const VstPluginInstance&) = delete;
  VstPluginInstance& operator=(const VstPluginInstance&) = delete;

  [[nodiscard]] bool started() const { return started_; }
  [[nodiscard]] const std::string& lastError() const { return lastError_; }
  [[nodiscard]] const std::string& activeClassName() const { return activeClassName_; }
  [[nodiscard]] uint32_t latencySamples() const { return latencySamples_; }

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
  detail::EmptyParameterChanges inputParameterChanges_{detail::emptyParameterChangesVtbl()};
  detail::EmptyParameterChanges outputParameterChanges_{detail::emptyParameterChangesVtbl()};

  vst3::IComponent* component_ = nullptr;
  vst3::IAudioProcessor* processor_ = nullptr;
  bool initialized_ = false;
  bool active_ = false;
  bool processing_ = false;
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

}  // namespace corevideo::vsthost
