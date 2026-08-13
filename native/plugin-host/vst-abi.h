#pragma once

// Raw VST3 COM-ABI declarations (VST spec P2c, docs/vst-host-spec.md).
//
// NO VST3 SDK dependency, by house rule (the SDK is GPLv3/proprietary
// dual-licensed and is NOT vendored). The VST3 module ABI on Windows x64 is
// COM-compatible: interfaces are vtables of __stdcall functions, interface ids
// are 16-byte GUID-layout TUIDs, and the exchange structs are plain-old-data
// with natural (<=16) alignment. These declarations were derived from the
// published binary-interface facts of VST3 (interface ids, vtable order,
// struct field order) the same way the probe's IPluginFactory tables were,
// and are VALIDATED two ways:
//   1. compile-time static_asserts on struct sizes/offsets below (the SDK
//      wraps its interface headers in pack(16) on 64-bit Windows, which is
//      natural alignment for every field used here — verified against the
//      documented layouts);
//   2. live instantiation + processing against an installed third-party
//      plugin via `corevideo-plugin-host --process` (the real proof).
//
// Packing/alignment assumptions verified:
//   - TUID is char[16] in COM GUID byte order (Data1/2/3 little-endian,
//     Data4 as-is) — same order the probe's PClassInfo.cid already reads.
//   - ProcessSetup: 3x int32 then a double at offset 16 (8-byte alignment
//     inserts 4 pad bytes), total 24.
//   - AudioBusBuffers: int32, uint64 at offset 8, pointer union at 16,
//     total 24.
//   - ProcessData: 5x int32 (20 bytes), then 7 pointers starting at offset
//     24, total 80 on x64.
//   - BusInfo: int32 x3, char16_t[128] name, int32 busType, uint32 flags,
//     total 276 (no padding — all fields are <=4-byte aligned).

#include <cstddef>
#include <cstdint>
#include <cstring>

#ifdef _WIN32
#define CVST_API __stdcall
#else
#define CVST_API
#endif

namespace corevideo::vst3 {

using tresult = int32_t;
using TBool = uint8_t;
using SpeakerArrangement = uint64_t;  // bitmask of speaker positions
using ParamID = uint32_t;

// COM-compatible result codes (Windows HRESULT values).
inline constexpr tresult kResultOk = 0;          // S_OK / kResultTrue
inline constexpr tresult kResultFalse = 1;       // S_FALSE
inline constexpr tresult kNoInterface = static_cast<tresult>(0x80004002);  // E_NOINTERFACE
inline constexpr tresult kNotImplemented = static_cast<tresult>(0x80004001);

// Media types / bus directions / bus types / io modes / process modes.
inline constexpr int32_t kAudio = 0;
inline constexpr int32_t kEvent = 1;
inline constexpr int32_t kInput = 0;
inline constexpr int32_t kOutput = 1;
inline constexpr int32_t kMain = 0;
inline constexpr int32_t kAux = 1;
inline constexpr int32_t kIoAdvanced = 1;
inline constexpr int32_t kRealtime = 0;
inline constexpr int32_t kSample32 = 0;

// kSpeakerL | kSpeakerR
inline constexpr SpeakerArrangement kSpeakerStereo = 0x3;

// 16-byte interface/class id, COM GUID byte order on Windows. The constexpr
// builder mirrors the VST3 COM-compatible INLINE_UID byte placement so the
// well-known ids below are derived from their canonical 4x uint32 form
// instead of hand-transcribed bytes.
struct Tuid {
  char bytes[16];
};

constexpr Tuid makeComTuid(uint32_t l1, uint32_t l2, uint32_t l3, uint32_t l4) {
  return Tuid{{
      static_cast<char>(l1 & 0xFF), static_cast<char>((l1 >> 8) & 0xFF),
      static_cast<char>((l1 >> 16) & 0xFF), static_cast<char>((l1 >> 24) & 0xFF),
      static_cast<char>((l2 >> 16) & 0xFF), static_cast<char>((l2 >> 24) & 0xFF),
      static_cast<char>(l2 & 0xFF), static_cast<char>((l2 >> 8) & 0xFF),
      static_cast<char>((l3 >> 24) & 0xFF), static_cast<char>((l3 >> 16) & 0xFF),
      static_cast<char>((l3 >> 8) & 0xFF), static_cast<char>(l3 & 0xFF),
      static_cast<char>((l4 >> 24) & 0xFF), static_cast<char>((l4 >> 16) & 0xFF),
      static_cast<char>((l4 >> 8) & 0xFF), static_cast<char>(l4 & 0xFF),
  }};
}

// Well-known interface ids (public constants of the VST3 binary interface).
inline constexpr Tuid kFUnknownIid = makeComTuid(0x00000000, 0x00000000, 0xC0000000, 0x00000046);
inline constexpr Tuid kIComponentIid = makeComTuid(0xE831FF31, 0xF2D54301, 0x928EBBEE, 0x25697802);
inline constexpr Tuid kIAudioProcessorIid = makeComTuid(0x42043F99, 0xB7DA453C, 0xA569E79D, 0x9AAEC33D);
inline constexpr Tuid kIHostApplicationIid = makeComTuid(0x58E595CC, 0xDB2D4969, 0x8B6AAF8C, 0x36A664E5);
inline constexpr Tuid kIParameterChangesIid = makeComTuid(0xA4779663, 0x0BB64A56, 0xB44384A8, 0x466FEB9D);
inline constexpr Tuid kIEditControllerIid = makeComTuid(0xDCD7BBE3, 0x7742448D, 0xA874AACC, 0x979C759E);
inline constexpr Tuid kIComponentHandlerIid = makeComTuid(0x93A0BEA3, 0x0BD045DB, 0x8E890B0C, 0xC1E46AC6);
inline constexpr Tuid kIPlugViewIid = makeComTuid(0x5BC32507, 0xD06049EA, 0xA6151B52, 0x2B755B29);
inline constexpr Tuid kIPlugFrameIid = makeComTuid(0x367FAF01, 0xAFA94693, 0x8D4DA2A0, 0xED0882A3);
// A2: IBStream — the byte-stream contract IComponent::get/setState reads and
// writes through (the host implements it over a memory buffer; plugins only
// ever see the vtable).
inline constexpr Tuid kIBStreamIid = makeComTuid(0xC3BF6EA2, 0x30994752, 0x9B6BF990, 0x1EE33E9B);

inline bool tuidEqual(const char* a, const char* b) {
  return a != nullptr && b != nullptr && std::memcmp(a, b, 16) == 0;
}

// ---------------------------------------------------------------------------
// Factory-side structs (shared with the P2a probe).
// ---------------------------------------------------------------------------
struct PFactoryInfo {
  char vendor[64];
  char url[256];
  char email[128];
  int32_t flags;
};

struct PClassInfo {
  char cid[16];
  int32_t cardinality;
  char category[32];
  char name[64];
};

struct IPluginFactory;

struct IPluginFactoryVtbl {
  tresult(CVST_API* queryInterface)(void* self, const char* iid, void** obj);
  uint32_t(CVST_API* addRef)(void* self);
  uint32_t(CVST_API* release)(void* self);
  tresult(CVST_API* getFactoryInfo)(void* self, PFactoryInfo* info);
  int32_t(CVST_API* countClasses)(void* self);
  tresult(CVST_API* getClassInfo)(void* self, int32_t index, PClassInfo* info);
  // P2c: instantiate a class. cid/iid are 16-byte TUIDs passed as char*.
  tresult(CVST_API* createInstance)(void* self, const char* cid, const char* iid, void** obj);
};

struct IPluginFactory {
  IPluginFactoryVtbl* vtbl;
};

// ---------------------------------------------------------------------------
// Processing structs. Field order/types follow the published VST3 ABI; the
// static_asserts prove the compiler produced the exact expected layout.
// ---------------------------------------------------------------------------
struct BusInfo {
  int32_t mediaType = 0;
  int32_t direction = 0;
  int32_t channelCount = 0;
  char16_t name[128] = {};
  int32_t busType = 0;
  uint32_t flags = 0;
};
static_assert(sizeof(BusInfo) == 276, "VST3 BusInfo ABI layout");
static_assert(offsetof(BusInfo, name) == 12, "VST3 BusInfo ABI layout");
static_assert(offsetof(BusInfo, busType) == 268, "VST3 BusInfo ABI layout");

struct RoutingInfo {
  int32_t mediaType = 0;
  int32_t busIndex = 0;
  int32_t channel = 0;
};

struct ProcessSetup {
  int32_t processMode = kRealtime;
  int32_t symbolicSampleSize = kSample32;
  int32_t maxSamplesPerBlock = 0;
  double sampleRate = 48000.0;
};
static_assert(sizeof(ProcessSetup) == 24, "VST3 ProcessSetup ABI layout");
static_assert(offsetof(ProcessSetup, sampleRate) == 16, "VST3 ProcessSetup ABI layout");

struct AudioBusBuffers {
  int32_t numChannels = 0;
  uint64_t silenceFlags = 0;
  float** channelBuffers32 = nullptr;  // union with double** in the SDK; 32-bit floats only here
};
static_assert(sizeof(AudioBusBuffers) == 24, "VST3 AudioBusBuffers ABI layout");
static_assert(offsetof(AudioBusBuffers, silenceFlags) == 8, "VST3 AudioBusBuffers ABI layout");
static_assert(offsetof(AudioBusBuffers, channelBuffers32) == 16, "VST3 AudioBusBuffers ABI layout");

struct ProcessData {
  int32_t processMode = kRealtime;
  int32_t symbolicSampleSize = kSample32;
  int32_t numSamples = 0;
  int32_t numInputs = 0;
  int32_t numOutputs = 0;
  AudioBusBuffers* inputs = nullptr;
  AudioBusBuffers* outputs = nullptr;
  void* inputParameterChanges = nullptr;   // IParameterChanges*
  void* outputParameterChanges = nullptr;  // IParameterChanges*
  void* inputEvents = nullptr;             // IEventList* (optional, null)
  void* outputEvents = nullptr;            // IEventList* (optional, null)
  void* processContext = nullptr;          // ProcessContext* (optional, null)
};
static_assert(sizeof(ProcessData) == 80, "VST3 ProcessData ABI layout (x64)");
static_assert(offsetof(ProcessData, inputs) == 24, "VST3 ProcessData ABI layout (x64)");
static_assert(offsetof(ProcessData, processContext) == 72, "VST3 ProcessData ABI layout (x64)");

// ---------------------------------------------------------------------------
// Interface vtables (flat: FUnknown's 3 entries first, then base interfaces
// in declaration order — exactly how the compiler lays out single-inheritance
// COM vtables).
// ---------------------------------------------------------------------------
struct IComponent;

struct IComponentVtbl {
  // FUnknown
  tresult(CVST_API* queryInterface)(void* self, const char* iid, void** obj);
  uint32_t(CVST_API* addRef)(void* self);
  uint32_t(CVST_API* release)(void* self);
  // IPluginBase
  tresult(CVST_API* initialize)(void* self, void* hostContext /*FUnknown*/);
  tresult(CVST_API* terminate)(void* self);
  // IComponent
  tresult(CVST_API* getControllerClassId)(void* self, char* classId /*TUID out*/);
  tresult(CVST_API* setIoMode)(void* self, int32_t mode);
  int32_t(CVST_API* getBusCount)(void* self, int32_t mediaType, int32_t direction);
  tresult(CVST_API* getBusInfo)(void* self, int32_t mediaType, int32_t direction, int32_t index, BusInfo* bus);
  tresult(CVST_API* getRoutingInfo)(void* self, RoutingInfo* inInfo, RoutingInfo* outInfo);
  tresult(CVST_API* activateBus)(void* self, int32_t mediaType, int32_t direction, int32_t index, TBool state);
  tresult(CVST_API* setActive)(void* self, TBool state);
  tresult(CVST_API* setState)(void* self, void* stream /*IBStream*/);
  tresult(CVST_API* getState)(void* self, void* stream /*IBStream*/);
};

struct IComponent {
  IComponentVtbl* vtbl;
};

struct IAudioProcessor;

struct IAudioProcessorVtbl {
  // FUnknown
  tresult(CVST_API* queryInterface)(void* self, const char* iid, void** obj);
  uint32_t(CVST_API* addRef)(void* self);
  uint32_t(CVST_API* release)(void* self);
  // IAudioProcessor
  tresult(CVST_API* setBusArrangements)(void* self, SpeakerArrangement* inputs, int32_t numIns,
                                        SpeakerArrangement* outputs, int32_t numOuts);
  tresult(CVST_API* getBusArrangement)(void* self, int32_t direction, int32_t index, SpeakerArrangement* arr);
  tresult(CVST_API* canProcessSampleSize)(void* self, int32_t symbolicSampleSize);
  uint32_t(CVST_API* getLatencySamples)(void* self);
  tresult(CVST_API* setupProcessing)(void* self, ProcessSetup* setup);
  tresult(CVST_API* setProcessing)(void* self, TBool state);
  tresult(CVST_API* process)(void* self, ProcessData* data);
  uint32_t(CVST_API* getTailSamples)(void* self);
};

struct IAudioProcessor {
  IAudioProcessorVtbl* vtbl;
};

struct ParameterInfo {
  ParamID id = 0;
  char16_t title[128] = {};
  char16_t shortTitle[128] = {};
  char16_t units[128] = {};
  int32_t stepCount = 0;
  double defaultNormalizedValue = 0.0;
  int32_t unitId = 0;
  int32_t flags = 0;
};
static_assert(sizeof(ParameterInfo) == 792, "VST3 ParameterInfo ABI layout");
static_assert(offsetof(ParameterInfo, defaultNormalizedValue) == 776, "VST3 ParameterInfo ABI layout");

struct IEditController;
struct IEditControllerVtbl {
  tresult(CVST_API* queryInterface)(void* self, const char* iid, void** obj);
  uint32_t(CVST_API* addRef)(void* self);
  uint32_t(CVST_API* release)(void* self);
  tresult(CVST_API* initialize)(void* self, void* hostContext);
  tresult(CVST_API* terminate)(void* self);
  tresult(CVST_API* setComponentState)(void* self, void* stream);
  tresult(CVST_API* setState)(void* self, void* stream);
  tresult(CVST_API* getState)(void* self, void* stream);
  int32_t(CVST_API* getParameterCount)(void* self);
  tresult(CVST_API* getParameterInfo)(void* self, int32_t index, ParameterInfo* info);
  tresult(CVST_API* getParamStringByValue)(void* self, ParamID id, double value, char16_t* string);
  tresult(CVST_API* getParamValueByString)(void* self, ParamID id, const char16_t* string, double* value);
  double(CVST_API* normalizedParamToPlain)(void* self, ParamID id, double value);
  double(CVST_API* plainParamToNormalized)(void* self, ParamID id, double value);
  double(CVST_API* getParamNormalized)(void* self, ParamID id);
  tresult(CVST_API* setParamNormalized)(void* self, ParamID id, double value);
  tresult(CVST_API* setComponentHandler)(void* self, void* handler);
  void*(CVST_API* createView)(void* self, const char* name);
};
struct IEditController { IEditControllerVtbl* vtbl; };

struct ViewRect { int32_t left = 0; int32_t top = 0; int32_t right = 0; int32_t bottom = 0; };
static_assert(sizeof(ViewRect) == 16, "VST3 ViewRect ABI layout");

struct IPlugView;
struct IPlugViewVtbl {
  tresult(CVST_API* queryInterface)(void* self, const char* iid, void** obj);
  uint32_t(CVST_API* addRef)(void* self);
  uint32_t(CVST_API* release)(void* self);
  tresult(CVST_API* isPlatformTypeSupported)(void* self, const char* type);
  tresult(CVST_API* attached)(void* self, void* parent, const char* type);
  tresult(CVST_API* removed)(void* self);
  tresult(CVST_API* onWheel)(void* self, float distance);
  tresult(CVST_API* onKeyDown)(void* self, char16_t key, int16_t keyCode, int16_t modifiers);
  tresult(CVST_API* onKeyUp)(void* self, char16_t key, int16_t keyCode, int16_t modifiers);
  tresult(CVST_API* getSize)(void* self, ViewRect* size);
  tresult(CVST_API* onSize)(void* self, ViewRect* newSize);
  tresult(CVST_API* onFocus)(void* self, TBool state);
  tresult(CVST_API* setFrame)(void* self, void* frame);
  tresult(CVST_API* canResize)(void* self);
  tresult(CVST_API* checkSizeConstraint)(void* self, ViewRect* rect);
};
struct IPlugView { IPlugViewVtbl* vtbl; };

// A2: IBStream (state persistence). Vtable order is the published binary
// interface: FUnknown's 3 entries, then read/write/seek/tell. Seek modes are
// the public kIBSeekSet/Cur/End constants.
inline constexpr int32_t kIBSeekSet = 0;
inline constexpr int32_t kIBSeekCur = 1;
inline constexpr int32_t kIBSeekEnd = 2;

struct IBStreamVtbl {
  // FUnknown
  tresult(CVST_API* queryInterface)(void* self, const char* iid, void** obj);
  uint32_t(CVST_API* addRef)(void* self);
  uint32_t(CVST_API* release)(void* self);
  // IBStream
  tresult(CVST_API* read)(void* self, void* buffer, int32_t numBytes, int32_t* numBytesRead);
  tresult(CVST_API* write)(void* self, void* buffer, int32_t numBytes, int32_t* numBytesWritten);
  tresult(CVST_API* seek)(void* self, int64_t pos, int32_t mode, int64_t* result);
  tresult(CVST_API* tell)(void* self, int64_t* pos);
};
struct IBStream { IBStreamVtbl* vtbl; };

struct IComponentHandlerVtbl {
  tresult(CVST_API* queryInterface)(void* self, const char* iid, void** obj);
  uint32_t(CVST_API* addRef)(void* self);
  uint32_t(CVST_API* release)(void* self);
  tresult(CVST_API* beginEdit)(void* self, ParamID id);
  tresult(CVST_API* performEdit)(void* self, ParamID id, double normalized);
  tresult(CVST_API* endEdit)(void* self, ParamID id);
  tresult(CVST_API* restartComponent)(void* self, int32_t flags);
};

struct IPlugFrameVtbl {
  tresult(CVST_API* queryInterface)(void* self, const char* iid, void** obj);
  uint32_t(CVST_API* addRef)(void* self);
  uint32_t(CVST_API* release)(void* self);
  tresult(CVST_API* resizeView)(void* self, IPlugView* view, ViewRect* newSize);
};

// Host-side interfaces the plugin may call back into.
struct IHostApplicationVtbl {
  // FUnknown
  tresult(CVST_API* queryInterface)(void* self, const char* iid, void** obj);
  uint32_t(CVST_API* addRef)(void* self);
  uint32_t(CVST_API* release)(void* self);
  // IHostApplication
  tresult(CVST_API* getName)(void* self, char16_t* name /*String128*/);
  tresult(CVST_API* createInstance)(void* self, char* cid, char* iid, void** obj);
};

struct IParameterChangesVtbl {
  // FUnknown
  tresult(CVST_API* queryInterface)(void* self, const char* iid, void** obj);
  uint32_t(CVST_API* addRef)(void* self);
  uint32_t(CVST_API* release)(void* self);
  // IParameterChanges
  int32_t(CVST_API* getParameterCount)(void* self);
  void*(CVST_API* getParameterData)(void* self, int32_t index);  // IParamValueQueue*
  void*(CVST_API* addParameterData)(void* self, const ParamID* id, int32_t* index);
};

struct IParamValueQueueVtbl {
  tresult(CVST_API* queryInterface)(void* self, const char* iid, void** obj);
  uint32_t(CVST_API* addRef)(void* self);
  uint32_t(CVST_API* release)(void* self);
  ParamID(CVST_API* getParameterId)(void* self);
  int32_t(CVST_API* getPointCount)(void* self);
  tresult(CVST_API* getPoint)(void* self, int32_t index, int32_t* sampleOffset, double* value);
  tresult(CVST_API* addPoint)(void* self, int32_t sampleOffset, double value, int32_t* index);
};

// Module entry points (same as the P2a probe).
using InitDllFn = bool(CVST_API*)();
using GetPluginFactoryFn = IPluginFactory*(CVST_API*)();

}  // namespace corevideo::vst3
