#pragma once
#include "graphics/GraphicsTemplate.h"
#include <condition_variable>
#include <deque>
#include <map>
#include <mutex>
#include <thread>

namespace corevideo::graphics {
// Single bounded package owner. Parsing, validation, SHA and catalog building
// happen here, never on the control/render owner. Versions are immutable.
class GraphicsTemplateRegistry {
 public:
  GraphicsTemplateRegistry();
  ~GraphicsTemplateRegistry();
  GraphicsTemplateRegistry(const GraphicsTemplateRegistry&) = delete;
  struct Publication {
    rpc::Json catalog, installs;
    std::map<std::pair<std::string, int>, std::shared_ptr<const GraphicsTemplate>> definitions;
  };
  std::string submit(const std::string& client, const std::string& request, const std::string& manifest);
  // A shared immutable lease: a stalled worker cannot stall a frame.
  std::shared_ptr<const Publication> poll() const;
 private:
  struct Job { std::string client, request, manifest, state, reason; };
  void run();
  void publishLocked();
  mutable std::mutex mutex_;
  std::condition_variable wake_;
  std::deque<std::shared_ptr<Job>> jobs_, history_;
  std::map<std::pair<std::string, int>, std::shared_ptr<const GraphicsTemplate>> definitions_;
  size_t documentBytes_ = 0;
  bool stopping_ = false;
  std::thread worker_;
  std::shared_ptr<const Publication> publication_;
};
} // namespace corevideo::graphics
