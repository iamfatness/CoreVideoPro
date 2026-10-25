#pragma once
#include "core/BoundedAsyncLog.h"
#include <atomic>
#include <cstdint>
#include <mutex>

namespace corevideo::modules {

// #1030 lock guardrail. ZoomEngineRuntime::mutex_ is held across event
// handling (full state snapshots, roster-fact JSON) and the video ingest poll,
// so taking it while holding coreMutex on the sub-ms audio gather measured
// 8.5 ms worst (see MediaCore::pollZoomAudioUnlocked). Code that must never
// take the engine mutex opens a ForbidEngineMutexScope; any acquisition inside
// one is counted (and logged once) so tests can assert the path stays clean.
class EngineMutexGuardrail {
 public:
  static std::atomic<std::uint64_t>& forbiddenAcquisitions() {
    static std::atomic<std::uint64_t> count{0};
    return count;
  }
  static int& forbiddenDepth() {
    thread_local int depth = 0;
    return depth;
  }
  static const char*& forbiddenSite() {
    thread_local const char* site = nullptr;
    return site;
  }
  static void noteAcquire() {
    if (forbiddenDepth() <= 0) return;
    if (forbiddenAcquisitions().fetch_add(1, std::memory_order_relaxed) == 0) {
      ::corevideo::core::nativeLogf("[lock-guardrail] engine mutex taken inside forbidden scope '%s' (#1030)\n",
                                    forbiddenSite() ? forbiddenSite() : "?");
    }
  }
};

class ForbidEngineMutexScope {
 public:
  explicit ForbidEngineMutexScope(const char* site) : previous_(EngineMutexGuardrail::forbiddenSite()) {
    ++EngineMutexGuardrail::forbiddenDepth();
    EngineMutexGuardrail::forbiddenSite() = site;
  }
  ~ForbidEngineMutexScope() {
    --EngineMutexGuardrail::forbiddenDepth();
    EngineMutexGuardrail::forbiddenSite() = previous_;
  }
  ForbidEngineMutexScope(const ForbidEngineMutexScope&) = delete;
  ForbidEngineMutexScope& operator=(const ForbidEngineMutexScope&) = delete;

 private:
  const char* previous_;
};

// #1037: the ONLY way to take the engine mutex inside a forbidden scope. Each
// use is a reviewed, named exception; `grep AllowEngineMutexScope` lists them.
// It suspends the enclosing forbidden scope(s) on this thread and restores them
// (depth and site) on exit.
class AllowEngineMutexScope {
 public:
  explicit AllowEngineMutexScope(const char* reason)
      : savedDepth_(EngineMutexGuardrail::forbiddenDepth()),
        savedSite_(EngineMutexGuardrail::forbiddenSite()) {
    EngineMutexGuardrail::forbiddenDepth() = 0;
    EngineMutexGuardrail::forbiddenSite() = reason;
  }
  ~AllowEngineMutexScope() {
    EngineMutexGuardrail::forbiddenDepth() = savedDepth_;
    EngineMutexGuardrail::forbiddenSite() = savedSite_;
  }
  AllowEngineMutexScope(const AllowEngineMutexScope&) = delete;
  AllowEngineMutexScope& operator=(const AllowEngineMutexScope&) = delete;

 private:
  int savedDepth_;
  const char* savedSite_;
};

// std::mutex plus the guardrail check. Satisfies Lockable.
class EngineMutex {
 public:
  void lock() { EngineMutexGuardrail::noteAcquire(); mutex_.lock(); }
  bool try_lock() { EngineMutexGuardrail::noteAcquire(); return mutex_.try_lock(); }
  void unlock() { mutex_.unlock(); }

 private:
  std::mutex mutex_;
};

}  // namespace corevideo::modules
