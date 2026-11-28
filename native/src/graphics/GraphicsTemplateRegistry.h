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
    // Built-in style packs: [{id, name, genre, version, state, detail?, templates[]}]
    // and the palette the declarative thumbnails render under: {revision, palette}.
    rpc::Json packs, thumbnailPalette;
    std::map<std::pair<std::string, int>, std::shared_ptr<const GraphicsTemplate>> definitions;
    uint64_t encodedAssetBytes = 0;
    uint64_t documentBytes = 0;
  };
  std::string submit(const std::string& client, const std::string& request, const std::string& manifest, const std::string& packageRoot = {});
  // A style pack folder (GraphicsPack.h): the worker lints it, then installs
  // every template it lists with the pack root as package root, all or none,
  // as one queued install. Same admission and replay rules as submit().
  std::string submitPack(const std::string& client, const std::string& request, const std::string& packRoot);
  // Built-in style packs (graphics styles slice 11). Queues one worker job that
  // lints every pack folder under `dir` (graphicsPacksDirectory() in
  // production) and publishes each passing pack's templates as built-in
  // catalog rows carrying `pack` {id, name, genre}. Built-in pack templates
  // never spend the imported library's 16 versions or 256 KiB and cannot be
  // removed. A pack with any problem publishes nothing and is listed in
  // Publication::packs as "invalid" with its first problem. Never blocks.
  bool loadBuiltInPacks(const std::string& dir);
  // A shared immutable lease: a stalled worker cannot stall a frame.
  std::shared_ptr<const Publication> poll() const;
  // `palette` (token -> #RRGGBBAA, already validated; null/empty = the
  // reference appearance) is what declarative thumbnails render with. A new
  // palette re-renders them (compatibility presets ignore it) and bumps
  // thumbnailPalette.revision; each declarative thumbnail names the revision
  // it was rendered under, so a stale card is never shown as current.
  bool requestThumbnails(const rpc::Json& palette = {});
  // Frees an unused imported version's slot and document bytes. The caller
  // (the instance owner) decides in-use first; the registry refuses built-ins
  // and unknown versions. Contention is retryable, never waited on.
  std::string remove(const std::string& templateId, int templateVersion);
  static bool builtIn(const std::string& templateId, int templateVersion);
 private:
  struct Job { std::string client, request, manifest, state, reason, packageRoot, packRoot, detail, builtInPacks; };
  void loadPacks(const std::string& dir);
  bool packKey(const std::pair<std::string, int>& key) const { return packKeys_.count(key) != 0; }
  std::string admit(std::shared_ptr<Job> job);
  // Validates one document and its assets against the pool without
  // publishing; staged pool entries and bytes accumulate across a pack.
  std::shared_ptr<const GraphicsTemplate> prepare(const std::string& manifest, const std::string& packageRoot,
      std::map<std::string, std::shared_ptr<const std::vector<uint8_t>>>& newAssets, uint64_t& addedBytes, std::string& error);
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
  rpc::Json thumbnailPalette_;
  uint64_t thumbnailPaletteRevision_ = 0;
  // Built-in pack templates (key -> {id, name, genre}) and per-pack status rows.
  std::map<std::pair<std::string, int>, rpc::Json> packKeys_;
  rpc::Json::Array packs_;
  std::map<std::pair<std::string,int>,rpc::Json> thumbnails_;
  bool stopping_ = false;
  std::thread worker_;
  std::shared_ptr<const Publication> publication_;
};
} // namespace corevideo::graphics
