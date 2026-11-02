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
    uint64_t encodedAssetBytes = 0;
    uint64_t documentBytes = 0;
  };
  std::string submit(const std::string& client, const std::string& request, const std::string& manifest, const std::string& packageRoot = {});
  // A shared immutable lease: a stalled worker cannot stall a frame.
  std::shared_ptr<const Publication> poll() const;
  bool requestThumbnails();
  // Frees an unused imported version's slot and document bytes. The caller
  // (the instance owner) decides in-use first; the registry refuses built-ins
  // and unknown versions. Contention is retryable, never waited on.
  std::string remove(const std::string& templateId, int templateVersion);
  static bool builtIn(const std::string& templateId, int templateVersion);
 private:
  struct Job { std::string client, request, manifest, state, reason, packageRoot; };
  void run();
  void publishLocked();
  mutable std::mutex mutex_;
  std::condition_variable wake_;
  std::deque<std::shared_ptr<Job>> jobs_, history_;
  std::map<std::pair<std::string, int>, std::shared_ptr<const GraphicsTemplate>> definitions_;
  size_t documentBytes_ = 0;
  std::map<std::string, std::shared_ptr<const std::vector<uint8_t>>> assetPool_;
  std::map<std::pair<std::string, int>, std::string> origins_;
  uint64_t assetBytes_ = 0;
  bool thumbnailsRequested_=false,thumbnailsPending_=false;
  std::map<std::pair<std::string,int>,rpc::Json> thumbnails_;
  bool stopping_ = false;
  std::thread worker_;
  std::shared_ptr<const Publication> publication_;
};
} // namespace corevideo::graphics
