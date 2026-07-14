// VST P2c: the raw COM-ABI machinery (vst-abi.h / vst-processor.h) proven
// with an injected FAKE factory — the same injection pattern the probe uses:
// no LoadLibrary, no VST3 SDK, the fake implements the exact vtables a real
// plugin module exports, so every call the host makes into a plugin (and its
// argument marshalling) is exercised in-process.
//
// Layout notes: the ABI structs also carry compile-time static_asserts in
// vst-abi.h; the runtime checks here just make the contract visible in test
// output.

#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "vst-abi.h"
#include "vst-processor.h"
#include "core/PluginHostScan.h"

namespace {

using namespace corevideo::vst3;

// ---------------------------------------------------------------------------
// Fake plugin: one audio class ("Fake Gain Stereo") applying L*2 / R*0.5 —
// asymmetric on purpose, so a swapped deinterleave/reinterleave fails loudly.
// ---------------------------------------------------------------------------
struct FakeState;

struct FakeComponent {
  IComponentVtbl* vtbl;
  FakeState* state;
};

struct FakeProcessor {
  IAudioProcessorVtbl* vtbl;
  FakeState* state;
};

struct FakeState {
  std::vector<std::string> log;
  bool failInitialize = false;
  bool failCreateInstance = false;
  tresult processResult = kResultOk;
  bool produceNan = false;
  SpeakerArrangement mainArrangement = kSpeakerStereo;  // what getBusArrangement reports
  double setupSampleRate = 0.0;
  int32_t setupMaxBlock = 0;
  uint32_t latency = 7;
  float leftGain = 2.0f;
  float rightGain = 0.5f;
  FakeComponent* component = nullptr;
  FakeProcessor* processor = nullptr;
};

constexpr char kFakeCid[16] = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16};

// --- component vtable ---
tresult CVST_API fakeComponentQueryInterface(void* self, const char* iid, void** obj) {
  auto* component = static_cast<FakeComponent*>(self);
  if (tuidEqual(iid, kIAudioProcessorIid.bytes)) {
    component->state->log.push_back("queryInterface:IAudioProcessor");
    *obj = component->state->processor;
    return kResultOk;
  }
  if (tuidEqual(iid, kFUnknownIid.bytes) || tuidEqual(iid, kIComponentIid.bytes)) {
    *obj = self;
    return kResultOk;
  }
  *obj = nullptr;
  return kNoInterface;
}
uint32_t CVST_API fakeAddRef(void*) { return 1; }
uint32_t CVST_API fakeRelease(void*) { return 1; }
tresult CVST_API fakeInitialize(void* self, void* hostContext) {
  auto* component = static_cast<FakeComponent*>(self);
  component->state->log.push_back("initialize");
  if (component->state->failInitialize) {
    return kResultFalse;
  }
  // The host must pass a context that answers IHostApplication.
  if (hostContext == nullptr) {
    return kResultFalse;
  }
  auto* unknown = static_cast<corevideo::vsthost::HostApplicationContext*>(hostContext);
  void* hostApp = nullptr;
  if (unknown->vtbl->queryInterface(hostContext, kIHostApplicationIid.bytes, &hostApp) != kResultOk ||
      hostApp == nullptr) {
    return kResultFalse;
  }
  char16_t name[128] = {};
  unknown->vtbl->getName(hostApp, name);
  if (name[0] == 0) {
    return kResultFalse;
  }
  return kResultOk;
}
tresult CVST_API fakeTerminate(void* self) {
  static_cast<FakeComponent*>(self)->state->log.push_back("terminate");
  return kResultOk;
}
tresult CVST_API fakeGetControllerClassId(void*, char*) { return kResultFalse; }
tresult CVST_API fakeSetIoMode(void* self, int32_t mode) {
  static_cast<FakeComponent*>(self)->state->log.push_back("setIoMode:" + std::to_string(mode));
  return kNotImplemented;  // common in real plugins; the host must tolerate it
}
int32_t CVST_API fakeGetBusCount(void*, int32_t mediaType, int32_t) {
  return mediaType == kAudio ? 1 : 0;
}
tresult CVST_API fakeGetBusInfo(void*, int32_t mediaType, int32_t direction, int32_t index, BusInfo* bus) {
  if (mediaType != kAudio || index != 0 || bus == nullptr) {
    return kResultFalse;
  }
  bus->mediaType = mediaType;
  bus->direction = direction;
  bus->channelCount = 2;
  bus->busType = kMain;
  bus->flags = 1;
  return kResultOk;
}
tresult CVST_API fakeGetRoutingInfo(void*, RoutingInfo*, RoutingInfo*) { return kResultFalse; }
tresult CVST_API fakeActivateBus(void* self, int32_t, int32_t direction, int32_t index, TBool state) {
  static_cast<FakeComponent*>(self)->state->log.push_back(
      "activateBus:" + std::to_string(direction) + ":" + std::to_string(index) + ":" + std::to_string(state));
  return kResultOk;
}
tresult CVST_API fakeSetActive(void* self, TBool state) {
  static_cast<FakeComponent*>(self)->state->log.push_back("setActive:" + std::to_string(state));
  return kResultOk;
}
tresult CVST_API fakeSetState(void*, void*) { return kResultOk; }
tresult CVST_API fakeGetState(void*, void*) { return kResultOk; }

IComponentVtbl* fakeComponentVtbl() {
  static IComponentVtbl vtbl = {
      &fakeComponentQueryInterface, &fakeAddRef, &fakeRelease,
      &fakeInitialize, &fakeTerminate,
      &fakeGetControllerClassId, &fakeSetIoMode, &fakeGetBusCount, &fakeGetBusInfo,
      &fakeGetRoutingInfo, &fakeActivateBus, &fakeSetActive, &fakeSetState, &fakeGetState,
  };
  return &vtbl;
}

// --- processor vtable ---
tresult CVST_API fakeProcessorQueryInterface(void* self, const char* iid, void** obj) {
  auto* processor = static_cast<FakeProcessor*>(self);
  if (tuidEqual(iid, kIAudioProcessorIid.bytes) || tuidEqual(iid, kFUnknownIid.bytes)) {
    *obj = self;
    return kResultOk;
  }
  if (tuidEqual(iid, kIComponentIid.bytes)) {
    *obj = processor->state->component;
    return kResultOk;
  }
  *obj = nullptr;
  return kNoInterface;
}
tresult CVST_API fakeSetBusArrangements(void* self, SpeakerArrangement* inputs, int32_t numIns,
                                        SpeakerArrangement* outputs, int32_t numOuts) {
  auto* processor = static_cast<FakeProcessor*>(self);
  processor->state->log.push_back("setBusArrangements:" + std::to_string(numIns) + ":" +
                                  std::to_string(numOuts));
  if (numIns != 1 || numOuts != 1 || inputs == nullptr || outputs == nullptr) {
    return kResultFalse;
  }
  if (inputs[0] != kSpeakerStereo || outputs[0] != kSpeakerStereo) {
    return kResultFalse;
  }
  return kResultOk;
}
tresult CVST_API fakeGetBusArrangement(void* self, int32_t, int32_t index, SpeakerArrangement* arr) {
  if (index != 0 || arr == nullptr) {
    return kResultFalse;
  }
  *arr = static_cast<FakeProcessor*>(self)->state->mainArrangement;
  return kResultOk;
}
tresult CVST_API fakeCanProcessSampleSize(void*, int32_t symbolicSampleSize) {
  return symbolicSampleSize == kSample32 ? kResultOk : kResultFalse;
}
uint32_t CVST_API fakeGetLatencySamples(void* self) {
  return static_cast<FakeProcessor*>(self)->state->latency;
}
tresult CVST_API fakeSetupProcessing(void* self, ProcessSetup* setup) {
  auto* processor = static_cast<FakeProcessor*>(self);
  processor->state->log.push_back("setupProcessing");
  if (setup == nullptr || setup->symbolicSampleSize != kSample32 || setup->processMode != kRealtime) {
    return kResultFalse;
  }
  processor->state->setupSampleRate = setup->sampleRate;
  processor->state->setupMaxBlock = setup->maxSamplesPerBlock;
  return kResultOk;
}
tresult CVST_API fakeSetProcessing(void* self, TBool state) {
  static_cast<FakeProcessor*>(self)->state->log.push_back("setProcessing:" + std::to_string(state));
  return kResultOk;
}
tresult CVST_API fakeProcess(void* self, ProcessData* data) {
  auto* processor = static_cast<FakeProcessor*>(self);
  FakeState& state = *processor->state;
  if (state.processResult != kResultOk) {
    return state.processResult;
  }
  if (data == nullptr || data->numInputs != 1 || data->numOutputs != 1 ||
      data->inputs == nullptr || data->outputs == nullptr ||
      data->inputs[0].numChannels != 2 || data->outputs[0].numChannels != 2 ||
      data->inputParameterChanges == nullptr) {
    return kResultFalse;
  }
  const float* inLeft = data->inputs[0].channelBuffers32[0];
  const float* inRight = data->inputs[0].channelBuffers32[1];
  float* outLeft = data->outputs[0].channelBuffers32[0];
  float* outRight = data->outputs[0].channelBuffers32[1];
  for (int32_t frame = 0; frame < data->numSamples; ++frame) {
    outLeft[frame] = state.produceNan ? std::nanf("") : inLeft[frame] * state.leftGain;
    outRight[frame] = inRight[frame] * state.rightGain;
  }
  return kResultOk;
}
uint32_t CVST_API fakeGetTailSamples(void*) { return 0; }

IAudioProcessorVtbl* fakeProcessorVtbl() {
  static IAudioProcessorVtbl vtbl = {
      &fakeProcessorQueryInterface, &fakeAddRef, &fakeRelease,
      &fakeSetBusArrangements, &fakeGetBusArrangement, &fakeCanProcessSampleSize,
      &fakeGetLatencySamples, &fakeSetupProcessing, &fakeSetProcessing, &fakeProcess,
      &fakeGetTailSamples,
  };
  return &vtbl;
}

// --- factory vtable ---
struct FakeFactory {
  IPluginFactoryVtbl* vtbl;
  FakeState* state;
};

tresult CVST_API fakeFactoryQueryInterface(void* self, const char*, void** obj) {
  *obj = self;
  return kResultOk;
}
tresult CVST_API fakeGetFactoryInfo(void*, PFactoryInfo* info) {
  std::snprintf(info->vendor, sizeof(info->vendor), "FakeVendor");
  return kResultOk;
}
int32_t CVST_API fakeCountClasses(void*) { return 2; }
tresult CVST_API fakeGetClassInfo(void*, int32_t index, PClassInfo* info) {
  std::memset(info, 0, sizeof(*info));
  if (index == 0) {
    // A controller class the host must SKIP (not an audio class).
    std::snprintf(info->category, sizeof(info->category), "Component Controller Class");
    std::snprintf(info->name, sizeof(info->name), "Fake Gain Controller");
    return kResultOk;
  }
  if (index == 1) {
    std::memcpy(info->cid, kFakeCid, sizeof(kFakeCid));
    std::snprintf(info->category, sizeof(info->category), "Audio Module Class");
    std::snprintf(info->name, sizeof(info->name), "Fake Gain Stereo");
    return kResultOk;
  }
  return kResultFalse;
}
tresult CVST_API fakeCreateInstance(void* self, const char* cid, const char* iid, void** obj) {
  auto* factory = static_cast<FakeFactory*>(self);
  factory->state->log.push_back("createInstance");
  if (factory->state->failCreateInstance) {
    return kResultFalse;
  }
  if (std::memcmp(cid, kFakeCid, 16) != 0 || !tuidEqual(iid, kIComponentIid.bytes)) {
    *obj = nullptr;
    return kNoInterface;
  }
  *obj = factory->state->component;
  return kResultOk;
}

IPluginFactoryVtbl* fakeFactoryVtbl() {
  static IPluginFactoryVtbl vtbl = {
      &fakeFactoryQueryInterface, &fakeAddRef, &fakeRelease,
      &fakeGetFactoryInfo, &fakeCountClasses, &fakeGetClassInfo, &fakeCreateInstance,
  };
  return &vtbl;
}

// Wires a full fake plugin around one FakeState.
struct FakePlugin {
  FakeState state;
  FakeComponent component{fakeComponentVtbl(), &state};
  FakeProcessor processor{fakeProcessorVtbl(), &state};
  FakeFactory factory{fakeFactoryVtbl(), &state};
  FakePlugin() {
    state.component = &component;   // createInstance returns this
    state.processor = &processor;   // component QI(IAudioProcessor) returns this
  }
  IPluginFactory* factoryInterface() { return reinterpret_cast<IPluginFactory*>(&factory); }
};

bool logContains(const std::vector<std::string>& log, const std::string& entry) {
  for (const auto& line : log) {
    if (line == entry) {
      return true;
    }
  }
  return false;
}

}  // namespace

TEST(VstHostAbi, StructLayoutsMatchTheVst3ComAbi) {
  // Compile-time asserted in vst-abi.h; restated here so a failure is visible
  // as a test, not just a build break.
  EXPECT_EQ(sizeof(ProcessSetup), 24u);
  EXPECT_EQ(sizeof(AudioBusBuffers), 24u);
  EXPECT_EQ(sizeof(ProcessData), 80u);
  EXPECT_EQ(sizeof(BusInfo), 276u);
  EXPECT_EQ(offsetof(ProcessData, inputs), 24u);
  EXPECT_EQ(offsetof(BusInfo, busType), 268u);
  // COM byte order for a known TUID: IComponent Data1 0xE831FF31 -> LE.
  EXPECT_EQ(static_cast<unsigned char>(kIComponentIid.bytes[0]), 0x31u);
  EXPECT_EQ(static_cast<unsigned char>(kIComponentIid.bytes[3]), 0xE8u);
  EXPECT_EQ(static_cast<unsigned char>(kIComponentIid.bytes[15]), 0x02u);
}

TEST(VstHostAbi, FullLifecycleProcessesStereoBlocksThroughAFakeFactory) {
  FakePlugin fake;
  corevideo::vsthost::VstPluginInstance instance;
  corevideo::vsthost::VstProcessorConfig config;
  config.sampleRate = 48000.0;
  config.maxBlockFrames = 1024;
  ASSERT_TRUE(instance.start(fake.factoryInterface(), "Fake Gain Stereo", config))
      << instance.lastError();
  EXPECT_EQ(instance.activeClassName(), "Fake Gain Stereo");
  EXPECT_EQ(instance.latencySamples(), 7u);
  EXPECT_TRUE(std::abs(fake.state.setupSampleRate - 48000.0) < 1e-9);
  EXPECT_EQ(fake.state.setupMaxBlock, 1024);

  // The canonical lifecycle actually ran, in a sane order.
  EXPECT_TRUE(logContains(fake.state.log, "createInstance"));
  EXPECT_TRUE(logContains(fake.state.log, "initialize"));
  EXPECT_TRUE(logContains(fake.state.log, "queryInterface:IAudioProcessor"));
  EXPECT_TRUE(logContains(fake.state.log, "setupProcessing"));
  EXPECT_TRUE(logContains(fake.state.log, "setActive:1"));
  EXPECT_TRUE(logContains(fake.state.log, "setProcessing:1"));
  EXPECT_TRUE(logContains(fake.state.log, "activateBus:0:0:1"));
  EXPECT_TRUE(logContains(fake.state.log, "activateBus:1:0:1"));

  // Interleaved stereo in-place processing with ASYMMETRIC channel gains —
  // proves the deinterleave/reinterleave channel mapping.
  std::vector<float> pcm(960 * 2);
  for (int frame = 0; frame < 960; ++frame) {
    pcm[frame * 2] = 0.25f;       // left
    pcm[frame * 2 + 1] = -0.10f;  // right
  }
  ASSERT_TRUE(instance.processInterleavedStereo(pcm.data(), static_cast<int32_t>(pcm.size())));
  EXPECT_TRUE(std::abs(pcm[0] - 0.5f) < 1e-6f);        // 0.25 * leftGain(2.0)
  EXPECT_TRUE(std::abs(pcm[1] - (-0.05f)) < 1e-6f);    // -0.10 * rightGain(0.5)
  EXPECT_TRUE(std::abs(pcm[958 * 2] - 0.5f) < 1e-6f);
  EXPECT_TRUE(std::abs(pcm[958 * 2 + 1] - (-0.05f)) < 1e-6f);

  instance.shutdown();
  EXPECT_TRUE(logContains(fake.state.log, "setProcessing:0"));
  EXPECT_TRUE(logContains(fake.state.log, "setActive:0"));
  EXPECT_TRUE(logContains(fake.state.log, "terminate"));
}

TEST(VstHostAbi, EmptyClassNameSelectsTheFirstAudioClass) {
  FakePlugin fake;
  corevideo::vsthost::VstPluginInstance instance;
  ASSERT_TRUE(instance.start(fake.factoryInterface(), "")) << instance.lastError();
  EXPECT_EQ(instance.activeClassName(), "Fake Gain Stereo");
}

TEST(VstHostAbi, ClassNameMatchIsCaseInsensitiveAndSubstring) {
  FakePlugin fake;
  corevideo::vsthost::VstPluginInstance instance;
  ASSERT_TRUE(instance.start(fake.factoryInterface(), "fake gain")) << instance.lastError();
  EXPECT_EQ(instance.activeClassName(), "Fake Gain Stereo");
}

TEST(VstHostAbi, StartFailsHonestlyWhenTheClassIsMissing) {
  FakePlugin fake;
  corevideo::vsthost::VstPluginInstance instance;
  EXPECT_FALSE(instance.start(fake.factoryInterface(), "No Such Plugin"));
  EXPECT_NE(instance.lastError().find("no audio class"), std::string::npos);
}

TEST(VstHostAbi, StartFailsHonestlyWhenCreateInstanceFails) {
  FakePlugin fake;
  fake.state.failCreateInstance = true;
  corevideo::vsthost::VstPluginInstance instance;
  EXPECT_FALSE(instance.start(fake.factoryInterface(), "Fake Gain Stereo"));
  EXPECT_NE(instance.lastError().find("createInstance"), std::string::npos);
}

TEST(VstHostAbi, StartFailsHonestlyWhenInitializeFails) {
  FakePlugin fake;
  fake.state.failInitialize = true;
  corevideo::vsthost::VstPluginInstance instance;
  EXPECT_FALSE(instance.start(fake.factoryInterface(), "Fake Gain Stereo"));
  EXPECT_NE(instance.lastError().find("initialize"), std::string::npos);
}

TEST(VstHostAbi, StartFailsWhenThePluginRefusesStereo) {
  FakePlugin fake;
  fake.state.mainArrangement = 0x1;  // mono — the host must refuse, not guess
  corevideo::vsthost::VstPluginInstance instance;
  EXPECT_FALSE(instance.start(fake.factoryInterface(), "Fake Gain Stereo"));
  EXPECT_NE(instance.lastError().find("stereo"), std::string::npos);
}

TEST(VstHostAbi, ProcessFailureBypassesWithAudioUntouched) {
  FakePlugin fake;
  corevideo::vsthost::VstPluginInstance instance;
  ASSERT_TRUE(instance.start(fake.factoryInterface(), "Fake Gain Stereo"));
  fake.state.processResult = kResultFalse;
  std::vector<float> pcm(64, 0.33f);
  EXPECT_FALSE(instance.processInterleavedStereo(pcm.data(), static_cast<int32_t>(pcm.size())));
  for (const float sample : pcm) {
    EXPECT_TRUE(std::abs(sample - 0.33f) < 1e-9f);  // bit-identical bypass
  }
  EXPECT_NE(instance.lastError().find("process"), std::string::npos);
}

TEST(VstHostAbi, NonFiniteOutputBypassesWithAudioUntouched) {
  FakePlugin fake;
  corevideo::vsthost::VstPluginInstance instance;
  ASSERT_TRUE(instance.start(fake.factoryInterface(), "Fake Gain Stereo"));
  fake.state.produceNan = true;
  std::vector<float> pcm(64, 0.5f);
  EXPECT_FALSE(instance.processInterleavedStereo(pcm.data(), static_cast<int32_t>(pcm.size())));
  for (const float sample : pcm) {
    EXPECT_TRUE(std::abs(sample - 0.5f) < 1e-9f);
  }
  EXPECT_NE(instance.lastError().find("non-finite"), std::string::npos);
}

// ---------------------------------------------------------------------------
// P2c insert-name selection (core side, pure functions).
// ---------------------------------------------------------------------------
TEST(VstInsertSelection, QueryParsingHonorsTheVstPrefixOnly) {
  using corevideo::core::vstSelectionQueryFromInsertName;
  EXPECT_EQ(vstSelectionQueryFromInsertName("vst:Curves AQ Stereo"), "Curves AQ Stereo");
  EXPECT_EQ(vstSelectionQueryFromInsertName("VST: TDR Nova "), "TDR Nova");
  EXPECT_EQ(vstSelectionQueryFromInsertName("vst"), "");            // legacy test processor
  EXPECT_EQ(vstSelectionQueryFromInsertName("VST3 Bridge Slot"), "");
  EXPECT_EQ(vstSelectionQueryFromInsertName("Host Test Gain"), "");
  EXPECT_EQ(vstSelectionQueryFromInsertName("vst:"), "");
}

TEST(VstInsertSelection, ResolvesClassBundleAndFailureCases) {
  using corevideo::core::PluginHostPluginInfo;
  using corevideo::core::resolveVstInsertSelection;

  std::vector<PluginHostPluginInfo> plugins;
  PluginHostPluginInfo waves;
  waves.id = "C:/Program Files/Common Files/VST3/WaveShell1-VST3 15.11_x64.vst3";
  waves.name = "WaveShell1-VST3 15.11";
  waves.probe = "pass";
  waves.classNames = {"Curves AQ Mono", "Curves AQ Stereo", "MetaFlanger Stereo"};
  plugins.push_back(waves);
  PluginHostPluginInfo broken;
  broken.id = "C:/VST3/Broken.vst3";
  broken.name = "Broken";
  broken.probe = "fail";
  broken.classNames = {"Broken Thing"};
  plugins.push_back(broken);
  PluginHostPluginInfo nova;
  nova.id = "C:/VST3/TDR Nova.vst3";
  nova.name = "TDR Nova";
  nova.probe = "pass";
  nova.classNames = {"TDR Nova"};
  plugins.push_back(nova);

  // Exact class name (case-insensitive) — the WaveShell multi-class case.
  auto selection = resolveVstInsertSelection("curves aq stereo", plugins);
  ASSERT_TRUE(selection.resolved) << selection.error;
  EXPECT_EQ(selection.bundleId, waves.id);
  EXPECT_EQ(selection.className, "Curves AQ Stereo");

  // Substring class match. NOTE: "curves aq" hits "Curves AQ Mono" first
  // (scan order) — exact names are the reliable form; substring is a
  // convenience.
  selection = resolveVstInsertSelection("metaflanger", plugins);
  ASSERT_TRUE(selection.resolved);
  EXPECT_EQ(selection.className, "MetaFlanger Stereo");

  // Explicit bundle/class disambiguation.
  selection = resolveVstInsertSelection("WaveShell/Curves AQ Stereo", plugins);
  ASSERT_TRUE(selection.resolved) << selection.error;
  EXPECT_EQ(selection.bundleId, waves.id);
  EXPECT_EQ(selection.className, "Curves AQ Stereo");

  // Plugin-name match falls back to its first class.
  selection = resolveVstInsertSelection("TDR Nova", plugins);
  ASSERT_TRUE(selection.resolved);
  EXPECT_EQ(selection.bundleId, nova.id);
  EXPECT_EQ(selection.className, "TDR Nova");

  // probe=fail plugins are never selectable.
  selection = resolveVstInsertSelection("Broken Thing", plugins);
  EXPECT_FALSE(selection.resolved);
  EXPECT_NE(selection.error.find("no scanned plugin"), std::string::npos);

  // Unknown name: honest error.
  selection = resolveVstInsertSelection("Does Not Exist", plugins);
  EXPECT_FALSE(selection.resolved);

  // Empty scan: tells the operator to scan.
  selection = resolveVstInsertSelection("Curves AQ Stereo", {});
  EXPECT_FALSE(selection.resolved);
  EXPECT_NE(selection.error.find("scan"), std::string::npos);
}
