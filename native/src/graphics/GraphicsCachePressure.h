#pragma once
#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <vector>
namespace corevideo::graphics {
// Lifetime-safe notification only. A producer under process-wide pressure
// wakes other preparation owners; each owner retires its own unused cache entry.
// No callback captures a renderer and no render thread waits for reclamation.
class GraphicsCachePressure {
 public:
  struct Signal {std::atomic<bool> requested{false};std::condition_variable wake;};
  static std::shared_ptr<Signal> subscribe() {
    auto signal=std::make_shared<Signal>();auto& state=registry();std::lock_guard lock(state.mutex);
    std::erase_if(state.signals,[](const auto& value){return value.expired();});
    if(state.signals.size()<64) state.signals.emplace_back(signal);
    return signal;
  }
  static void requestTrim() {
    auto& state=registry();std::lock_guard lock(state.mutex);
    for(const auto& weak:state.signals) if(auto signal=weak.lock()) {signal->requested=true;signal->wake.notify_one();}
  }
 private:
  struct Registry {std::mutex mutex;std::vector<std::weak_ptr<Signal>> signals;};
  static Registry& registry() {static Registry state;return state;}
};
}
