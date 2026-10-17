#pragma once

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <memory>

namespace corevideo::graphics {

// Process-wide allocation model for graphics preparation, independent of cache
// membership. A texture's charge survives cache eviction, device replacement and
// GPU retirement leases. Driver/library overhead is not measured by this model.
class GraphicsResourceBudget {
  struct State {
    const uint64_t cpuLimit, gpuLimit, preparationLimit;
    std::atomic<uint64_t> cpu{0}, gpu{0}, preparations{0};
    std::atomic<uint64_t> cpuHigh{0}, gpuHigh{0}, preparationsHigh{0}, refused{0};
    State(uint64_t cpuBytes, uint64_t gpuBytes, uint64_t producers)
        : cpuLimit(cpuBytes), gpuLimit(gpuBytes), preparationLimit(producers) {}
  };
 public:
  struct Counters {
    uint64_t cpuBytes, gpuBytes, activePreparations, cpuHighWater, gpuHighWater, preparationHighWater, refused;
    uint64_t cpuLimit, gpuLimit, preparationLimit;
  };
  class Lease {
   public:
    Lease(std::shared_ptr<State> state, uint64_t cpu, uint64_t gpu)
        : state_(std::move(state)), cpu_(cpu), gpu_(gpu) {}
    ~Lease() {
      finishPreparation();
      state_->gpu.fetch_sub(gpu_, std::memory_order_relaxed);
    }
    Lease(const Lease&) = delete;
    Lease& operator=(const Lease&) = delete;
    // Called once by the producer before publication; only the immutable GPU
    // reservation then travels with the prepared asset.
    void finishPreparation() {
      if (!preparing_) return;
      state_->cpu.fetch_sub(cpu_, std::memory_order_relaxed);
      state_->preparations.fetch_sub(1, std::memory_order_relaxed);
      preparing_ = false;
    }
   private:
    std::shared_ptr<State> state_;
    uint64_t cpu_, gpu_;
    bool preparing_ = true;
  };
  explicit GraphicsResourceBudget(uint64_t cpuBytes = 128ull * 1024 * 1024,
      uint64_t gpuBytes = 256ull * 1024 * 1024, uint64_t producers = 2)
      : state_(std::make_shared<State>(cpuBytes, gpuBytes, producers)) {}

  std::shared_ptr<Lease> tryReserve(uint64_t cpuBytes, uint64_t gpuBytes) {
    const auto refuse = [&]() -> std::shared_ptr<Lease> { ++state_->refused; return {}; };
    if (!cpuBytes || !gpuBytes || !take(state_->preparations, 1, state_->preparationLimit, state_->preparationsHigh)) return refuse();
    if (!take(state_->cpu, cpuBytes, state_->cpuLimit, state_->cpuHigh)) {
      --state_->preparations; return refuse();
    }
    if (!take(state_->gpu, gpuBytes, state_->gpuLimit, state_->gpuHigh)) {
      state_->cpu.fetch_sub(cpuBytes); --state_->preparations; return refuse();
    }
    std::shared_ptr<Lease> lease;
    try { lease = std::make_shared<Lease>(state_, cpuBytes, gpuBytes); }
    catch (...) {
      state_->cpu.fetch_sub(cpuBytes); state_->gpu.fetch_sub(gpuBytes); --state_->preparations;
      throw;
    }
    return lease;
  }
  Counters counters() const {
    return {state_->cpu.load(), state_->gpu.load(), state_->preparations.load(), state_->cpuHigh.load(),
        state_->gpuHigh.load(), state_->preparationsHigh.load(), state_->refused.load(),
        state_->cpuLimit, state_->gpuLimit, state_->preparationLimit};
  }
  static GraphicsResourceBudget& process() {
    static GraphicsResourceBudget value;
    return value;
  }
 private:
  static bool take(std::atomic<uint64_t>& counter, uint64_t amount, uint64_t limit, std::atomic<uint64_t>& peak) {
    auto used = counter.load(std::memory_order_relaxed);
    do { if (amount > limit || used > limit - amount) return false; }
    while (!counter.compare_exchange_weak(used, used + amount, std::memory_order_relaxed));
    high(peak, used + amount);
    return true;
  }
  static void high(std::atomic<uint64_t>& counter, uint64_t value) {
    auto current = counter.load(std::memory_order_relaxed);
    while (current < value && !counter.compare_exchange_weak(current, value, std::memory_order_relaxed)) {}
  }
  std::shared_ptr<State> state_;
};
} // namespace corevideo::graphics
