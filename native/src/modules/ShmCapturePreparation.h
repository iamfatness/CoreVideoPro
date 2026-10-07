#pragma once
#include "modules/Interfaces.h"
#include <condition_variable>
#include <thread>

namespace corevideo::modules {

// Owns mapping, copy, pool allocation and retirement away from Program. The
// render consumer only copies completed VideoFrame descriptors/shared leases.
class ShmCapturePreparation {
 public:
  static constexpr size_t kBudgetBytes = 512u * 1024u * 1024u;
  static constexpr size_t kPoolFrames = 4;
  static constexpr size_t kMaxSources = 16;
  struct Stats {
    uint64_t accepted = 0, refused = 0, prepared = 0, torn = 0, poolBusy = 0, failed = 0;
    uint64_t copyTotalNs = 0, copyMaximumNs = 0;
    size_t residentBytes = 0, active = 0, retiring = 0;
    std::string state = "idle", reason, lastRefusalReason;
  };
  // Test-only callable: no environment, command or settings surface supplies it.
  explicit ShmCapturePreparation(std::function<void()> beforeCopy = {});
  ~ShmCapturePreparation();
  bool registerBuffer(const std::string& id, const std::string& name, int width, int height);
  void unregisterBuffer(const std::string& id);
  [[nodiscard]] std::vector<VideoFrame> latest() const;
  [[nodiscard]] Stats stats() const;
 private:
  struct Request {
    std::string id, name;
    int width = 0, height = 0;
    uint64_t generation = 0;
    size_t bytes = 0;
    VideoFrame held; // frozen before replacement; late old-generation copies cannot update it
  };
  struct Completed { uint64_t generation = 0; VideoFrame frame; };
  struct Mapping;
  using Requests = std::vector<Request>;
  using Frames = std::vector<Completed>;
  void run();
  mutable std::mutex control_;
  std::condition_variable changed_;
  std::shared_ptr<const Requests> wanted_ = std::make_shared<const Requests>();
  std::shared_ptr<const Frames> completed_ = std::make_shared<const Frames>();
  Stats stats_;
  std::atomic<uint64_t> nextGeneration_{1};
  bool stopping_ = false;
  std::function<void()> beforeCopy_;
  std::thread thread_;
};
}
