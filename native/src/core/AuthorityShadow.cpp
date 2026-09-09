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
  queue_.resize(config_.maxRecords); status_ = Status::AwaitingCheckpoint;
  if (config_.workerThread) worker_ = std::thread([this] { run(); });
}
AuthorityShadow::~AuthorityShadow() { stop(); }
void AuthorityShadow::lose() { ++lossEpoch_; }
AuthorityShadow::Admission AuthorityShadow::submit(Record record) {
  if (!config_.enabled) return Admission::Disabled;
  if (stopped_ || error_) return Admission::Stopped;
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
  lastSequence_ = b.captureSequence;
  const auto before = show_->snapshot();
  auto command = r.desired;
  // Legacy control revisions identify the capture basis. The shadow maintains its
  // own semantic revision and must not pretend both counters advance identically.
  command.authorityEpoch = config_.authorityEpoch; command.expectedRevision = before->revision;
  const auto projection = LegacyShowCommandProjector::project(*before, command, config_.maxEntities, config_.maxBytes);
  if (!projection.candidate) { ++invalid_; lose(); status_ = Status::AdapterInvalid; return; }
  const auto sourceResult = sources_->sync(r.sources);
  if (sourceResult.status != ZoomSourceAuthorityAdapter::Status::Applied && sourceResult.status != ZoomSourceAuthorityAdapter::Status::Unchanged) {
    ++invalid_; lose(); status_ = Status::AdapterInvalid; return;
  }
  const auto showResult = show_->replace(config_.authorityEpoch, before->revision, *projection.candidate);
  if (showResult.status != ShowStateUpdateStatus::Changed && showResult.status != ShowStateUpdateStatus::Unchanged) {
    ++invalid_; lose(); status_ = Status::AdapterInvalid; return;
  }
  auto publication = std::make_shared<Published>(); publication->lossEpoch = envelope.lossEpoch;
  const auto cutoff = b.capturedAtNs > config_.videoFreshnessWindowNs
      ? b.capturedAtNs - config_.videoFreshnessWindowNs : 0;
  publication->evidence = {b, generateShowPlans(*showResult.snapshot, *sourceResult.snapshot, {cutoff})};
  std::shared_ptr<const Published> frozen = std::move(publication);
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
