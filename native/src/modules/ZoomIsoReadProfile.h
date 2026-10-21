#pragma once
#include "rpc/Json.h"
#include <array>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#endif

namespace corevideo::modules::iso_profile {
// Opt-in diagnostic only. Bounded aggregates, no pixel retention or per-frame
// logging. CPU counters are raw OS accounting, not a calibrated scheduler trace.
inline bool enabled() {
  static const bool value = [] { const auto* p = std::getenv("COREVIDEO_QA_ISO_READ_PROFILE"); return p && std::string(p) == "1"; }();
  return value;
}
enum Stage { Iteration, Sleep, CaptureWait, CaptureHold, Allocate, Copy, Prepare, PublishWait, PublishHold, StageCount };
inline constexpr const char* names[] = {"iteration", "sleep", "captureWait", "captureHold", "allocate", "copy", "prepare", "publishWait", "publishHold"};
inline thread_local bool isoRead = false;
struct IsoReadScope {
  bool previous = isoRead;
  IsoReadScope() { isoRead = true; }
  ~IsoReadScope() { isoRead = previous; }
};
inline std::string sourceLabel(uint32_t id) { return std::to_string(id) + (isoRead ? ":iso" : ":latest"); }
struct Stamp {
  int64_t ticks = 0;
  uint64_t cpu100ns = 0, cycles = 0;
  uint32_t thread = 0;
  bool cpuKnown = false, cyclesKnown = false;
};
inline uint64_t frequency() {
#ifdef _WIN32
  static const auto value = [] { LARGE_INTEGER f{}; QueryPerformanceFrequency(&f); return static_cast<uint64_t>(f.QuadPart); }();
  return value;
#else
  return 1000000000;
#endif
}
inline Stamp stamp() {
  Stamp s;
#ifdef _WIN32
  LARGE_INTEGER now{}; QueryPerformanceCounter(&now); s.ticks = now.QuadPart;
  s.thread = GetCurrentThreadId();
  FILETIME created{}, exited{}, kernel{}, user{};
  s.cpuKnown = GetThreadTimes(GetCurrentThread(), &created, &exited, &kernel, &user) != FALSE;
  if (s.cpuKnown) s.cpu100ns = (uint64_t(kernel.dwHighDateTime) << 32 | kernel.dwLowDateTime) +
      (uint64_t(user.dwHighDateTime) << 32 | user.dwLowDateTime);
  ULONG64 cycles = 0; s.cyclesKnown = QueryThreadCycleTime(GetCurrentThread(), &cycles) != FALSE; s.cycles = cycles;
#else
  s.ticks = std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
#endif
  return s;
}
struct Row {
  uint64_t count = 0, totalNs = 0, maxNs = 0;
  std::array<uint64_t, 9> histogram{}; // <= .25,.5,1,2,4,8,16,32ms, >32ms
  Stamp start, end;
  uint64_t bytes = 0, frame = 0;
};
struct Profile { std::mutex mutex; std::array<Row, StageCount> rows; };
inline std::mutex registryMutex;
inline std::map<std::string, std::shared_ptr<Profile>> registry;
inline std::shared_ptr<Profile> profile(const std::string& id) {
  if (!enabled()) return {};
  std::lock_guard lock(registryMutex);
  if (auto it = registry.find(id); it != registry.end()) return it->second;
  if (registry.size() >= 32) return {}; // Explicitly absent if the diagnostic capacity is exhausted.
  return registry.emplace(id, std::make_shared<Profile>()).first->second;
}
class Scope {
 public:
  Scope(const std::string& id, Stage stage, uint64_t bytes = 0, uint64_t frame = 0)
      : owner_(profile(id)), stage_(stage), bytes_(bytes), frame_(frame) { if (owner_) begin_ = stamp(); }
  ~Scope() { finish(); }
  void finish() {
    if (!owner_) return;
    const auto end = stamp();
    const auto ns = static_cast<uint64_t>((end.ticks - begin_.ticks) * 1000000000.0 / frequency());
    std::lock_guard lock(owner_->mutex);
    auto& row = owner_->rows[stage_]; ++row.count; row.totalNs += ns;
    constexpr uint64_t bounds[] = {250000,500000,1000000,2000000,4000000,8000000,16000000,32000000};
    size_t bucket = 0; while (bucket < 8 && ns > bounds[bucket]) ++bucket; ++row.histogram[bucket];
    if (ns >= row.maxNs) { row.maxNs = ns; row.start = begin_; row.end = end; row.bytes = bytes_; row.frame = frame_; }
    owner_.reset();
  }
 private:
  std::shared_ptr<Profile> owner_;
  Stage stage_;
  Stamp begin_;
  uint64_t bytes_, frame_;
};
inline rpc::Json snapshot(const std::string& id) {
  if (!enabled()) return nullptr;
  std::shared_ptr<Profile> owner;
  { std::lock_guard lock(registryMutex); const auto it = registry.find(id); if (it == registry.end()) return nullptr; owner = it->second; }
  std::lock_guard lock(owner->mutex);
  rpc::Json::Array stages;
  for (size_t i = 0; i < StageCount; ++i) {
    const auto& row = owner->rows[i]; if (!row.count) continue;
    rpc::Json::Array histogram; for (auto n : row.histogram) histogram.emplace_back(static_cast<double>(n));
    rpc::Json::Object worst{{"startQpc", std::to_string(row.start.ticks)}, {"endQpc", std::to_string(row.end.ticks)},
        {"threadId", static_cast<double>(row.start.thread)}, {"bytes", static_cast<double>(row.bytes)}, {"frameId", static_cast<double>(row.frame)},
        {"cpuKnown", row.start.cpuKnown && row.end.cpuKnown}, {"cyclesKnown", row.start.cyclesKnown && row.end.cyclesKnown}};
    if (row.start.cpuKnown && row.end.cpuKnown) worst.emplace("cpu100ns", static_cast<double>(row.end.cpu100ns - row.start.cpu100ns));
    if (row.start.cyclesKnown && row.end.cyclesKnown) worst.emplace("cycles", std::to_string(row.end.cycles - row.start.cycles));
    stages.emplace_back(rpc::Json::Object{{"stage", names[i]}, {"samples", static_cast<double>(row.count)},
        {"totalNs", static_cast<double>(row.totalNs)}, {"maxNs", static_cast<double>(row.maxNs)},
        {"histogram", std::move(histogram)}, {"worst", std::move(worst)}});
  }
  return rpc::Json::Object{{"schema", "zoom-iso-read-profile-v1"}, {"label", id},
      {"qpcFrequency", std::to_string(frequency())}, {"stages", std::move(stages)},
      {"scope", "Elapsed stage time and raw thread CPU/cycle accounting; CPU counter resolution and scheduler waits require ETW correlation."}};
}
}
