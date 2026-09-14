#pragma once
#include "core/ShowPlanGenerator.h"
#include <atomic>
#include <chrono>
#include <functional>
#include <thread>

namespace corevideo::core {
// Standalone foundation. No MediaCore, RPC, compositor or output registration.
class ProgramRenderWorker final {
 public:
  struct Generation {
    std::string authorityEpoch, clockId;
    uint64_t renderer{1}, clock{1};
    bool operator==(const Generation&) const = default;
  };
  class Work final {
   public:
    const Generation generation;
    const uint64_t revision;
    const int64_t slot, deadlineNs;
    const ShowPlans plans;
    // Freezes an owning copy BEFORE submission. All times use steady_clock ns.
    Work(Generation, uint64_t revision, int64_t slot, int64_t deadlineNs, const ShowPlans&);
  };
  class Renderer {
   public:
    virtual ~Renderer() = default;
    // Creation, render and destruction all execute on one owner thread. Poll
    // cancellation and bound GPU/driver waits; never call back into the worker.
    virtual bool render(const Work&, const std::atomic<bool>& cancelled) = 0;
  };
  using Factory = std::function<std::unique_ptr<Renderer>(const Generation&, const std::atomic<bool>&)>;
  struct Config {
    bool enabled{false};
    Generation generation;
    size_t capacity{2};
    std::chrono::milliseconds shutdownBudget{50}, stallThreshold{100};
  };
  enum class Admission { Accepted, Coalesced, Disabled, Stopped, Contended, Stale, Invalid };
  struct Diagnostics {
    uint64_t accepted{0}, coalesced{0}, rendered{0}, failed{0}, stalled{0}, skipped{0},
        contended{0}, rejectedStale{0}, staleCompletions{0}, deadlineMisses{0};
    size_t queued{0}, maximumQueued{0};
    bool enabled{false}, busy{false}, stopped{false}, shutdownComplete{false};
    // Callback completion only: no GPU readiness, delivery or display claim.
  };
  ProgramRenderWorker(Config, Factory);
  ~ProgramRenderWorker();
  ProgramRenderWorker(const ProgramRenderWorker&) = delete;
  ProgramRenderWorker& operator=(const ProgramRenderWorker&) = delete;
  Admission submit(std::shared_ptr<const Work>);
  // Compare-and-swap generation fence. Monotonic renderer generation never resets.
  Admission reset(const Generation& expected, Generation next);
  Diagnostics diagnostics() const;
  std::shared_ptr<const Work> lastRendered() const;
  // Bounded wait. False means cooperative shutdown is incomplete; shared state
  // survives a detached worker so even a broken renderer cannot cause a UAF.
  bool shutdown() noexcept;
 private:
  struct State;
  static void run(std::shared_ptr<State>, Factory);
  // Requires joinMutex_. Sole owner of every thread_ access, including on the
  // failure path, so no caller can touch the thread outside the lock.
  bool retireJoined(std::shared_ptr<State>) noexcept;
  std::shared_ptr<State> state_;
  std::thread thread_;
  std::mutex joinMutex_;
};
}
