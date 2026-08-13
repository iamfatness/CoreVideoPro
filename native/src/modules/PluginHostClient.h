#pragma once

// Core-side plugin-host audio transport client (VST spec P2b).
//
// Owns the kernel objects (SHM block + req/done events), spawns the resident
// `corevideo-plugin-host --serve <instance>` process, and performs the
// synchronous deadline-bounded block exchange the audio worker calls:
//
//   exchange(): copy pcm + plugin selection in → ++seqIn → signal req →
//               wait done (deadline) → verify seqOut == seqIn → copy pcm back
//               → harvest the host's status (activePlugin/lastError) when its
//               generation changed.
//
// A missed deadline returns false (BYPASS — the caller keeps its unprocessed
// audio and the show never stalls); stale completions from an abandoned tick
// are discarded by the sequence check + a done-event reset at the top of the
// next exchange. The audio worker is the single exchange() caller by contract
// (audioOutputMutex_ work span); the only lock is statusMutex_, a tiny leaf
// taken on status CHANGE (rare) and by snapshot readers.
//
// Windows-only transport; other platforms report never-ready so callers fall
// through to the built-in DSP (the guaranteed fallback everywhere).

#include <atomic>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <vector>
#include <string>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

#include "host-transport.h"

namespace corevideo::modules {

// A2: one published parameter, copied out of the SHM block on generation
// change (see PluginHostClient::params()).
struct VstPublishedParam {
  uint32_t id = 0;
  int32_t stepCount = 0;
  int32_t flags = 0;
  double normalized = 0.0;
  std::string title;
  std::string units;
  std::string display;
};

struct VstPublishedParams {
  uint32_t listGeneration = 0;
  uint32_t valuesGeneration = 0;
  int32_t totalCount = 0;  // real controller count; entries is capped
  std::string pluginClass;
  std::vector<VstPublishedParam> entries;
};

class PluginHostClient {
 public:
  ~PluginHostClient() { stop(); }

#ifdef _WIN32
  bool start(const std::string& exePath, const std::string& instance) {
    using namespace corevideo::pluginhost;
    if (exePath.empty()) {
      return false;
    }
    // The resident host intentionally exits after an idle window. A client can
    // therefore still be marked ready while its process handle is signaled,
    // especially when the operator opens an editor before audio has flowed.
    // Reap that stale transport here so control-plane editor requests can
    // relaunch the host without waiting for exchange() to notice the exit.
    if (ready_) {
      if (hostAlive()) {
        return true;
      }
      stop();
    }

    instance_ = instance;
    shm_ = ::CreateFileMappingA(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0,
                                sizeof(HostAudioBlock), hostShmName(instance).c_str());
    req_ = ::CreateEventA(nullptr, FALSE, FALSE, hostReqEventName(instance).c_str());
    done_ = ::CreateEventA(nullptr, FALSE, FALSE, hostDoneEventName(instance).c_str());
    editor_ = ::CreateEventA(nullptr, FALSE, FALSE, hostEditorEventName(instance).c_str());
    param_ = ::CreateEventA(nullptr, FALSE, FALSE, hostParamEventName(instance).c_str());
    stateReq_ = ::CreateEventA(nullptr, FALSE, FALSE, hostStateReqEventName(instance).c_str());
    stateDone_ = ::CreateEventA(nullptr, FALSE, FALSE, hostStateDoneEventName(instance).c_str());
    if (shm_ == nullptr || req_ == nullptr || done_ == nullptr || editor_ == nullptr ||
        param_ == nullptr || stateReq_ == nullptr || stateDone_ == nullptr) {
      std::fprintf(stderr, "[plugin-host-client] kernel object creation failed (err=%lu)\n", ::GetLastError());
      stop();
      return false;
    }
    block_ = static_cast<HostAudioBlock*>(::MapViewOfFile(shm_, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(HostAudioBlock)));
    if (block_ == nullptr) {
      std::fprintf(stderr, "[plugin-host-client] MapViewOfFile failed (err=%lu)\n", ::GetLastError());
      stop();
      return false;
    }
    new (block_) HostAudioBlock();
    harvestedStatusGeneration_ = 0;
    {
      std::lock_guard<std::mutex> lock(statusMutex_);
      activePlugin_.clear();
      lastError_.clear();
      statusCode_ = corevideo::pluginhost::kHostStatusTestProcessor;
    }
    {
      std::lock_guard<std::mutex> lock(paramsMutex_);
      harvestedParams_ = {};
    }

    // Spawn the resident host — exact exe path, no shell (see PluginHostScan.h
    // security note), no inherited stdio needed.
    std::string commandLine = "\"" + exePath + "\" \"--serve\" \"" + instance + "\"";
    std::vector<char> mutableCommandLine(commandLine.begin(), commandLine.end());
    mutableCommandLine.push_back('\0');
    STARTUPINFOA startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION process{};
    if (!::CreateProcessA(exePath.c_str(), mutableCommandLine.data(), nullptr, nullptr, FALSE,
                          CREATE_NO_WINDOW, nullptr, nullptr, &startup, &process)) {
      std::fprintf(stderr, "[plugin-host-client] CreateProcess('%s') failed (err=%lu)\n",
                   exePath.c_str(), ::GetLastError());
      stop();
      return false;
    }
    process_ = process.hProcess;
    ::CloseHandle(process.hThread);
    startCount_.fetch_add(1, std::memory_order_relaxed);
    ready_ = true;
    return true;
  }

  [[nodiscard]] bool ready() const { return ready_; }
  // Bumped on every successful host launch — the respawn state re-injection
  // (A2) keys off it to know a fresh host needs the saved states pushed again.
  [[nodiscard]] int64_t startCount() const { return startCount_.load(std::memory_order_relaxed); }

  bool requestEditor(const std::string& pluginBundle, const std::string& pluginClass) {
    using namespace corevideo::pluginhost;
    if (!ready_ || block_ == nullptr || editor_ == nullptr || pluginBundle.empty()) return false;
    copyBlockString(block_->editorPluginBundle, pluginBundle.c_str());
    copyBlockString(block_->editorPluginClass, pluginClass.c_str());
    block_->editorRequestGeneration = block_->editorRequestGeneration + 1;
    return ::SetEvent(editor_) != FALSE;
  }

  // A2: queue a normalized param value for the host to apply (latest-wins
  // ring + a dedicated event — NEVER the audio req/done pair). Control-plane
  // callers only; paramSetMutex_ keeps concurrent writers ordered.
  bool setParam(const std::string& pluginBundle, const std::string& pluginClass,
                uint32_t paramId, double normalized) {
    using namespace corevideo::pluginhost;
    if (!ready_ || block_ == nullptr || param_ == nullptr || pluginBundle.empty()) return false;
    std::lock_guard<std::mutex> lock(paramSetMutex_);
    copyBlockString(block_->paramSetPluginBundle, pluginBundle.c_str());
    copyBlockString(block_->paramSetPluginClass, pluginClass.c_str());
    const uint32_t seq = block_->paramSetSeq;
    HostParamSetEntry& entry = block_->paramSet[seq % kHostParamSetRing];
    entry.id = paramId;
    entry.normalized = normalized;
    block_->paramSetSeq = seq + 1;
    return ::SetEvent(param_) != FALSE;
  }

  // A2: the host-published param surface for the active selection. Copies out
  // of the block ONLY when a generation moved (steady state = two volatile
  // reads); a mid-copy value bump is retried once and otherwise tolerated
  // (values are refreshed continuously anyway).
  [[nodiscard]] VstPublishedParams params() const {
    using namespace corevideo::pluginhost;
    std::lock_guard<std::mutex> lock(paramsMutex_);
    if (block_ == nullptr || !ready_) {
      return harvestedParams_;
    }
    for (int attempt = 0; attempt < 2; ++attempt) {
      const uint32_t listGeneration = block_->paramListGeneration;
      const uint32_t valuesGeneration = block_->paramValuesGeneration;
      if (listGeneration == harvestedParams_.listGeneration &&
          valuesGeneration == harvestedParams_.valuesGeneration) {
        return harvestedParams_;
      }
      VstPublishedParams fresh;
      fresh.listGeneration = listGeneration;
      fresh.valuesGeneration = valuesGeneration;
      fresh.totalCount = block_->paramTotalCount;
      fresh.pluginClass.assign(block_->paramPluginClass,
                               strnlen(block_->paramPluginClass, sizeof(block_->paramPluginClass)));
      const int32_t published = block_->paramPublishedCount;
      const int32_t count = published < 0 ? 0
                            : (published > kHostParamPublishMax ? kHostParamPublishMax : published);
      fresh.entries.reserve(static_cast<size_t>(count));
      for (int32_t index = 0; index < count; ++index) {
        const HostParamEntry& raw = block_->params[index];
        VstPublishedParam entry;
        entry.id = raw.id;
        entry.stepCount = raw.stepCount;
        entry.flags = raw.flags;
        entry.normalized = raw.normalized;
        entry.title.assign(raw.title, strnlen(raw.title, sizeof(raw.title)));
        entry.units.assign(raw.units, strnlen(raw.units, sizeof(raw.units)));
        entry.display.assign(raw.display, strnlen(raw.display, sizeof(raw.display)));
        fresh.entries.push_back(std::move(entry));
      }
      harvestedParams_ = std::move(fresh);
      if (block_->paramListGeneration == listGeneration &&
          block_->paramValuesGeneration == valuesGeneration) {
        break;  // clean copy
      }
    }
    return harvestedParams_;
  }

  // A3: the active selection's reported plugin latency (0 for the test
  // processor / nothing loaded). Volatile read — safe from any thread.
  [[nodiscard]] uint32_t latencySamples() const {
    return ready_ && block_ != nullptr ? block_->latencySamples : 0;
  }

  // A2: component-state round trips (CONTROL PLANE — never the audio worker;
  // serialized by stateMutex_; the timeout covers an on-demand plugin load in
  // the host). Failures come back as a loud error string, never silence.
  bool getState(const std::string& pluginBundle, const std::string& pluginClass,
                std::vector<uint8_t>* out, std::string* outError, int timeoutMs = 8000) {
    return stateTransact(corevideo::pluginhost::kHostStateRequestGet, pluginBundle, pluginClass,
                         nullptr, out, outError, timeoutMs);
  }

  bool setState(const std::string& pluginBundle, const std::string& pluginClass,
                const std::vector<uint8_t>& data, std::string* outError, int timeoutMs = 8000) {
    return stateTransact(corevideo::pluginhost::kHostStateRequestSet, pluginBundle, pluginClass,
                         &data, nullptr, outError, timeoutMs);
  }

  // P2c: pluginBundle/pluginClass select the plugin the host runs on this
  // block. Empty bundle = the legacy -6dB test processor (plain "vst" insert
  // names, keeps the existing rig drill working).
  bool exchange(float* pcm, size_t count, int channels, int sampleRate, int deadlineMs,
                const std::string& pluginBundle = {}, const std::string& pluginClass = {}) {
    using namespace corevideo::pluginhost;
    if (!ready_ || pcm == nullptr || count == 0 || count > static_cast<size_t>(kHostBlockMaxSamples)) {
      return false;
    }
    if (!hostAlive()) {
      deadlineMisses_.fetch_add(1, std::memory_order_relaxed);
      stop();
      return false;
    }

    ::ResetEvent(done_);  // clear any stale completion from an abandoned tick
    std::memcpy(block_->pcm, pcm, count * sizeof(float));
    block_->sampleCount = static_cast<int32_t>(count);
    block_->channels = channels;
    block_->sampleRate = sampleRate;
    copyBlockString(block_->pluginBundle, pluginBundle.c_str());
    copyBlockString(block_->pluginClass, pluginClass.c_str());
    const uint32_t sequence = block_->seqIn + 1;
    block_->seqIn = sequence;
    ::SetEvent(req_);

    const DWORD wait = ::WaitForSingleObject(done_, static_cast<DWORD>(deadlineMs));
    if (wait != WAIT_OBJECT_0 || block_->seqOut != sequence) {
      deadlineMisses_.fetch_add(1, std::memory_order_relaxed);
      return false;  // BYPASS — caller keeps its unprocessed audio
    }

    harvestStatus();
    // The block came back, but the host may have BYPASSED it (plugin load or
    // process failure — statusCode kHostStatusPluginFailed). Copying back is
    // still correct (the pcm is untouched); the failure is surfaced through
    // lastError()/activePlugin() telemetry, never silently faked.
    std::memcpy(pcm, block_->pcm, count * sizeof(float));
    exchanges_.fetch_add(1, std::memory_order_relaxed);
    return true;
  }

  [[nodiscard]] bool hostAlive() const {
    if (process_ == nullptr) {
      return false;
    }
    return ::WaitForSingleObject(process_, 0) == WAIT_TIMEOUT;
  }

  void terminateHostForTest() {
    if (process_ != nullptr) {
      ::TerminateProcess(process_, 9);
      ::WaitForSingleObject(process_, 2000);
    }
  }

  void stop() {
    ready_ = false;
    if (process_ != nullptr) {
      // The host exits on its own after 30s idle; don't kill it abruptly here
      // in case it is mid-block — just drop our handle.
      ::CloseHandle(process_);
      process_ = nullptr;
    }
    if (block_ != nullptr) {
      ::UnmapViewOfFile(block_);
      block_ = nullptr;
    }
    for (HANDLE* handle : {&shm_, &req_, &done_, &editor_, &param_, &stateReq_, &stateDone_}) {
      if (*handle != nullptr) {
        ::CloseHandle(*handle);
        *handle = nullptr;
      }
    }
  }

 private:
  bool stateTransact(int32_t kind, const std::string& pluginBundle, const std::string& pluginClass,
                     const std::vector<uint8_t>* payload, std::vector<uint8_t>* out,
                     std::string* outError, int timeoutMs) {
    using namespace corevideo::pluginhost;
    const auto fail = [&](const std::string& message) {
      if (outError != nullptr) *outError = message;
      return false;
    };
    if (pluginBundle.empty()) {
      return fail("state request without a plugin selection");
    }
    if (payload != nullptr && payload->size() > static_cast<size_t>(kHostStateMaxBytes)) {
      return fail("state payload too large (" + std::to_string(payload->size()) + " bytes > " +
                  std::to_string(kHostStateMaxBytes) + " cap)");
    }
    std::lock_guard<std::mutex> lock(stateMutex_);
    if (!ready_ || block_ == nullptr || stateReq_ == nullptr || stateDone_ == nullptr || !hostAlive()) {
      return fail("isolated VST3 host is not running");
    }
    ::ResetEvent(stateDone_);
    copyBlockString(block_->statePluginBundle, pluginBundle.c_str());
    copyBlockString(block_->statePluginClass, pluginClass.c_str());
    block_->stateRequestKind = kind;
    if (payload != nullptr) {
      if (!payload->empty()) {
        std::memcpy(block_->stateData, payload->data(), payload->size());
      }
      block_->stateDataBytes = static_cast<int32_t>(payload->size());
    }
    const uint32_t generation = block_->stateRequestGeneration + 1;
    block_->stateRequestGeneration = generation;
    ::SetEvent(stateReq_);
    const DWORD wait = ::WaitForSingleObject(stateDone_, static_cast<DWORD>(timeoutMs));
    if (wait != WAIT_OBJECT_0 || block_->stateResponseGeneration != generation) {
      return fail("isolated VST3 host did not answer the state request (timeout)");
    }
    if (block_->stateResultCode != kHostStateResultOk) {
      return fail(std::string(block_->stateError, strnlen(block_->stateError, sizeof(block_->stateError))));
    }
    if (out != nullptr) {
      const int32_t bytes = block_->stateDataBytes;
      const int32_t count = bytes < 0 ? 0 : (bytes > kHostStateMaxBytes ? kHostStateMaxBytes : bytes);
      out->assign(block_->stateData, block_->stateData + count);
    }
    return true;
  }

  // Copies the host's status strings out of the block, but only when the
  // host bumped statusGeneration (status content changed) — steady-state
  // ticks never touch the mutex or allocate.
  void harvestStatus() {
    using namespace corevideo::pluginhost;
    const uint32_t generation = block_->statusGeneration;
    if (generation == harvestedStatusGeneration_) {
      return;
    }
    harvestedStatusGeneration_ = generation;
    const std::string activePlugin(block_->activePlugin,
                                   strnlen(block_->activePlugin, sizeof(block_->activePlugin)));
    const std::string lastError(block_->lastError, strnlen(block_->lastError, sizeof(block_->lastError)));
    std::lock_guard<std::mutex> lock(statusMutex_);
    activePlugin_ = activePlugin;
    lastError_ = lastError;
    statusCode_ = block_->statusCode;
  }

  void harvestEditorStatus() const {
    if (block_ == nullptr) return;
    const uint32_t generation = block_->editorStatusGeneration;
    if (generation == harvestedEditorStatusGeneration_) return;
    harvestedEditorStatusGeneration_ = generation;
    std::lock_guard<std::mutex> lock(statusMutex_);
    editorActivePlugin_.assign(block_->editorActivePlugin,
                               strnlen(block_->editorActivePlugin, sizeof(block_->editorActivePlugin)));
    editorLastError_.assign(block_->editorLastError,
                            strnlen(block_->editorLastError, sizeof(block_->editorLastError)));
    editorStatusCode_ = block_->editorStatusCode;
  }

  std::string instance_;
  HANDLE shm_ = nullptr;
  HANDLE req_ = nullptr;
  HANDLE done_ = nullptr;
  HANDLE editor_ = nullptr;
  HANDLE param_ = nullptr;
  HANDLE stateReq_ = nullptr;
  HANDLE stateDone_ = nullptr;
  HANDLE process_ = nullptr;
  corevideo::pluginhost::HostAudioBlock* block_ = nullptr;
  uint32_t harvestedStatusGeneration_ = 0;  // audio worker only
  mutable uint32_t harvestedEditorStatusGeneration_ = 0;
#else
  bool start(const std::string&, const std::string&) { return false; }
  [[nodiscard]] bool ready() const { return false; }
  [[nodiscard]] int64_t startCount() const { return 0; }
  bool requestEditor(const std::string&, const std::string&) { return false; }
  bool setParam(const std::string&, const std::string&, uint32_t, double) { return false; }
  [[nodiscard]] VstPublishedParams params() const { return {}; }
  [[nodiscard]] uint32_t latencySamples() const { return 0; }
  bool getState(const std::string&, const std::string&, std::vector<uint8_t>*, std::string* outError,
                int = 8000) {
    if (outError != nullptr) *outError = "isolated VST3 host unsupported on this platform";
    return false;
  }
  bool setState(const std::string&, const std::string&, const std::vector<uint8_t>&, std::string* outError,
                int = 8000) {
    if (outError != nullptr) *outError = "isolated VST3 host unsupported on this platform";
    return false;
  }
  bool exchange(float*, size_t, int, int, int, const std::string& = {}, const std::string& = {}) {
    return false;
  }
  [[nodiscard]] bool hostAlive() const { return false; }
  void terminateHostForTest() {}
  void stop() {}

 private:
#endif
  mutable std::mutex statusMutex_;  // leaf: guards the harvested status strings only
  // A2 leaf mutexes: paramsMutex_ guards the harvested param cache (snapshot
  // readers), paramSetMutex_ orders concurrent set-param writers, stateMutex_
  // serializes state round trips. All tiny leaves — never held across the
  // audio exchange, never nested.
  mutable std::mutex paramsMutex_;
  std::mutex paramSetMutex_;
  std::mutex stateMutex_;
  mutable VstPublishedParams harvestedParams_;
  std::string activePlugin_;
  std::string lastError_;
  int32_t statusCode_ = 0;
  mutable std::string editorActivePlugin_;
  mutable std::string editorLastError_;
  mutable int32_t editorStatusCode_ = 0;
  std::atomic<bool> ready_{false};  // starter thread writes, audio worker reads
  std::atomic<int64_t> startCount_{0};
  std::atomic<int64_t> exchanges_{0};
  std::atomic<int64_t> deadlineMisses_{0};

 public:
  [[nodiscard]] int64_t exchanges() const { return exchanges_.load(std::memory_order_relaxed); }
  [[nodiscard]] int64_t deadlineMisses() const { return deadlineMisses_.load(std::memory_order_relaxed); }
  // P2c host-reported status (snapshot telemetry). Safe from any thread.
  [[nodiscard]] std::string activePlugin() const {
    std::lock_guard<std::mutex> lock(statusMutex_);
    return activePlugin_;
  }
  [[nodiscard]] std::string lastError() const {
    std::lock_guard<std::mutex> lock(statusMutex_);
    return lastError_;
  }
  [[nodiscard]] int32_t statusCode() const {
    std::lock_guard<std::mutex> lock(statusMutex_);
    return statusCode_;
  }
  [[nodiscard]] int32_t editorStatusCode() const {
#ifdef _WIN32
    harvestEditorStatus();
#endif
    std::lock_guard<std::mutex> lock(statusMutex_);
    return editorStatusCode_;
  }
  [[nodiscard]] std::string editorActivePlugin() const {
#ifdef _WIN32
    harvestEditorStatus();
#endif
    std::lock_guard<std::mutex> lock(statusMutex_);
    return editorActivePlugin_;
  }
  [[nodiscard]] std::string editorLastError() const {
#ifdef _WIN32
    harvestEditorStatus();
#endif
    std::lock_guard<std::mutex> lock(statusMutex_);
    return editorLastError_;
  }
};

}  // namespace corevideo::modules
