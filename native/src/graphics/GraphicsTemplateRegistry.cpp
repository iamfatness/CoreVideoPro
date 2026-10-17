#include "graphics/GraphicsTemplateRegistry.h"
#include <atomic>

namespace corevideo::graphics {
using rpc::Json;
GraphicsTemplateRegistry::GraphicsTemplateRegistry() {
  for (const auto& builtin : GraphicsTemplate::curated()) {
    definitions_[{builtin->id(), builtin->version()}] = builtin;
    documentBytes_ += builtin->document().stringify().size();
    for (const auto& entry : builtin->assets()) if (assetPool_.emplace(entry.second->sha256,entry.second->encoded).second)
      assetBytes_ += entry.second->encoded->size();
  }
  publishLocked();
}
GraphicsTemplateRegistry::~GraphicsTemplateRegistry() {
  { std::lock_guard lock(mutex_); stopping_ = true; }
  wake_.notify_all();
  if (worker_.joinable()) worker_.join();
}
std::shared_ptr<const GraphicsTemplateRegistry::Publication> GraphicsTemplateRegistry::poll() const {
  return std::atomic_load(&publication_);
}
std::string GraphicsTemplateRegistry::submit(const std::string& client, const std::string& request, const std::string& manifest, const std::string& packageRoot) {
  if (manifest.empty() || manifest.size() > 256 * 1024) return "template-manifest-size";
  if (packageRoot.size() > 2048 || packageRoot.find('\0') != std::string::npos) return "graphics-package-root-invalid";
  // Never wait for validation/catalog publication. Contention is retryable.
  std::unique_lock lock(mutex_, std::try_to_lock);
  if (!lock.owns_lock()) return "template-import-busy";
  for (const auto& old : history_) if (old->client == client && old->request == request)
    return old->manifest == manifest && old->packageRoot == packageRoot ? "template-install-queued" : "request-id-reused";
  size_t pending = 0;
  for (const auto& old : history_) if (old->state == "queued" || old->state == "validating") ++pending;
  if (pending >= 2) return "template-import-busy";
  // No work/status construction under the deadline: wake the owner to publish.
  auto job = std::make_shared<Job>(Job{client, request, manifest, "queued", "", packageRoot});
  if (history_.size() == 16) {
    if (history_.front()->state == "queued" || history_.front()->state == "validating") return "template-import-busy";
    history_.pop_front();
  }
  history_.push_back(job); jobs_.push_back(job);
  if (!worker_.joinable()) worker_ = std::thread([this] { run(); });
  wake_.notify_one();
  return "template-install-queued";
}
void GraphicsTemplateRegistry::publishLocked() {
  auto publication = std::make_shared<Publication>();
  Json::Array catalog, installs;
  for (const auto& preset : {"compact-solid", "minimal-accent", "broadcast"})
    catalog.emplace_back(Json::Object{{"templateId", preset}, {"templateVersion", 1}, {"name", preset}, {"family", "lower-third"}, {"runtime", "compatibility"}});
  for (const auto& [key, definition] : definitions_) {
    auto row = definition->document().asObject(); row["runtime"] = "declarative";
    // Exact package document for save/export. Typed UI descriptors cannot
    // reconstruct omitted defaults or round-trip arbitrary metadata faithfully.
    row["definition"] = definition->document();
    const auto origin = origins_.find(key); if (origin != origins_.end()) row["packageRoot"] = origin->second;
    std::string hash; static constexpr char hex[] = "0123456789abcdef";
    for (auto byte : definition->hash()) { hash += hex[byte >> 4]; hash += hex[byte & 15]; }
    row["manifestHash"] = std::move(hash); catalog.emplace_back(std::move(row));
  }
  for (const auto& job : history_) installs.emplace_back(Json::Object{
    {"clientId", job->client}, {"requestId", job->request}, {"state", job->state}, {"reason", job->reason}});
  publication->definitions = definitions_; publication->catalog = std::move(catalog); publication->installs = std::move(installs);
  publication->encodedAssetBytes = assetBytes_;
  std::atomic_store(&publication_, std::shared_ptr<const Publication>(std::move(publication)));
}
void GraphicsTemplateRegistry::run() {
  for (;;) {
    std::shared_ptr<Job> job;
    {
      std::unique_lock lock(mutex_); wake_.wait(lock, [&] { return stopping_ || !jobs_.empty(); });
      if (stopping_) return;
      job = jobs_.front(); jobs_.pop_front(); job->state = "validating"; publishLocked();
    }
    std::string error;
    auto definition = GraphicsTemplate::load(job->manifest, error);
    GraphicsAssets assets;
    if (definition && !loadGraphicsAssets(definition->document(), job->packageRoot, assets, error)) definition.reset();
    // Reuse verified immutable bytes across IDs and versions. Only this worker
    // touches the pool; failed/colliding imports never publish new pool entries.
    std::map<std::string, std::shared_ptr<const std::vector<uint8_t>>> newAssets;
    uint64_t addedBytes = 0;
    if (definition) {
      for (auto& entry : assets) {
        auto asset = std::make_shared<GraphicsAsset>(*entry.second);
        const auto existing = assetPool_.find(asset->sha256);
        if (existing != assetPool_.end()) asset->encoded = existing->second;
        else {
          const auto [found, inserted] = newAssets.emplace(asset->sha256, asset->encoded);
          if (inserted) addedBytes += asset->encoded->size(); else asset->encoded = found->second;
        }
        entry.second = std::move(asset);
      }
      if (addedBytes > 128ull * 1024 * 1024 - assetBytes_) { error = "graphics-encoded-asset-capacity"; definition.reset(); }
      else definition = GraphicsTemplate::load(job->manifest, error, std::move(assets));
    }
    const auto bytes = definition ? definition->document().stringify().size() : 0;
    {
      std::lock_guard lock(mutex_);
      if (stopping_) return;
      if (definition) {
        const auto key = std::make_pair(definition->id(), definition->version());
        const auto found = definitions_.find(key);
        if (key.first == "compact-solid" || key.first == "minimal-accent" || key.first == "broadcast") error = "template-id-reserved";
        else if (found != definitions_.end()) {
          if (found->second->hash() != definition->hash()) error = "template-version-immutable";
          else definitions_[key] = definition; // explicit repair affects future instances only
        } else if (definitions_.size() >= 16 || documentBytes_ + bytes > 256 * 1024) error = "template-library-capacity";
        else { definitions_[key] = definition; documentBytes_ += bytes; }
        if (error.empty()) {
          for (auto& asset : newAssets) assetPool_.emplace(std::move(asset));
          assetBytes_ += addedBytes;
          origins_[key] = job->packageRoot;
        }
      }
      job->state = error.empty() && definition ? "installed" : "failed";
      job->reason = error;
      publishLocked();
    }
  }
}
} // namespace corevideo::graphics
