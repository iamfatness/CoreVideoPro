#include "modules/EncoderCapacityProbe.h"

#include "core/BoundedAsyncLog.h"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <sstream>
#include <thread>
#include <utility>
#include <vector>

#if defined(_WIN32) && COREVIDEO_WITH_MF_ENCODER && !COREVIDEO_STUB && COREVIDEO_ENABLE_DEV_ADAPTERS
#define COREVIDEO_HAS_MF_ENCODER_PROBE 1
#else
#define COREVIDEO_HAS_MF_ENCODER_PROBE 0
#endif

#if COREVIDEO_HAS_MF_ENCODER_PROBE
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <dxgi.h>
#include <mfapi.h>
#include <mferror.h>
#include <mfidl.h>
#include <mftransform.h>
#include <objbase.h>
#include <wrl/client.h>
#endif

namespace corevideo::modules {

namespace {

// How many concurrent hardware sessions the probe will try to create before it
// stops asking. Deliberately modest: the number exists to bound an ISO
// selection, and creating dozens of encoder sessions on a stranger's machine to
// find an exact ceiling is not worth the churn. Overridable so a tester with an
// unusual GPU can be asked to raise it in a support session.
constexpr int kDefaultProbeSessionCap = 8;

// The reference workload the software budget is expressed against.
constexpr double kReferencePixelRate = 1920.0 * 1080.0 * 30.0;

int envInt(const char* name, int fallback) {
  const char* raw = std::getenv(name);
  if (raw == nullptr || *raw == '\0') return fallback;
  char* end = nullptr;
  const long parsed = std::strtol(raw, &end, 10);
  if (end == raw) return fallback;
  return static_cast<int>(parsed);
}

bool probeDisabledByEnv() { return envInt("COREVIDEO_ENCODER_PROBE", 1) == 0; }

int probeSessionCap() {
  const int cap = envInt("COREVIDEO_ENCODER_PROBE_MAX_SESSIONS", kDefaultProbeSessionCap);
  return (std::max)(1, (std::min)(cap, 32));
}

}  // namespace

const char* encoderProbeStatusId(EncoderProbeStatus status) {
  switch (status) {
    case EncoderProbeStatus::Pending:
      return "pending";
    case EncoderProbeStatus::Ready:
      return "ready";
    case EncoderProbeStatus::Failed:
      return "failed";
    case EncoderProbeStatus::Disabled:
      return "disabled";
  }
  return "pending";
}

std::string EncoderProbeKey::describe() const {
  std::ostringstream out;
  out << codec << " " << width << "x" << height << "@" << fps;
  return out.str();
}

std::string ProbedEncoderCapacity::describe() const {
  std::ostringstream out;
  out << "probe=" << encoderProbeStatusId(status) << " workload=" << key.describe();
  if (!adapterDescription.empty()) {
    out << " adapter=\"" << adapterDescription << "\"";
  }
  out << " hw=" << (hardwareAvailable ? "yes" : "no");
  if (hardwareAvailable) {
    out << " sessions<=" << hardwareSessionCeiling << (ceilingIsCreationProofOnly ? " (creation-proof only)" : "");
    if (hardwareSessionCeiling >= probeSessionCap && probeSessionCap > 0) {
      out << " (probe cap reached; true ceiling may be higher)";
    }
    if (!hardwareEncoderName.empty()) {
      out << " mft=\"" << hardwareEncoderName << "\"";
    }
  }
  out << " sw=" << (softwareAvailable ? "yes" : "no") << " cpus=" << logicalProcessors
      << " tookMs=" << probeDurationMs;
  if (!detail.empty()) {
    out << " detail=\"" << detail << "\"";
  }
  return out.str();
}

// ---------------------------------------------------------------------------
// Pure helpers
// ---------------------------------------------------------------------------

int softwareSessionBudget(unsigned logicalProcessors, int width, int height, int fps) {
  if (logicalProcessors == 0) {
    return -1;  // Unknown: never let an unknown cause a refusal.
  }
  const double pixelRate = static_cast<double>((std::max)(1, width)) *
                           static_cast<double>((std::max)(1, height)) *
                           static_cast<double>((std::max)(1, fps));
  // A 1080p30 H.264 software encode costs roughly a couple of cores once the
  // colour convert and the mux are counted, so a quarter of the logical
  // processors is the per-1080p30-stream allowance. Scaled inversely by pixel
  // rate: the same box that takes four 1080p30 stems takes none at 4K60.
  const double perStream = static_cast<double>(logicalProcessors) / 4.0;
  const double scaled = perStream * (kReferencePixelRate / pixelRate);
  const int budget = static_cast<int>(scaled);
  return (std::max)(0, (std::min)(budget, 8));
}

IsoEncoderCapacity assumedIsoEncoderCapacity(int reservedForProgram) {
  // EXACTLY what the product assumed before this probe existed. Keeping the old
  // numbers as the fallback is deliberate: a machine we cannot interrogate gets
  // today's behaviour, not a new and untested restriction.
  IsoEncoderCapacity capacity;
  capacity.hardwareSessionLimit = 8;
  capacity.reservedHardwareSessions = (std::max)(0, reservedForProgram);
  capacity.hardwareAvailable = true;
  capacity.softwareAvailable = true;
  capacity.softwareSessionLimit = -1;  // unknown => admission may warn, never refuse
  return capacity;
}

IsoEncoderCapacity toIsoEncoderCapacity(const ProbedEncoderCapacity& probe, int reservedForProgram) {
  if (!probe.probed) {
    return assumedIsoEncoderCapacity(reservedForProgram);
  }
  IsoEncoderCapacity capacity;
  capacity.hardwareSessionLimit = (std::max)(0, probe.hardwareSessionCeiling);
  capacity.reservedHardwareSessions = (std::max)(0, reservedForProgram);
  capacity.hardwareAvailable = probe.hardwareAvailable && probe.hardwareSessionCeiling > 0;
  capacity.softwareAvailable = probe.softwareAvailable;
  capacity.softwareSessionLimit =
      probe.softwareAvailable
          ? softwareSessionBudget(probe.logicalProcessors, probe.key.width, probe.key.height, probe.key.fps)
          : 0;
  return capacity;
}

// ---------------------------------------------------------------------------
// The platform probe
// ---------------------------------------------------------------------------

#if COREVIDEO_HAS_MF_ENCODER_PROBE

namespace {

using Microsoft::WRL::ComPtr;

std::string narrow(const wchar_t* wide) {
  if (wide == nullptr) return {};
  const int needed = ::WideCharToMultiByte(CP_UTF8, 0, wide, -1, nullptr, 0, nullptr, nullptr);
  if (needed <= 1) return {};
  std::string out(static_cast<size_t>(needed - 1), '\0');
  ::WideCharToMultiByte(CP_UTF8, 0, wide, -1, out.data(), needed, nullptr, nullptr);
  return out;
}

std::string hr(HRESULT result) {
  std::ostringstream out;
  out << "0x" << std::hex << std::uppercase << static_cast<unsigned long>(result);
  return out.str();
}

GUID subtypeForCodec(const std::string& codec) {
  if (codec == "hevc" || codec == "h265" || codec == "hvc1") return MFVideoFormat_HEVC;
  return MFVideoFormat_H264;
}

// Identify the adapter Media Foundation's hardware encoder will actually sit
// on: DXGI adapter 0, skipping the Basic Render Driver. A support bundle that
// names the GPU is half of diagnosing a machine nobody watched.
void fillAdapterIdentity(ProbedEncoderCapacity& out) {
  ComPtr<IDXGIFactory1> factory;
  HRESULT result = CreateDXGIFactory1(IID_PPV_ARGS(&factory));
  if (FAILED(result) || !factory) {
    out.detail += "dxgi-factory-failed(" + hr(result) + ") ";
    return;
  }
  for (UINT index = 0;; ++index) {
    ComPtr<IDXGIAdapter1> adapter;
    if (factory->EnumAdapters1(index, &adapter) == DXGI_ERROR_NOT_FOUND) break;
    DXGI_ADAPTER_DESC1 desc{};
    if (FAILED(adapter->GetDesc1(&desc))) continue;
    if ((desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) != 0) continue;  // WARP is not an encoder
    out.adapterDescription = narrow(desc.Description);
    out.vendorId = desc.VendorId;
    out.deviceId = desc.DeviceId;
    out.adapterLuid = (static_cast<std::uint64_t>(static_cast<std::uint32_t>(desc.AdapterLuid.HighPart)) << 32) |
                      static_cast<std::uint32_t>(desc.AdapterLuid.LowPart);
    return;
  }
  out.detail += "no-hardware-dxgi-adapter ";
}

// Configure one encoder MFT at the recording's ACTUAL frame size and rate. This
// is the resolution/fps term: a GPU whose encoder will not take 4K60 fails here
// and is reported honestly as zero capacity for 4K60, while still reporting a
// healthy ceiling for 1080p30.
bool configureEncoder(IMFTransform* transform, const EncoderProbeKey& key, std::string& why) {
  // A hardware encoder MFT is an ASYNC MFT and refuses SetInputType with
  // MF_E_TRANSFORM_ASYNC_LOCKED (0xC00D6D77) until the caller declares it
  // understands the async model. Without this the probe reported "no hardware
  // encoder" on an RTX 4090 — a false negative that would have been far worse
  // than the hard-coded literal it replaced.
  ComPtr<IMFAttributes> attributes;
  if (SUCCEEDED(transform->GetAttributes(&attributes)) && attributes) {
    UINT32 isAsync = 0;
    if (SUCCEEDED(attributes->GetUINT32(MF_TRANSFORM_ASYNC, &isAsync)) && isAsync != 0) {
      attributes->SetUINT32(MF_TRANSFORM_ASYNC_UNLOCK, TRUE);
    }
  }

  ComPtr<IMFMediaType> outputType;
  HRESULT result = MFCreateMediaType(&outputType);
  if (FAILED(result)) {
    why = "create-output-type " + hr(result);
    return false;
  }
  const UINT32 bitrate = static_cast<UINT32>(
      (std::min)(120.0, (std::max)(2.0, static_cast<double>(key.width) * key.height * key.fps / 100000.0)) *
      1000000.0);
  outputType->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
  outputType->SetGUID(MF_MT_SUBTYPE, subtypeForCodec(key.codec));
  outputType->SetUINT32(MF_MT_AVG_BITRATE, bitrate);
  outputType->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
  MFSetAttributeSize(outputType.Get(), MF_MT_FRAME_SIZE, static_cast<UINT32>(key.width),
                     static_cast<UINT32>(key.height));
  MFSetAttributeRatio(outputType.Get(), MF_MT_FRAME_RATE, static_cast<UINT32>(key.fps), 1);
  MFSetAttributeRatio(outputType.Get(), MF_MT_PIXEL_ASPECT_RATIO, 1, 1);

  result = transform->SetOutputType(0, outputType.Get(), 0);
  if (FAILED(result)) {
    why = "set-output-type " + hr(result);
    return false;
  }

  ComPtr<IMFMediaType> inputType;
  result = MFCreateMediaType(&inputType);
  if (FAILED(result)) {
    why = "create-input-type " + hr(result);
    return false;
  }
  inputType->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
  inputType->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_NV12);
  inputType->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
  MFSetAttributeSize(inputType.Get(), MF_MT_FRAME_SIZE, static_cast<UINT32>(key.width),
                     static_cast<UINT32>(key.height));
  MFSetAttributeRatio(inputType.Get(), MF_MT_FRAME_RATE, static_cast<UINT32>(key.fps), 1);
  MFSetAttributeRatio(inputType.Get(), MF_MT_PIXEL_ASPECT_RATIO, 1, 1);

  result = transform->SetInputType(0, inputType.Get(), 0);
  if (FAILED(result)) {
    why = "set-input-type " + hr(result);
    return false;
  }
  return true;
}

}  // namespace

ProbedEncoderCapacity probeEncoderCapacity(const EncoderProbeKey& key) {
  // ONE PROBE AT A TIME, PROCESS-WIDE. Two probes for different workloads (the
  // sink's default-profile prewarm and the configureRecording prewarm) ran
  // concurrently on the dev rig and cannibalised each other's encoder sessions:
  // the second reported "no hardware encoder on an RTX 4090", which is the
  // worst possible answer because it is confidently wrong. Serialising costs a
  // second on a background thread and removes the whole class.
  static std::mutex serializer;
  std::lock_guard<std::mutex> probeLock(serializer);

  ProbedEncoderCapacity out;
  out.key = key;
  out.logicalProcessors = std::thread::hardware_concurrency();
  out.probeSessionCap = probeSessionCap();
  const auto startedAt = std::chrono::steady_clock::now();

  if (probeDisabledByEnv()) {
    out.status = EncoderProbeStatus::Disabled;
    out.detail = "COREVIDEO_ENCODER_PROBE=0";
    return out;
  }

  const HRESULT comResult = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
  const bool comInitialized = SUCCEEDED(comResult);
  const bool mfStarted = SUCCEEDED(MFStartup(MF_VERSION));
  if (!mfStarted) {
    out.status = EncoderProbeStatus::Failed;
    out.detail = "MFStartup failed";
    if (comInitialized) CoUninitialize();
    return out;
  }

  fillAdapterIdentity(out);

  MFT_REGISTER_TYPE_INFO outputInfo{MFMediaType_Video, subtypeForCodec(key.codec)};

  // --- software MFT presence -------------------------------------------------
  {
    IMFActivate** activates = nullptr;
    UINT32 count = 0;
    const HRESULT result = MFTEnumEx(MFT_CATEGORY_VIDEO_ENCODER,
                                     MFT_ENUM_FLAG_SYNCMFT | MFT_ENUM_FLAG_ASYNCMFT |
                                         MFT_ENUM_FLAG_LOCALMFT | MFT_ENUM_FLAG_TRANSCODE_ONLY |
                                         MFT_ENUM_FLAG_SORTANDFILTER,
                                     nullptr, &outputInfo, &activates, &count);
    if (SUCCEEDED(result)) {
      out.softwareAvailable = count > 0;
      for (UINT32 i = 0; i < count; ++i) activates[i]->Release();
      CoTaskMemFree(activates);
    } else {
      out.detail += "software-enum-failed(" + hr(result) + ") ";
    }
  }

  // --- hardware MFT: does one exist, and how many sessions can we CREATE? -----
  CLSID encoderClsid{};
  bool haveClsid = false;
  {
    IMFActivate** activates = nullptr;
    UINT32 count = 0;
    const HRESULT result =
        MFTEnumEx(MFT_CATEGORY_VIDEO_ENCODER, MFT_ENUM_FLAG_HARDWARE | MFT_ENUM_FLAG_SORTANDFILTER,
                  nullptr, &outputInfo, &activates, &count);
    if (SUCCEEDED(result) && count > 0) {
      WCHAR* name = nullptr;
      UINT32 nameLength = 0;
      if (SUCCEEDED(activates[0]->GetAllocatedString(MFT_FRIENDLY_NAME_Attribute, &name, &nameLength))) {
        out.hardwareEncoderName = narrow(name);
        CoTaskMemFree(name);
      }
      if (SUCCEEDED(activates[0]->GetGUID(MFT_TRANSFORM_CLSID_Attribute, &encoderClsid))) {
        haveClsid = true;
      }
    } else if (FAILED(result)) {
      out.detail += "hardware-enum-failed(" + hr(result) + ") ";
    } else {
      out.detail += "no-hardware-encoder-mft ";
    }
    for (UINT32 i = 0; i < count; ++i) activates[i]->Release();
    CoTaskMemFree(activates);
  }

  if (haveClsid) {
    // CoCreateInstance rather than IMFActivate::ActivateObject: an activate
    // caches ONE object, and we need independent instances to find out how many
    // sessions coexist. Every instance is taken all the way to
    // NOTIFY_BEGIN_STREAMING, because that — not construction — is where the
    // driver's session limit is enforced.
    std::vector<ComPtr<IMFTransform>> sessions;
    std::string why;
    // Did the encoder REFUSE this codec/size/rate (a real capability answer), or
    // did we simply fail to get an object (an environmental answer)? The
    // difference decides whether a zero count is trustworthy — see below.
    bool refusedTheWorkload = false;
    for (int index = 0; index < out.probeSessionCap; ++index) {
      ComPtr<IMFTransform> transform;
      HRESULT result = CoCreateInstance(encoderClsid, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&transform));
      if (FAILED(result) || !transform) {
        why = "cocreate " + hr(result);
        break;
      }
      if (!configureEncoder(transform.Get(), key, why)) {
        refusedTheWorkload = true;
        break;
      }
      result = transform->ProcessMessage(MFT_MESSAGE_NOTIFY_BEGIN_STREAMING, 0);
      if (FAILED(result)) {
        why = "begin-streaming " + hr(result);
        break;
      }
      sessions.push_back(transform);
    }
    out.hardwareSessionCeiling = static_cast<int>(sessions.size());
    out.hardwareAvailable = out.hardwareSessionCeiling > 0;
    if (!why.empty()) {
      out.detail += "stopped-at-" + std::to_string(sessions.size()) + "(" + why + ") ";
    }
    for (auto& transform : sessions) {
      transform->ProcessMessage(MFT_MESSAGE_NOTIFY_END_STREAMING, 0);
    }
    sessions.clear();

    // A hardware encoder MFT is registered but we could not even CREATE one
    // session, and it never got as far as refusing the format. That is far more
    // likely to be contention (another application, or an encoder we already
    // have open) than a machine that genuinely cannot encode. Reporting it as
    // "no hardware" would refuse ISO on a capable box, so report it as an
    // INCONCLUSIVE probe instead: the caller falls back to the assumed capacity
    // and, by the never-refuse-on-an-assumption rule, cannot block the show.
    if (out.hardwareSessionCeiling == 0 && !refusedTheWorkload) {
      out.detail += "inconclusive-no-session-created ";
      out.status = EncoderProbeStatus::Failed;
      out.probeDurationMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                                std::chrono::steady_clock::now() - startedAt)
                                .count();
      MFShutdown();
      if (comInitialized) CoUninitialize();
      return out;
    }
  }

  out.status = EncoderProbeStatus::Ready;
  out.probed = true;
  out.ceilingIsCreationProofOnly = true;
  out.probeDurationMs =
      std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - startedAt)
          .count();

  MFShutdown();
  if (comInitialized) CoUninitialize();
  return out;
}

#else

ProbedEncoderCapacity probeEncoderCapacity(const EncoderProbeKey& key) {
  ProbedEncoderCapacity out;
  out.key = key;
  out.logicalProcessors = std::thread::hardware_concurrency();
  out.status = EncoderProbeStatus::Disabled;
  out.detail = "no encoder probe in this build/platform";
  return out;
}

#endif

// ---------------------------------------------------------------------------
// The cache
// ---------------------------------------------------------------------------

namespace {
// A probe re-runs at most this often per workload. It transiently occupies
// hardware encoder sessions, so it must not fire on every arm of a repeating
// sync command.
constexpr std::chrono::seconds kMinReprobeInterval{60};

std::map<EncoderProbeKey, std::chrono::steady_clock::time_point>& lastProbeAt() {
  // Intentionally leaked, for the same reason the cache below is: a detached
  // probe thread may still be running at process exit and must not touch a
  // destroyed object.
  static auto* map = new std::map<EncoderProbeKey, std::chrono::steady_clock::time_point>();
  return *map;
}
}  // namespace

EncoderCapacityCache& EncoderCapacityCache::instance() {
  // DELIBERATELY LEAKED. Probes run on detached threads that publish back into
  // this cache; a function-local static would be destroyed during process
  // teardown while such a thread could still be mid-publish. Leaking a
  // process-lifetime singleton is the same trade `startPluginHostScan` makes by
  // capturing a MediaCore that outlives every caller.
  static auto* cache = new EncoderCapacityCache();
  return *cache;
}

void EncoderCapacityCache::setProbeFunctionForTesting(ProbeFn fn) {
  std::lock_guard<std::mutex> lock(mutex_);
  probeFn_ = std::move(fn);
  cache_.clear();
  inFlight_.clear();
  lastProbeAt().clear();
  haveObservedAdapter_ = false;
  observedAdapterLuid_ = 0;
}

void EncoderCapacityCache::invalidate() {
  std::lock_guard<std::mutex> lock(mutex_);
  cache_.clear();
  lastProbeAt().clear();
}

void EncoderCapacityCache::setRecordingActive(bool active) {
  std::lock_guard<std::mutex> lock(mutex_);
  recordingActive_ = active;
}

void EncoderCapacityCache::setForcedCapacityForTesting(const ProbedEncoderCapacity* capacity) {
  std::lock_guard<std::mutex> lock(mutex_);
  forcedCapacityActive_ = capacity != nullptr;
  forcedCapacity_ = capacity != nullptr ? *capacity : ProbedEncoderCapacity{};
  cache_.clear();
  lastProbeAt().clear();
}

ProbedEncoderCapacity EncoderCapacityCache::lookup(const EncoderProbeKey& key) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (forcedCapacityActive_) {
    ProbedEncoderCapacity forced = forcedCapacity_;
    forced.key = key;
    return forced;
  }
  const auto found = cache_.find(key);
  if (found != cache_.end()) {
    return found->second;
  }
  // Cache miss on the arm path: DO NOT probe here (this call site runs under
  // coreMutex). Kick the background probe so the next arm is warm and report
  // Pending, which the caller turns into the assumed capacity.
  startProbeLocked(key);
  ProbedEncoderCapacity pending;
  pending.key = key;
  pending.status = EncoderProbeStatus::Pending;
  pending.detail = "capacity probe has not completed for this workload yet";
  return pending;
}

void EncoderCapacityCache::prewarm(const EncoderProbeKey& key) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (forcedCapacityActive_) {
    return;
  }
  const auto cached = cache_.find(key);
  if (cached != cache_.end()) {
    const auto at = lastProbeAt().find(key);
    if (at != lastProbeAt().end() && std::chrono::steady_clock::now() - at->second < kMinReprobeInterval) {
      return;
    }
  }
  startProbeLocked(key);
}

void EncoderCapacityCache::startProbeLocked(const EncoderProbeKey& key) {
  if (recordingActive_) {
    return;  // Never contend with a live show for encoder sessions.
  }
  if (inFlight_[key]) {
    return;
  }
  inFlight_[key] = true;
  lastProbeAt()[key] = std::chrono::steady_clock::now();
  ProbeFn fn = probeFn_;

  // Detached, exactly like startPluginHostScan: COM activation, driver calls and
  // GPU session churn have no business inside anybody's lock budget. The cache
  // is a function-local static that outlives every caller.
  std::thread([this, key, fn]() {
    ProbedEncoderCapacity result = fn ? fn(key) : probeEncoderCapacity(key);
    result.key = key;
    result.probed = result.status == EncoderProbeStatus::Ready;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      // ADAPTER CHANGE INVALIDATION: an eGPU unplugged, a driver reinstall or a
      // switchable-graphics flip gives a different LUID. Every entry keyed to
      // the old adapter is now a lie, so drop the lot.
      if (result.adapterLuid != 0) {
        if (haveObservedAdapter_ && observedAdapterLuid_ != result.adapterLuid) {
          cache_.clear();
        }
        observedAdapterLuid_ = result.adapterLuid;
        haveObservedAdapter_ = true;
      }
      cache_[key] = result;
      inFlight_[key] = false;
    }
    ::corevideo::core::nativeLogf("[encoder-probe] %s\n", result.describe().c_str());
  }).detach();
}

}  // namespace corevideo::modules
