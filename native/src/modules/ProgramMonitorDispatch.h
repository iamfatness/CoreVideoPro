#pragma once
#include "modules/Interfaces.h"
#include <algorithm>
#include <mutex>
#include <optional>

namespace corevideo::modules {
// Join one early scheduled copy with its same-slot optional monitor payload.
// Neither producer waits. Never attach a previous/future copy to another slot.
class ProgramMonitorDispatch {
 public:
  struct Result { std::optional<MonitorRenderRequest> ready; bool superseded = false; };
  Result offer(int64_t slot, MonitorRenderRequest request, bool copy) {
    Result result;
    std::optional<Entry> retired, obsolete;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      if (slot <= closedThrough_) return result;
      auto& own = copy ? copy_ : monitor_;
      auto& other = copy ? monitor_ : copy_;
      if (own && own->slot > slot) { result.superseded = true; return result; }
      retired.swap(own);
      result.superseded = retired.has_value();
      own.emplace(Entry{slot, std::move(request)});
      if (other && other->slot != slot) {
        result.superseded = true;
        if (other->slot < slot) obsolete.swap(other);
        else obsolete.swap(own);
      }
      if (copy_ && monitor_ && copy_->slot == monitor_->slot) {
        result.ready.emplace(std::move(monitor_->request));
        auto& ready = *result.ready;
        ready.programCopyPrepared = true;
        ready.readyProgramImage = std::move(copy_->request.readyProgramImage);
        ready.readyProgramObservation = std::move(copy_->request.readyProgramObservation);
        ready.programDisplayTargetNs = copy_->request.programDisplayTargetNs;
        ready.programDisplayDeadlineNs = copy_->request.programDisplayDeadlineNs;
        ready.deliveredProgramOwner.reset(); ready.acquireDeliveredProgram = {};
        closedThrough_ = slot;
        copy_.reset(); monitor_.reset();
      }
    }
    // Old input/image leases are destroyed outside the metadata lock.
    return result;
  }
  void discardThrough(int64_t slot) {
    std::optional<Entry> copy, monitor;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      closedThrough_ = (std::max)(closedThrough_, slot);
      if (copy_ && copy_->slot <= closedThrough_) copy.swap(copy_);
      if (monitor_ && monitor_->slot <= closedThrough_) monitor.swap(monitor_);
    }
  }
 private:
  struct Entry { int64_t slot; MonitorRenderRequest request; };
  std::mutex mutex_;
  std::optional<Entry> copy_, monitor_;
  int64_t closedThrough_ = -1;
};
}
