#include "graphics/GraphicsTemplateRegistry.h"
#include "graphics/GraphicsPack.h"
#include "graphics/GraphicsThumbnail.h"
#include <algorithm>
#include <atomic>
#include <filesystem>

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
bool GraphicsTemplateRegistry::requestThumbnails(const Json& palette) {
  std::unique_lock lock(mutex_,std::try_to_lock);if(!lock.owns_lock() || stopping_) return false;
  // An empty map is the reference appearance, the same as no palette.
  const Json next = palette.isObject() && !palette.asObject().empty() ? palette : Json{};
  const bool recolour = next.stringify() != thumbnailPalette_.stringify();
  if (recolour) {
    thumbnailPalette_ = next; ++thumbnailPaletteRevision_;
    // Only declarative looks draw with the palette; compatibility presets keep theirs.
    for (auto it = thumbnails_.begin(); it != thumbnails_.end();)
      it = definitions_.count(it->first) ? thumbnails_.erase(it) : std::next(it);
  }
  if(thumbnailsRequested_ && !recolour) return true;
  thumbnailsRequested_=thumbnailsPending_=true;
  if (recolour) publishLocked();
  if(!worker_.joinable()) worker_=std::thread([this]{run();});wake_.notify_one();return true;
}
bool GraphicsTemplateRegistry::loadBuiltInPacks(const std::string& dir) {
  if (dir.empty() || dir.size() > 2048 || dir.find('\0') != std::string::npos) return false;
  std::lock_guard lock(mutex_);
  if (stopping_) return false;
  auto job = std::make_shared<Job>();
  job->state = "queued"; job->builtInPacks = dir;
  // Not an operator install: never in history_/templateInstalls, never refused as busy.
  jobs_.push_back(job);
  if (!worker_.joinable()) worker_ = std::thread([this] { run(); });
  wake_.notify_one();
  return true;
}
bool GraphicsTemplateRegistry::builtIn(const std::string& templateId, int templateVersion) {
  if (templateVersion == 1 && (templateId == "compact-solid" || templateId == "minimal-accent" || templateId == "broadcast")) return true;
  for (const auto& builtin : GraphicsTemplate::curated())
    if (builtin->id() == templateId && builtin->version() == templateVersion) return true;
  return false;
}
std::string GraphicsTemplateRegistry::remove(const std::string& templateId, int templateVersion) {
  if (builtIn(templateId, templateVersion)) return "template-built-in";
  std::unique_lock lock(mutex_, std::try_to_lock);
  if (!lock.owns_lock()) return "template-library-busy";
  const auto key = std::make_pair(templateId, templateVersion);
  const auto found = definitions_.find(key);
  if (found == definitions_.end()) return "template-not-found";
  if (packKey(key)) return "template-built-in";
  // Same accounting as install. Verified encoded assets stay in the
  // content-addressed pool so a re-import by hash still resolves.
  documentBytes_ -= found->second->document().stringify().size();
  definitions_.erase(found); origins_.erase(key); thumbnails_.erase(key);
  publishLocked();
  return "template-removed";
}
std::string GraphicsTemplateRegistry::submit(const std::string& client, const std::string& request, const std::string& manifest, const std::string& packageRoot) {
  if (manifest.empty() || manifest.size() > 256 * 1024) return "template-manifest-size";
  if (packageRoot.size() > 2048 || packageRoot.find('\0') != std::string::npos) return "graphics-package-root-invalid";
  return admit(std::make_shared<Job>(Job{client, request, manifest, "queued", "", packageRoot, "", ""}));
}
std::string GraphicsTemplateRegistry::submitPack(const std::string& client, const std::string& request, const std::string& packRoot) {
  if (packRoot.empty() || packRoot.size() > 2048 || packRoot.find('\0') != std::string::npos) return "graphics-pack-root-invalid";
  return admit(std::make_shared<Job>(Job{client, request, "", "queued", "", "", packRoot, ""}));
}
std::string GraphicsTemplateRegistry::admit(std::shared_ptr<Job> job) {
  // Never wait for validation/catalog publication. Contention is retryable.
  std::unique_lock lock(mutex_, std::try_to_lock);
  if (!lock.owns_lock()) return "template-import-busy";
  for (const auto& old : history_) if (old->client == job->client && old->request == job->request)
    return old->manifest == job->manifest && old->packageRoot == job->packageRoot && old->packRoot == job->packRoot
        ? "template-install-queued" : "request-id-reused";
  size_t pending = 0;
  for (const auto& old : history_) if (old->state == "queued" || old->state == "validating") ++pending;
  if (pending >= 2) return "template-import-busy";
  // No work/status construction under the deadline: wake the owner to publish.
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
  const auto thumbnail=[&](const std::pair<std::string,int>& key)->Json {const auto found=thumbnails_.find(key);return found==thumbnails_.end()?Json::Object{{"state",thumbnailsRequested_?"pending":"not-requested"}}:found->second;};
  for (const auto& preset : {"compact-solid", "minimal-accent", "broadcast"})
    catalog.emplace_back(Json::Object{{"templateId", preset}, {"templateVersion", 1}, {"name", preset}, {"family", "lower-third"}, {"runtime", "compatibility"},{"thumbnail",thumbnail({preset,1})}});
  for (const auto& [key, definition] : definitions_) {
    auto row = definition->document().asObject(); row["runtime"] = "declarative";
    // Exact package document for save/export. Typed UI descriptors cannot
    // reconstruct omitted defaults or round-trip arbitrary metadata faithfully.
    row["definition"] = definition->document();
    row["thumbnail"]=thumbnail(key);
    const auto origin = origins_.find(key); if (origin != origins_.end()) row["packageRoot"] = origin->second;
    // Built-in style pack templates (slice 11) say which pack they belong to.
    if (const auto pack = packKeys_.find(key); pack != packKeys_.end()) { row["pack"] = pack->second; row["builtIn"] = true; }
    std::string hash; static constexpr char hex[] = "0123456789abcdef";
    for (auto byte : definition->hash()) { hash += hex[byte >> 4]; hash += hex[byte & 15]; }
    row["manifestHash"] = std::move(hash); catalog.emplace_back(std::move(row));
  }
  for (const auto& job : history_) {
    Json::Object row{{"clientId", job->client}, {"requestId", job->request}, {"state", job->state}, {"reason", job->reason}};
    // A pack install names its folder and, on failure, the first lint problem.
    if (!job->packRoot.empty()) row["packRoot"] = job->packRoot;
    if (!job->detail.empty()) row["detail"] = job->detail;
    installs.emplace_back(std::move(row));
  }
  publication->definitions = definitions_; publication->catalog = std::move(catalog); publication->installs = std::move(installs);
  publication->packs = packs_;
  publication->thumbnailPalette = Json::Object{{"revision", double(thumbnailPaletteRevision_)}, {"palette", thumbnailPalette_}};
  publication->encodedAssetBytes = assetBytes_; publication->documentBytes = documentBytes_;
  std::atomic_store(&publication_, std::shared_ptr<const Publication>(std::move(publication)));
}
std::shared_ptr<const GraphicsTemplate> GraphicsTemplateRegistry::prepare(const std::string& manifest, const std::string& packageRoot,
    std::map<std::string, std::shared_ptr<const std::vector<uint8_t>>>& newAssets, uint64_t& addedBytes, std::string& error) {
  auto definition = GraphicsTemplate::load(manifest, error);
  GraphicsAssets assets;
  if (definition && packageRoot.empty() && definition->document().get("assets")) {
    for(const auto& declaration:definition->document().get("assets")->asArray()) {
      const auto found=assetPool_.find(declaration.getString("sha256"));
      if(found==assetPool_.end()) {
        if(declaration.get("optional") && declaration.get("optional")->asBool()) continue;
        error="graphics-package-root-required"; definition.reset(); break;
      }
      auto asset=graphicsAssetFromBytes(declaration.getString("id"),found->second,error);
      if(!asset) {definition.reset();break;}
      const auto file=declaration.getString("file"); const bool png=file.size()>=4 && file.substr(file.size()-4)==".png";
      const auto& bytes=*asset->encoded; const bool jpeg=bytes.size()>=2 && bytes[0]==0xff && bytes[1]==0xd8;
      if(png==jpeg) {error="graphics-asset-extension-mismatch";definition.reset();break;}
      assets.emplace(asset->id,std::move(asset));
    }
  } else if (definition && !loadGraphicsAssets(definition->document(), packageRoot, assets, error)) definition.reset();
  if (!definition) return definition;
  // Reuse verified immutable bytes across IDs and versions. Only this worker
  // touches the pool; failed/colliding imports never publish new pool entries.
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
  constexpr uint64_t assetLimit = 128ull * 1024 * 1024;
  if (addedBytes > assetLimit - assetBytes_) {
    for (auto it = assetPool_.begin(); it != assetPool_.end() && addedBytes > assetLimit - assetBytes_;) {
      // A live definition, prepared revision or private draft retains its own lease.
      if (it->second.use_count() == 1) {
        assetBytes_ -= it->second->size();
        it = assetPool_.erase(it);
      } else ++it;
    }
  }
  if (addedBytes > assetLimit - assetBytes_) { error = "graphics-encoded-asset-capacity"; return {}; }
  return GraphicsTemplate::load(manifest, error, std::move(assets));
}
void GraphicsTemplateRegistry::run() {
  for (;;) {
    std::shared_ptr<Job> job;
    std::map<std::pair<std::string,int>,std::shared_ptr<const GraphicsTemplate>> thumbnailWork;
    Json thumbnailPalette; uint64_t thumbnailRevision = 0;
    {
      std::unique_lock lock(mutex_); wake_.wait(lock, [&] { return stopping_ || !jobs_.empty() || thumbnailsPending_; });
      if (stopping_) return;
      if(thumbnailsPending_) {
        thumbnailsPending_=false;
        thumbnailPalette=thumbnailPalette_; thumbnailRevision=thumbnailPaletteRevision_;
        for(const auto& preset:{"compact-solid","minimal-accent","broadcast"}) if(!thumbnails_.contains({preset,1})) thumbnailWork[{preset,1}]={};
        for(const auto& item:definitions_) if(!thumbnails_.contains(item.first)) thumbnailWork.emplace(item);
      } else {job = jobs_.front(); jobs_.pop_front(); job->state = "validating"; publishLocked();}
    }
    if(!job) {
      for(const auto& [key,definition]:thumbnailWork) {
        auto result=renderGraphicsThumbnail(key.first,definition,definition?thumbnailPalette:Json{});
        if(definition && result.isObject()) {auto row=result.asObject();row["paletteRevision"]=double(thumbnailRevision);result=std::move(row);}
        std::lock_guard lock(mutex_);if(stopping_) return;
        // A newer palette arrived mid-pass: drop this render; the pending pass redraws in it.
        if(definition && thumbnailRevision!=thumbnailPaletteRevision_) break;
        thumbnails_[key]=std::move(result);publishLocked();
      }
      continue;
    }
    if (!job->builtInPacks.empty()) { loadPacks(job->builtInPacks); continue; }
    std::string error, detail;
    std::vector<std::string> manifests;
    std::string packageRoot = job->packageRoot;
    if (job->packRoot.empty()) manifests.push_back(job->manifest);
    else {
      // The full pack lint runs first; a pack with any problem installs nothing.
      GraphicsPackManifest pack;
      if (!graphicsPackInstallManifests(job->packRoot, manifests, detail) ||
          !readGraphicsPackManifest(job->packRoot, pack, detail)) { error = "graphics-pack-invalid"; manifests.clear(); }
      else packageRoot = pack.root;
    }
    std::vector<std::shared_ptr<const GraphicsTemplate>> definitions;
    std::map<std::string, std::shared_ptr<const std::vector<uint8_t>>> newAssets;
    uint64_t addedBytes = 0;
    for (const auto& manifest : manifests) {
      auto definition = prepare(manifest, packageRoot, newAssets, addedBytes, error);
      if (!definition) {
        if (!job->packRoot.empty()) detail = "template " + std::to_string(definitions.size() + 1) + ": " + error;
        definitions.clear();
        break;
      }
      definitions.push_back(std::move(definition));
    }
    {
      std::lock_guard lock(mutex_);
      if (stopping_) return;
      // All or none: decide every key before publishing any.
      size_t added = 0, addedDocumentBytes = 0;
      for (const auto& definition : definitions) {
        if (!error.empty()) break;
        const auto key = std::make_pair(definition->id(), definition->version());
        const auto found = definitions_.find(key);
        if (key.first == "compact-solid" || key.first == "minimal-accent" || key.first == "broadcast") error = "template-id-reserved";
        else if (found != definitions_.end()) { if (found->second->hash() != definition->hash()) error = "template-version-immutable"; }
        else { ++added; addedDocumentBytes += definition->document().stringify().size(); }
      }
      // Built-in pack templates (slice 11) never spend the imported library's slots.
      if (error.empty() && added && (definitions_.size() - packKeys_.size() + added > 16 || documentBytes_ + addedDocumentBytes > 256 * 1024))
        error = "template-library-capacity";
      if (error.empty() && !definitions.empty()) {
        for (const auto& definition : definitions) {
          const auto key = std::make_pair(definition->id(), definition->version());
          // An identical re-import is an explicit repair: future instances only.
          if (!definitions_.count(key)) documentBytes_ += definition->document().stringify().size();
          definitions_[key] = definition;
          origins_[key] = packageRoot;
          if(thumbnailsRequested_) {thumbnails_.erase(key);thumbnailsPending_=true;}
        }
        for (auto& asset : newAssets) assetPool_.emplace(std::move(asset));
        assetBytes_ += addedBytes;
      }
      job->state = error.empty() && !definitions.empty() ? "installed" : "failed";
      job->reason = error;
      job->detail = job->state == "failed" ? detail : std::string();
      publishLocked();
    }
  }
}
void GraphicsTemplateRegistry::loadPacks(const std::string& dir) {
  namespace fs = std::filesystem;
  const auto fromUtf8 = [](const std::string& utf8) { return fs::path(std::u8string(reinterpret_cast<const char8_t*>(utf8.data()), utf8.size())); };
  const auto toUtf8 = [](const fs::path& path) { const auto value = path.u8string(); return std::string(reinterpret_cast<const char*>(value.data()), value.size()); };
  std::vector<std::string> folders;
  std::error_code ec;
  for (fs::directory_iterator it(fromUtf8(dir), ec), end; !ec && it != end; it.increment(ec))
    if (it->is_directory(ec) && fs::exists(it->path() / "pack.json", ec)) folders.push_back(toUtf8(it->path()));
  std::sort(folders.begin(), folders.end());
  for (const auto& folder : folders) {
    GraphicsPackManifest pack;
    std::vector<std::string> manifests;
    std::string error, detail;
    // The same lint and loader an operator pack install runs (off every lock).
    if (!graphicsPackInstallManifests(folder, manifests, detail) || !readGraphicsPackManifest(folder, pack, detail)) error = "graphics-pack-invalid";
    std::vector<std::shared_ptr<const GraphicsTemplate>> definitions;
    std::map<std::string, std::shared_ptr<const std::vector<uint8_t>>> newAssets;
    uint64_t addedBytes = 0;
    for (const auto& manifest : manifests) {
      if (!error.empty()) break;
      // prepare() reads and may trim the asset pool, which is otherwise only
      // touched on this worker, so no lock is needed; same as run().
      auto definition = prepare(manifest, pack.root, newAssets, addedBytes, error);
      if (!definition) { detail = "template " + std::to_string(definitions.size() + 1) + ": " + error; definitions.clear(); break; }
      definitions.push_back(std::move(definition));
    }
    std::lock_guard lock(mutex_);
    if (stopping_) return;
    for (const auto& definition : definitions) {
      if (!error.empty()) break;
      const auto key = std::make_pair(definition->id(), definition->version());
      const auto found = definitions_.find(key);
      if (key.first == "compact-solid" || key.first == "minimal-accent" || key.first == "broadcast" || builtIn(key.first, key.second)) error = "template-id-reserved";
      else if (found != definitions_.end() && found->second->hash() != definition->hash()) error = "template-version-immutable";
      if (!error.empty()) detail = key.first + ": " + error;
    }
    Json::Array ids;
    const Json packRef = Json::Object{{"id", pack.id}, {"name", pack.name}, {"genre", pack.genre}};
    if (error.empty()) {
      for (const auto& definition : definitions) {
        const auto key = std::make_pair(definition->id(), definition->version());
        // An identical operator import of the same version becomes the built-in one.
        if (const auto found = definitions_.find(key); found != definitions_.end() && !packKey(key))
          documentBytes_ -= found->second->document().stringify().size();
        definitions_[key] = definition;
        origins_[key] = pack.root;
        packKeys_[key] = packRef;
        ids.emplace_back(key.first);
        if (thumbnailsRequested_) { thumbnails_.erase(key); thumbnailsPending_ = true; }
      }
      for (auto& asset : newAssets) assetPool_.emplace(std::move(asset));
      assetBytes_ += addedBytes;
    }
    const auto id = pack.id.empty() ? toUtf8(fromUtf8(folder).filename()) : pack.id;
    Json::Object status{{"id", id}, {"name", pack.name}, {"genre", pack.genre}, {"version", pack.version},
        {"state", error.empty() ? "ready" : "invalid"}, {"templates", std::move(ids)}};
    if (!error.empty()) status["detail"] = detail.empty() ? error : detail;
    packs_.erase(std::remove_if(packs_.begin(), packs_.end(), [&](const Json& row) { return row.getString("id") == id; }), packs_.end());
    packs_.emplace_back(std::move(status));
    publishLocked();
    if (thumbnailsPending_) wake_.notify_one();
  }
}
} // namespace corevideo::graphics
