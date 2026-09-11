#include "core/AuthorityShadow.h"
#include <stdexcept>
#include <algorithm>
#include <utility>

namespace corevideo::core {
namespace {
struct Budget {
  size_t used{0}, limit;
  void add(size_t n) { if (used > limit || n > limit - used) used = limit + 1; else used += n; }
  void string(const std::string& s) { add(s.capacity() + 1); }
  void ref(const ShowEntityRef& r) { string(r.id); }
  void ref(const ShowSourceRef& r) { string(r.sourceId); string(r.instanceId); string(r.processEpoch); }
  void target(const ShowRouteTarget& t) { if (t.source) ref(*t.source); if (t.person) ref(*t.person); }
  template<class T> void vector(const std::vector<T>& v) {
    if (v.capacity() > limit / sizeof(T)) used = limit + 1; else add(v.capacity() * sizeof(T));
  }
};
}
size_t AuthorityShadow::retainedBytes(const Record& r, size_t limit) {
  Budget b{sizeof(Envelope), limit};
  b.string(r.basis.nativeProcessEpoch); b.string(r.basis.legacyAuthorityEpoch);
  b.string(r.desired.authorityEpoch); b.string(r.sources.processEpoch);
  b.vector(r.sources.people); b.vector(r.sources.sources);
  if (r.exactFrames) {
    b.add(sizeof(ShadowExactSourceFrames::Checkpoint));
    b.string(r.exactFrames->processEpoch); b.vector(r.exactFrames->sources);
    if (b.used > limit) return b.used;
    for (const auto& row : r.exactFrames->sources) {
      const auto& id = row.source.identity;
      b.string(id.sourceId); b.string(id.instanceId); b.string(id.processEpoch); b.string(id.kind);
      if (b.used > limit) return b.used;
    }
  }
  if (b.used > limit) return b.used;
  for (const auto& p : r.sources.people) { b.string(p.id); b.string(p.name); if (b.used > limit) return b.used; }
  for (const auto& s : r.sources.sources) {
    b.string(s.id); b.string(s.instanceId); b.string(s.externalId); b.string(s.personId); b.string(s.name);
    if (s.publication) b.string(s.publication->pixelFormat);
    if (b.used > limit) return b.used;
  }
  const auto rows = [&](const auto& optional, auto visit) {
    if (!optional || b.used > limit) return;
    b.vector(*optional); if (b.used > limit) return;
    for (const auto& row : *optional) { b.string(row.id); visit(row); if (b.used > limit) return; }
  };
  rows(r.desired.inputs,[&](const auto& row) { b.string(row.intent.label); b.target(row.intent.target); });
  rows(r.desired.isoSelections,[&](const auto& row) { b.target(row.intent.target); });
  rows(r.desired.audioRoutes,[&](const auto& row) { b.target(row.intent.source); b.ref(row.intent.destination); });
  rows(r.desired.outputs,[&](const auto&) {});
  rows(r.desired.overlays,[&](const auto& row) { b.string(row.intent.content); if(row.intent.source) b.ref(*row.intent.source); });
  rows(r.desired.scenes,[&](const auto& scene) {
    b.string(scene.label); b.vector(scene.layers); if (b.used > limit) return;
    for (const auto& row : scene.layers) { b.string(row.id); b.target(row.intent.target); if (b.used > limit) return; }
  });
  rows(r.desired.tiles,[&](const auto& tiles) {
    b.vector(tiles.includedInputs); b.vector(tiles.excludedInputs); b.vector(tiles.reservedSlots);
    if (b.used > limit) return;
    for (const auto& ref : tiles.includedInputs) { b.ref(ref); if (b.used > limit) return; }
    for (const auto& ref : tiles.excludedInputs) { b.ref(ref); if (b.used > limit) return; }
    for (const auto& slot : tiles.reservedSlots) { if (slot.input) b.ref(*slot.input); if (b.used > limit) return; }
  });
  if (r.desired.preview.scene) b.ref(*r.desired.preview.scene);
  if (r.desired.program.scene) b.ref(*r.desired.program.scene);
  if (r.sceneVersions) {
    const auto ref = [&](const SceneVersionRef& value) { b.string(value.authorityEpoch); b.string(value.sceneId); };
    if (r.sceneVersions->program) ref(*r.sceneVersions->program);
    if (r.sceneVersions->preview) ref(*r.sceneVersions->preview);
    b.vector(r.sceneVersions->definitions);
    if (b.used > limit) return b.used;
    for (const auto& definition : r.sceneVersions->definitions) {
      ref(definition.reference); b.string(definition.scene.label);
      b.vector(definition.scene.layerOrder);
      if (b.used > limit) return b.used;
      for (const auto& id : definition.scene.layerOrder) {
        b.string(id); if (b.used > limit) return b.used;
      }
      for (const auto& [id, layer] : definition.scene.routes) {
        b.add(sizeof(layer) + sizeof(std::string) + 4 * sizeof(void*)); b.string(id); b.target(layer.target);
        if (b.used > limit) return b.used;
      }
      if (b.used > limit) return b.used;
    }
  }
  return b.used;
}
bool AuthorityShadow::hasPrivateMetadata(const Record& r) {
  for (const auto& p : r.sources.people) if (!p.name.empty()) return true;
  for (const auto& s : r.sources.sources) if (!s.name.empty()) return true;
  return false;
}
AuthorityShadow::AuthorityShadow(Config config) : config_(std::move(config)) {
  if (!config_.enabled) return;
  if (config_.nativeProcessEpoch.empty() || config_.legacyAuthorityEpoch.empty() ||
      config_.authorityEpoch.empty() || config_.registryEpoch.empty() ||
      config_.nativeProcessEpoch.size() > 512 || config_.legacyAuthorityEpoch.size() > 512 ||
      config_.authorityEpoch.size() > 512 || config_.registryEpoch.size() > 512 ||
      config_.videoFreshnessWindowNs <= 0 || config_.videoFreshnessWindowNs > 60'000'000'000LL ||
      !config_.maxRecords || config_.maxRecords > 4096 || !config_.maxBytes || config_.maxBytes > 64 * 1024 * 1024 ||
      !config_.maxEntities || config_.maxEntities > 65536) throw std::invalid_argument("Invalid shadow configuration");
  sources_ = std::make_unique<ZoomSourceAuthorityAdapter>(config_.registryEpoch, config_.maxEntities, config_.maxEntities, config_.maxEntities);
  show_ = std::make_unique<ShowStateOwner>(config_.authorityEpoch, config_.maxEntities);
  if (config_.versionedScenes) sceneVersions_ = std::make_unique<SceneVersionStore>(config_.authorityEpoch, config_.sceneVersionLimits);
  queue_.resize(config_.maxRecords); status_ = Status::AwaitingCheckpoint;
  if (config_.workerThread) worker_ = std::thread([this] { run(); });
}
AuthorityShadow::~AuthorityShadow() { stop(); }
void AuthorityShadow::lose() { ++lossEpoch_; }
AuthorityShadow::Admission AuthorityShadow::submit(Record record) {
  if (!config_.enabled) return Admission::Disabled;
  if (stopped_ || error_) return Admission::Stopped;
  if (config_.exactFrameComparison != bool(record.exactFrames) ||
      config_.versionedScenes != record.sceneVersions.has_value()) {
    ++invalid_; ++drops_; lose(); return Admission::ModeMismatch;
  }
  const auto size = retainedBytes(record, config_.maxBytes);
  if (size > config_.maxBytes) { ++oversized_; ++drops_; lose(); return Admission::Oversized; }
  if (hasPrivateMetadata(record)) { ++private_; ++drops_; lose(); return Admission::PrivateMetadata; }
  std::unique_lock lock(queueMutex_, std::try_to_lock);
  if (!lock.owns_lock()) { ++drops_; lose(); return Admission::Contended; }
  if (stopped_ || error_) return Admission::Stopped;
  if (depth_ == queue_.size() || size > config_.maxBytes - bytes_) { ++drops_; lose(); return Admission::Full; }
  queue_[tail_].emplace(Envelope{std::move(record), size, lossEpoch_.load()});
  tail_ = (tail_ + 1) % queue_.size(); ++depth_; bytes_ += size;
  maximumDepth_ = std::max(maximumDepth_, depth_); ++captured_;
  queueDepthGauge_ = depth_; queueBytesGauge_ = bytes_; maxDepthGauge_ = maximumDepth_;
  lock.unlock(); wake_.notify_one(); return Admission::Accepted;
}
bool AuthorityShadow::consumeOne() {
  std::lock_guard processing(processingMutex_);
  std::optional<Envelope> record;
  {
    std::lock_guard lock(queueMutex_);
    if (!depth_ || stopped_ || error_) return false;
    record = std::move(queue_[head_]); queue_[head_].reset();
    head_ = (head_ + 1) % queue_.size(); --depth_; bytes_ -= record->bytes;
    queueDepthGauge_ = depth_; queueBytesGauge_ = bytes_;
  }
  try { process(std::move(*record)); }
  catch (...) { ++exceptions_; lose(); error_ = true; status_ = Status::DisabledError; }
  return true;
}
bool AuthorityShadow::processOne() {
  if (!config_.enabled || config_.workerThread) return false;
  return consumeOne();
}
void AuthorityShadow::process(Envelope envelope) {
  ++processed_;
  const auto& r = envelope.record;
  const auto& b = r.basis;
  const auto gap = [&] { ++gaps_; lose(); status_ = Status::BasisGap; };
  if (envelope.lossEpoch != lossEpoch_.load()) { ++gaps_; status_ = Status::BasisGap; return; }
  if (b.nativeProcessEpoch != config_.nativeProcessEpoch ||
      b.legacyAuthorityEpoch != config_.legacyAuthorityEpoch ||
      r.desired.authorityEpoch != b.legacyAuthorityEpoch ||
      !b.captureSequence || b.captureSequence <= lastSequence_ ||
      b.captureSequence > 9007199254740991ULL || b.legacyRevision > 9007199254740991ULL ||
      !b.clockGeneration || b.clockGeneration > 9007199254740991ULL || b.capturedAtNs < 0 ||
      b.sourceSequence != r.sources.sequence ||
      r.desired.expectedRevision != b.legacyRevision) { gap(); return; }
  if (const auto previous = std::atomic_load_explicit(&published_, std::memory_order_acquire); previous &&
      (b.legacyRevision < previous->evidence.basis.legacyRevision ||
       b.clockGeneration < previous->evidence.basis.clockGeneration ||
       b.capturedAtNs < previous->evidence.basis.capturedAtNs)) { gap(); return; }
  const bool complete = r.completeCheckpoint && r.desired.inputs && r.desired.scenes && r.desired.tiles &&
      r.desired.overlays && r.desired.isoSelections && r.desired.audioRoutes && r.desired.outputs &&
      r.desired.preview.supplied && r.desired.program.supplied;
  if (!complete) { gap(); return; }
  // All admitted records are complete checkpoints. Missing sequence numbers are
  // observable gaps, recovered by this coherent checkpoint, never replayed deltas.
  if (lastSequence_ && b.captureSequence != lastSequence_ + 1) ++gaps_;
  // Every owner below is private staging state. A rejection must not consume
  // source incarnations, entity tombstones, or scene versions in the next retry.
  auto stagedShow = std::make_unique<ShowStateOwner>(*show_);
  auto stagedSources = std::make_unique<ZoomSourceAuthorityAdapter>(*sources_);
  auto stagedVersions = sceneVersions_ ? std::make_unique<SceneVersionStore>(*sceneVersions_) : nullptr;
  const auto before = stagedShow->snapshot();
  auto command = r.desired;
  // Legacy control revisions identify the capture basis. The shadow maintains its
  // own semantic revision and must not pretend both counters advance identically.
  command.authorityEpoch = config_.authorityEpoch; command.expectedRevision = before->revision;
  const auto projection = LegacyShowCommandProjector::project(*before, command, config_.maxEntities, config_.maxBytes);
  if (!projection.candidate) { ++invalid_; lose(); status_ = Status::AdapterInvalid; return; }
  const auto sourceResult = stagedSources->sync(r.sources);
  if (sourceResult.status != ZoomSourceAuthorityAdapter::Status::Applied && sourceResult.status != ZoomSourceAuthorityAdapter::Status::Unchanged) {
    ++invalid_; lose(); status_ = Status::AdapterInvalid; return;
  }
  const auto showResult = stagedShow->replace(config_.authorityEpoch, before->revision, *projection.candidate);
  if (showResult.status != ShowStateUpdateStatus::Changed && showResult.status != ShowStateUpdateStatus::Unchanged) {
    ++invalid_; lose(); status_ = Status::AdapterInvalid; return;
  }
  auto publication = std::make_shared<Published>(); publication->lossEpoch = envelope.lossEpoch;
  const auto cutoff = b.capturedAtNs > config_.videoFreshnessWindowNs
      ? b.capturedAtNs - config_.videoFreshnessWindowNs : 0;
  if (r.sceneVersions) {
    const auto reject = [&] { ++invalid_; lose(); status_ = Status::AdapterInvalid; };
    const auto& versions = *r.sceneVersions;
    if (versions.definitions.size() > 2) { reject(); return; }
    std::map<SceneVersionRef, const ShowSceneIntent*> definitions;
    for (const auto& definition : versions.definitions)
      if (!definitions.emplace(definition.reference, &definition.scene).second) { reject(); return; }
    const auto validBus = [&](const std::optional<SceneVersionRef>& ref, const LegacyBusDto& bus) {
      if (!ref) return !bus.scene;
      const auto found = definitions.find(*ref);
      return ref->authorityEpoch == config_.authorityEpoch && bus.scene && ref->sceneId == bus.scene->id &&
          found != definitions.end() && found->second->generation == bus.scene->generation;
    };
    if (!validBus(versions.program, r.desired.program) || !validBus(versions.preview, r.desired.preview)) { reject(); return; }
    std::vector<SceneVersionStore::Publication> batch;
    std::map<std::string, std::optional<SceneVersionRef>> stagedHeads;
    for (const auto& [ref, scene] : definitions) {
      if ((!versions.program || *versions.program != ref) && (!versions.preview || *versions.preview != ref)) { reject(); return; }
      auto [head, inserted] = stagedHeads.try_emplace(ref.sceneId);
      if (inserted) {
        const auto current = stagedVersions->head(ref.sceneId);
        if (current.lease) head->second = current.lease->ref;
      }
      batch.push_back({ref, *scene, head->second});
      if (!head->second || ref.version > head->second->version) head->second = ref;
    }
    const auto admitted = stagedVersions->publishBatch(std::move(batch));
    if (admitted.status != SceneVersionStore::Status::Applied && admitted.status != SceneVersionStore::Status::Unchanged) { reject(); return; }
    auto evidence = SceneVersionShadowEvidence::capture(*stagedVersions, *showResult.snapshot, *sourceResult.snapshot,
        {cutoff}, versions.program, versions.preview);
    if (!evidence) { reject(); return; }
    publication->evidence = {b, evidence->plans, std::move(evidence)};
  } else publication->evidence = {b, generateShowPlans(*showResult.snapshot, *sourceResult.snapshot, {cutoff}), {}};
  auto& comparison = publication->evidence.exactFrames;
  if (config_.exactFrameComparison) {
    comparison.state = ExactFrameComparison::State::Unavailable;
    const auto& frames = r.exactFrames;
    bool coherent = frames && frames->valid && frames->processEpoch == r.sources.processEpoch &&
        frames->sequence == b.sourceSequence && frames->sources.size() == r.sources.sources.size() &&
        frames->sources.size() <= config_.maxEntities;
    std::map<std::string, const ShadowExactSourceFrames::Observation*> rows;
    if (coherent) {
      for (const auto& row : frames->sources) rows.emplace(row.source.identity.sourceId, &row);
      for (const auto& source : r.sources.sources) {
        const auto found = rows.find(source.id);
        if (found == rows.end()) { coherent = false; break; }
        const auto& row = *found->second;
        const auto kind = source.kind == SourceRegistry::Kind::ParticipantVideo ? "camera" :
            source.kind == SourceRegistry::Kind::ParticipantShare ? "share" : "";
        const auto& id = row.source.identity;
        if (id.instanceId != source.instanceId || id.generation != source.incarnation || id.kind != kind ||
            row.source.available != source.videoAvailable ||
            (row.publicationSequence && (!source.publication || *row.publicationSequence > source.publication->sequence ||
             !row.observedNs || *row.observedNs > b.capturedAtNs))) { coherent = false; break; }
      }
    }
    if (coherent) {
      comparison.state = ExactFrameComparison::State::Complete;
      const auto evaluate = [&](const PlannedBinding& binding) {
        if (!binding.source) {
          // A retired fixed identity remains an explicit failed comparison.
          if (binding.intent.kind == ShowRouteKind::FixedSource && binding.intent.source) {
            ++comparison.evaluated; ++comparison.missing;
          }
          return;
        }
        ++comparison.evaluated;
        const auto found = rows.find(binding.source->sourceId);
        if (found == rows.end()) { ++comparison.missing; return; }
        const auto& row = *found->second; const auto& id = row.source.identity;
        if (id.instanceId != binding.source->instanceId || id.processEpoch != binding.source->processEpoch ||
            id.generation != binding.source->generation || !row.source.available || !row.publicationSequence ||
            !row.observedNs || (binding.sourceKind &&
              ((id.kind == "camera" && *binding.sourceKind != SourceRegistry::Kind::ParticipantVideo) ||
               (id.kind == "share" && *binding.sourceKind != SourceRegistry::Kind::ParticipantShare)))) { ++comparison.missing; return; }
        if (*row.observedNs < cutoff) { ++comparison.expired; return; }
        ++comparison.eligible;
      };
      const auto& plans = *publication->evidence.plans;
      for (const auto& input : plans.render.inputs) evaluate(input.binding);
      for (const auto* scene : {&plans.render.program, &plans.render.preview})
        for (const auto& layer : scene->layers) evaluate(layer.binding);
      for (const auto& tiles : plans.render.tiles) for (const auto& slot : tiles.slots) evaluate(slot.binding);
      for (const auto& overlay : plans.render.overlays) evaluate(overlay.binding);
      for (const auto& iso : plans.output.isoSelections) evaluate(iso.videoBinding);
    }
  }
  std::shared_ptr<const Published> frozen = std::move(publication);
  show_.swap(stagedShow);
  sources_.swap(stagedSources);
  sceneVersions_.swap(stagedVersions);
  lastSequence_ = b.captureSequence;
  std::atomic_store_explicit(&published_, std::move(frozen), std::memory_order_release);
  status_ = Status::Ready;
}
void AuthorityShadow::run() {
  while (!stopped_ && !error_) {
    { std::unique_lock lock(queueMutex_); wake_.wait(lock, [&] { return stopped_ || error_ || depth_ != 0; }); }
    if (!consumeOne() && (stopped_ || error_)) break;
  }
}
void AuthorityShadow::stop() {
  std::lock_guard stopLock(stopMutex_);
  stopped_ = true; wake_.notify_all();
  if (worker_.joinable()) worker_.join();
  std::lock_guard processing(processingMutex_);
  std::vector<std::optional<Envelope>> retired;
  { std::lock_guard lock(queueMutex_); retired.swap(queue_); depth_ = bytes_ = 0; queueDepthGauge_ = queueBytesGauge_ = 0; }
  if (config_.enabled && !error_) status_ = Status::Stopped;
}
std::shared_ptr<const AuthorityShadow::Evidence> AuthorityShadow::latest() const {
  const auto publication = std::atomic_load_explicit(&published_, std::memory_order_acquire);
  if (!publication || stopped_ || error_ || publication->lossEpoch != lossEpoch_.load()) return {};
  return std::shared_ptr<const Evidence>(publication, &publication->evidence);
}
AuthorityShadow::Diagnostics AuthorityShadow::diagnostics() const {
  Diagnostics d; d.enabled = config_.enabled; d.status = status_; d.continuous = bool(latest());
  if (!d.continuous && d.status == Status::Ready) d.status = Status::BasisGap;
  d.captured = captured_; d.processed = processed_; d.queueDrops = drops_; d.oversized = oversized_;
  d.privateMetadata = private_; d.basisGaps = gaps_; d.adapterInvalid = invalid_; d.exceptions = exceptions_;
  d.lastProcessedSequence = lastSequence_;
  if (const auto p = latest()) { d.controlRevision = p->plans->render.stamp.controlRevision; d.registryRevision = p->plans->render.stamp.registryRevision; }
  d.queueDepth = queueDepthGauge_; d.queueBytes = queueBytesGauge_; d.maximumQueueDepth = maxDepthGauge_;
  return d;
}
} // namespace corevideo::core
