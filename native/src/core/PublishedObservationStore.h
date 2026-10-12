#pragma once

#include <array>
#include <chrono>
#include <cstdint>
#include <memory>
#include <mutex>
#include <utility>
#include <type_traits>

namespace corevideo::core {

// One owner, bounded immutable reader leases. Readers never enter the owner's
// work lock. Construction and destruction run outside the publication mutex.
template <typename T, std::size_t Capacity = 4>
class PublishedObservationStore {
  static_assert(Capacity >= 2, "Publication needs a current and a construction slot");
 public:
  using Clock = std::chrono::steady_clock;
  struct Observation {
    T value;
    std::uint64_t sequence = 0;
    std::uint64_t revision = 0;
    Clock::time_point publishedAt{};
    std::size_t bytes = sizeof(T);
  };
  struct View {
    std::shared_ptr<const Observation> payload;
    std::uint64_t refused = 0;
    std::size_t residentBytes = 0;
  };

  explicit PublishedObservationStore(std::size_t maxPayloadBytes = 512 * 1024) : maxPayloadBytes_(maxPayloadBytes) {
    for (auto& slot : slots_) slot = std::make_shared<Observation>();
  }

  // A single owner calls publish. Holding old views cannot grow the pool or
  // block the owner. A stalled builder cannot prevent readers leasing current.
  template <typename Build>
  bool publish(std::uint64_t revision, Build&& build, Clock::time_point now = Clock::now()) {
    std::size_t selected = Capacity;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      for (std::size_t index = 0; index < Capacity; ++index) {
        if (index != current_ && !building_[index] && slots_[index].use_count() == 1) {
          selected = index;
          building_[index] = true;
          break;
        }
      }
      if (selected == Capacity) { ++refused_; return false; }
    }
    try {
      std::size_t bytes = sizeof(T);
      if constexpr (std::is_void_v<std::invoke_result_t<Build, T&>>) {
        build(slots_[selected]->value);
      } else {
        bytes = build(slots_[selected]->value);
      }
      if (bytes > maxPayloadBytes_) {
        // An oversized draft never becomes visible. Destroy it before clearing
        // the reservation, outside both publication and owner work locks.
        slots_[selected]->value = T{};
        std::lock_guard<std::mutex> lock(mutex_);
        slots_[selected]->bytes = sizeof(T);
        building_[selected] = false;
        ++refused_;
        return false;
      }
      slots_[selected]->bytes = bytes;
    } catch (...) {
      slots_[selected]->value = T{};
      std::lock_guard<std::mutex> lock(mutex_);
      slots_[selected]->bytes = sizeof(T);
      building_[selected] = false;
      ++refused_;
      throw;
    }
    {
      std::lock_guard<std::mutex> lock(mutex_);
      auto& observation = *slots_[selected];
      observation.sequence = ++sequence_;
      observation.revision = revision;
      observation.publishedAt = now;
      building_[selected] = false;
      current_ = selected;
    }
    return true;
  }

  [[nodiscard]] View acquire() const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::size_t bytes = 0;
    for (std::size_t i = 0; i < Capacity; ++i) if (!building_[i]) bytes += slots_[i]->bytes;
    return {current_ == Capacity ? nullptr : slots_[current_], refused_, bytes};
  }

 private:
  mutable std::mutex mutex_;
  std::array<std::shared_ptr<Observation>, Capacity> slots_;
  std::array<bool, Capacity> building_{};
  std::size_t current_ = Capacity;
  std::uint64_t sequence_ = 0;
  std::uint64_t refused_ = 0;
  const std::size_t maxPayloadBytes_;
};

}  // namespace corevideo::core
