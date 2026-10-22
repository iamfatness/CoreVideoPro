#pragma once
#include <cstdint>
#include <optional>
#include <utility>

namespace corevideo::modules {
// Capture-owner metadata for one submitted copy. Resolve or cancel it before
// replacing pools or admitting another GPU write. No extra texture, OS-frame
// lease, producer wait or Program buffering is introduced.
class DeferredCaptureGpuCopy {
 public:
  struct Copy {
    std::int64_t sequence = 0, capture100ns = 0;
    std::uint64_t epoch = 0;
    int width = 0, height = 0, productionSlot = -1, monitorSlot = -1;
  };
  bool defer(Copy copy) {
    if (pending_ || copy.sequence <= 0 || !copy.epoch || copy.capture100ns <= 0 ||
        copy.width <= 0 || copy.height <= 0 || copy.productionSlot < 0) return false;
    pending_ = copy;
    return true;
  }
  template <typename Ready, typename Publish>
  bool poll(Ready&& ready, Publish&& publish) {
    if (!pending_) return false;
    auto image = ready(pending_->productionSlot);
    if (!image) return false;
    const auto copy = *pending_;
    pending_.reset();
    publish(copy, std::move(image));
    return true;
  }
  bool pending() const { return pending_.has_value(); }
  void cancel() { pending_.reset(); }
 private:
  std::optional<Copy> pending_;
};
}
