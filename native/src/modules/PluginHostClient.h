#pragma once

// Core-side plugin-host audio transport client (VST spec P2b).
//
// Owns the kernel objects (SHM block + req/done events), spawns the resident
// `corevideo-plugin-host --serve <instance>` process, and performs the
// synchronous deadline-bounded block exchange the audio worker calls:
//
//   exchange(): copy pcm in → ++seqIn → signal req → wait done (deadline)
//               → verify seqOut == seqIn → copy pcm back.
//
// A missed deadline returns false (BYPASS — the caller keeps its unprocessed
// audio and the show never stalls); stale completions from an abandoned tick
// are discarded by the sequence check + a done-event reset at the top of the
// next exchange. No locks in here: the audio worker is the single caller by
// contract (audioOutputMutex_ work span).
//
// Windows-only transport; other platforms report never-ready so callers fall
// through to the built-in DSP (the guaranteed fallback everywhere).

#include <atomic>
#include <cstdint>
#include <cstring>
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

class PluginHostClient {
 public:
  ~PluginHostClient() { stop(); }

#ifdef _WIN32
  bool start(const std::string& exePath, const std::string& instance) {
    using namespace corevideo::pluginhost;
    if (ready_ || exePath.empty()) {
      return ready_;
    }

    instance_ = instance;
    shm_ = ::CreateFileMappingA(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0,
                                sizeof(HostAudioBlock), hostShmName(instance).c_str());
    req_ = ::CreateEventA(nullptr, FALSE, FALSE, hostReqEventName(instance).c_str());
    done_ = ::CreateEventA(nullptr, FALSE, FALSE, hostDoneEventName(instance).c_str());
    if (shm_ == nullptr || req_ == nullptr || done_ == nullptr) {
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
    ready_ = true;
    return true;
  }

  [[nodiscard]] bool ready() const { return ready_; }

  bool exchange(float* pcm, size_t count, int channels, int sampleRate, int deadlineMs) {
    using namespace corevideo::pluginhost;
    if (!ready_ || pcm == nullptr || count == 0 || count > static_cast<size_t>(kHostBlockMaxSamples)) {
      return false;
    }

    ::ResetEvent(done_);  // clear any stale completion from an abandoned tick
    std::memcpy(block_->pcm, pcm, count * sizeof(float));
    block_->sampleCount = static_cast<int32_t>(count);
    block_->channels = channels;
    block_->sampleRate = sampleRate;
    const uint32_t sequence = block_->seqIn + 1;
    block_->seqIn = sequence;
    ::SetEvent(req_);

    const DWORD wait = ::WaitForSingleObject(done_, static_cast<DWORD>(deadlineMs));
    if (wait != WAIT_OBJECT_0 || block_->seqOut != sequence) {
      deadlineMisses_.fetch_add(1, std::memory_order_relaxed);
      return false;  // BYPASS — caller keeps its unprocessed audio
    }

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
    for (HANDLE* handle : {&shm_, &req_, &done_}) {
      if (*handle != nullptr) {
        ::CloseHandle(*handle);
        *handle = nullptr;
      }
    }
  }

 private:
  std::string instance_;
  HANDLE shm_ = nullptr;
  HANDLE req_ = nullptr;
  HANDLE done_ = nullptr;
  HANDLE process_ = nullptr;
  corevideo::pluginhost::HostAudioBlock* block_ = nullptr;
#else
  bool start(const std::string&, const std::string&) { return false; }
  [[nodiscard]] bool ready() const { return false; }
  bool exchange(float*, size_t, int, int, int) { return false; }
  [[nodiscard]] bool hostAlive() const { return false; }
  void terminateHostForTest() {}
  void stop() {}

 private:
#endif
  bool ready_ = false;
  std::atomic<int64_t> exchanges_{0};
  std::atomic<int64_t> deadlineMisses_{0};

 public:
  [[nodiscard]] int64_t exchanges() const { return exchanges_.load(std::memory_order_relaxed); }
  [[nodiscard]] int64_t deadlineMisses() const { return deadlineMisses_.load(std::memory_order_relaxed); }
};

}  // namespace corevideo::modules
