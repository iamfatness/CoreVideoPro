#include "graphics/GraphicsTemplateRegistry.h"
#include <atomic>

namespace corevideo::graphics {
using rpc::Json;
GraphicsTemplateRegistry::GraphicsTemplateRegistry() {
  auto builtin = GraphicsTemplate::corporateLowerThird();
  definitions_[{builtin->id(), builtin->version()}] = builtin;
  documentBytes_ = builtin->document().stringify().size();
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
std::string GraphicsTemplateRegistry::submit(const std::string& client, const std::string& request, const std::string& manifest) {
  if (manifest.empty() || manifest.size() > 256 * 1024) return "template-manifest-size";
  // Never wait for validation/catalog publication. Contention is retryable.
  std::unique_lock lock(mutex_, std::try_to_lock);
  if (!lock.owns_lock()) return "template-import-busy";
  for (const auto& old : history_) if (old->client == client && old->request == request)
    return old->manifest == manifest ? "template-install-queued" : "request-id-reused";
  size_t pending = 0;
  for (const auto& old : history_) if (old->state == "queued" || old->state == "validating") ++pending;
  if (pending >= 2) return "template-import-busy";
  // No work/status construction under the deadline: wake the owner to publish.
  auto job = std::make_shared<Job>(Job{client, request, manifest, "queued", ""});
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
    std::string hash; static constexpr char hex[] = "0123456789abcdef";
    for (auto byte : definition->hash()) { hash += hex[byte >> 4]; hash += hex[byte & 15]; }
    row["manifestHash"] = std::move(hash); catalog.emplace_back(std::move(row));
  }
  for (const auto& job : history_) installs.emplace_back(Json::Object{
    {"clientId", job->client}, {"requestId", job->request}, {"state", job->state}, {"reason", job->reason}});
  publication->definitions = definitions_; publication->catalog = std::move(catalog); publication->installs = std::move(installs);
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
        } else if (definitions_.size() >= 16 || documentBytes_ + bytes > 256 * 1024) error = "template-library-capacity";
        else { definitions_[key] = definition; documentBytes_ += bytes; }
      }
      job->state = error.empty() && definition ? "installed" : "failed";
      job->reason = error;
      publishLocked();
    }
  }
}
} // namespace corevideo::graphics
