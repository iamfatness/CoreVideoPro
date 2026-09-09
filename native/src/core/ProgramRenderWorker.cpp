#include "core/ProgramRenderWorker.h"
#include <condition_variable>
#include <stdexcept>

namespace corevideo::core {
namespace {
constexpr uint64_t safe = 9007199254740991ULL;
bool valid(const ProgramRenderWorker::Generation& g) {
  return !g.authorityEpoch.empty() && g.authorityEpoch.size() <= 512 && !g.clockId.empty() &&
      g.clockId.size() <= 512 && g.renderer > 0 && g.renderer <= safe && g.clock > 0 && g.clock <= safe;
}
int64_t nowNs() { return std::chrono::duration_cast<std::chrono::nanoseconds>(
    std::chrono::steady_clock::now().time_since_epoch()).count(); }
ShowPlans freeze(const ShowPlans& p) {
  // Bound copied metadata and retained immutable scene payloads before allocating
  // the work copy. Queue count alone is not a memory bound.
  constexpr size_t maximumBytes = 16 * 1024 * 1024;
  size_t bytes = sizeof(ShowPlans);
  const auto add = [&](size_t n) {
    if (n > maximumBytes - bytes) throw std::invalid_argument("Render work exceeds metadata budget");
    bytes += n;
  };
  const auto text = [&](const std::string& s) { add(s.capacity()); };
  const auto target = [&](const ShowRouteTarget& t) {
    if (t.source) { text(t.source->sourceId); text(t.source->instanceId); text(t.source->processEpoch); }
    if (t.person) text(t.person->id);
  };
  const auto binding = [&](const PlannedBinding& b) {
    target(b.intent);
    if (b.source) { text(b.source->sourceId); text(b.source->instanceId); text(b.source->processEpoch); }
  };
  const auto rows = [&](const auto& list) {
    size_t count = list.size();
    if constexpr (requires { list.capacity(); }) count = list.capacity();
    if (count > 4096) throw std::invalid_argument("Too many render work items");
    add(count * (sizeof(typename std::decay_t<decltype(list)>::value_type) + 64));
  };
  for (const auto* stamp : {&p.render.stamp, &p.audio.stamp, &p.output.stamp}) {
    text(stamp->authorityEpoch); text(stamp->registryEpoch); text(stamp->eligibilityIdentity);
  }
  rows(p.render.inputs); for (const auto& input : p.render.inputs) { text(input.input.id); binding(input.binding); }
  for (const auto* scene : {&p.render.program, &p.render.preview}) {
    if (scene->scene) text(scene->scene->id);
    if (scene->version) { text(scene->version->sceneId); text(scene->version->authorityEpoch); }
    rows(scene->layers);
    for (const auto& layer : scene->layers) { text(layer.route.id); target(layer.intent.target); binding(layer.binding); }
    if (scene->versionLease) {
      const auto& held = *scene->versionLease;
      text(held.ref.sceneId); text(held.ref.authorityEpoch); text(held.scene.label); rows(held.scene.routes); rows(held.scene.layerOrder);
      for (const auto& id : held.scene.layerOrder) text(id);
      for (const auto& [id, layer] : held.scene.routes) { text(id); target(layer.target); }
    }
  }
  rows(p.render.tiles); for (const auto& tile : p.render.tiles) {
    text(tile.tiles.id); rows(tile.slots);
    for (const auto& slot : tile.slots) { if (slot.input) text(slot.input->id); binding(slot.binding); }
  }
  rows(p.render.overlays); for (const auto& overlay : p.render.overlays) {
    text(overlay.overlay.id); text(overlay.intent.content); binding(overlay.binding);
    if (overlay.intent.source) { text(overlay.intent.source->sourceId); text(overlay.intent.source->instanceId); text(overlay.intent.source->processEpoch); }
  }
  rows(p.audio.routes); for (const auto& route : p.audio.routes) {
    text(route.route.id); target(route.intent.source); text(route.intent.destination.id); binding(route.binding);
  }
  rows(p.output.outputs); for (const auto& output : p.output.outputs) text(output.output.id);
  rows(p.output.isoSelections); for (const auto& iso : p.output.isoSelections) { text(iso.selection.id); binding(iso.videoBinding); binding(iso.audioBinding); }
  return p;
}
}
ProgramRenderWorker::Work::Work(Generation g, uint64_t r, int64_t s, int64_t deadline, const ShowPlans& p)
    : generation(std::move(g)), revision(r), slot(s), deadlineNs(deadline), plans(freeze(p)) {
  if (!valid(generation) || revision > safe || slot < 0 || static_cast<uint64_t>(slot) > safe || deadlineNs < 0 ||
      plans.render.stamp.authorityEpoch != generation.authorityEpoch || plans.render.stamp.controlRevision != revision ||
      plans.render.stamp != plans.audio.stamp || plans.render.stamp != plans.output.stamp)
    throw std::invalid_argument("Invalid render work basis");
}
struct ProgramRenderWorker::State {
  explicit State(Config c) : config(std::move(c)), generation(config.generation), queue(config.capacity) {}
  const Config config;
  mutable std::mutex mutex;
  std::condition_variable wake, finished;
  Generation generation;
  std::vector<std::shared_ptr<const Work>> queue;
  size_t head{0}, depth{0};
  int64_t lastSlot{-1}; uint64_t revision{0};
  std::atomic<bool> cancelled{false}, busy{false}, complete{false}, stallReported{false};
  std::atomic<int64_t> startedNs{0};
  std::atomic<uint64_t> accepted{0}, coalesced{0}, rendered{0}, failed{0}, stalled{0}, skipped{0},
      contended{0}, rejectedStale{0}, staleCompletions{0}, deadlineMisses{0};
  std::atomic<size_t> queued{0}, maximumQueued{0};
  // Real atomic, not the deprecated shared_ptr free functions: those make
  // correctness a convention every future editor must remember, and a plain
  // assignment to this member would reintroduce a data race with no warning.
  std::atomic<std::shared_ptr<const Work>> latest;
  void reportStall() {
    if (busy && nowNs() - startedNs.load() > std::chrono::duration_cast<std::chrono::nanoseconds>(config.stallThreshold).count() &&
        !stallReported.exchange(true)) ++stalled;
  }
};
ProgramRenderWorker::ProgramRenderWorker(Config config, Factory factory) {
  if ((config.enabled && !valid(config.generation)) || !config.capacity || config.capacity > 8 ||
      config.shutdownBudget.count() < 0 || config.shutdownBudget > std::chrono::seconds(1) ||
      config.stallThreshold.count() <= 0 || config.stallThreshold > std::chrono::seconds(60) || (config.enabled && !factory))
    throw std::invalid_argument("Invalid render worker configuration");
  state_ = std::make_shared<State>(std::move(config));
  if (state_->config.enabled) thread_ = std::thread(&ProgramRenderWorker::run, state_, std::move(factory));
  else state_->complete = true;
}
ProgramRenderWorker::~ProgramRenderWorker() {
  auto s = state_;
  s->cancelled = true;
  s->wake.notify_all();
  // Blocking, unlike shutdown(). A concurrent shutdown() caller may still be
  // inside the critical section below, using thread_ and holding joinMutex_,
  // and destroying either out from under it is undefined behavior. shutdown()
  // may skip a busy peer because the object outlives it; the destructor is the
  // one caller that may not.
  std::unique_lock<std::mutex> joinLock(joinMutex_);
  retireJoined(std::move(s));
}
ProgramRenderWorker::Admission ProgramRenderWorker::submit(std::shared_ptr<const Work> work) {
  auto& s = *state_;
  if (!s.config.enabled) return Admission::Disabled;
  if (!work) return Admission::Invalid;
  if (s.cancelled) return Admission::Stopped;
  std::shared_ptr<const Work> retired;
  std::unique_lock lock(s.mutex, std::try_to_lock);
  if (!lock.owns_lock()) { ++s.contended; return Admission::Contended; }
  if (s.cancelled) return Admission::Stopped;
  if (work->generation != s.generation || work->revision < s.revision || work->slot <= s.lastSlot) {
    ++s.rejectedStale; return Admission::Stale;
  }
  if (s.lastSlot >= 0 && work->slot > s.lastSlot + 1) s.skipped += work->slot - s.lastSlot - 1;
  s.lastSlot = work->slot; s.revision = work->revision;
  bool replaced = s.depth == s.queue.size();
  if (replaced) {
    retired = std::move(s.queue[s.head]); s.head = (s.head + 1) % s.queue.size(); --s.depth;
    ++s.coalesced; ++s.skipped;
  }
  s.queue[(s.head + s.depth) % s.queue.size()] = std::move(work); ++s.depth;
  s.queued = s.depth; if (s.depth > s.maximumQueued) s.maximumQueued = s.depth;
  ++s.accepted;
  lock.unlock(); s.wake.notify_one();
  return replaced ? Admission::Coalesced : Admission::Accepted;
}
ProgramRenderWorker::Admission ProgramRenderWorker::reset(const Generation& expected, Generation next) {
  auto& s = *state_;
  if (!s.config.enabled) return Admission::Disabled;
  if (!valid(next) || next.renderer <= expected.renderer ||
      (next.clockId == expected.clockId && next.clock < expected.clock)) return Admission::Invalid;
  std::vector<std::shared_ptr<const Work>> retired(s.config.capacity);
  std::shared_ptr<const Work> oldLatest;
  std::unique_lock lock(s.mutex, std::try_to_lock);
  if (!lock.owns_lock()) { ++s.contended; return Admission::Contended; }
  if (s.cancelled) return Admission::Stopped;
  if (s.generation != expected) return Admission::Stale;
  if (s.generation.authorityEpoch != next.authorityEpoch) s.revision = 0;
  s.generation = std::move(next); s.lastSlot = -1; s.skipped += s.depth;
  retired.swap(s.queue); s.head = s.depth = 0; s.queued = 0;
  oldLatest = s.latest.exchange({});
  lock.unlock(); s.wake.notify_one(); return Admission::Accepted;
}
void ProgramRenderWorker::run(std::shared_ptr<State> state, Factory factory) {
  auto& s = *state;
  std::unique_ptr<Renderer> renderer;
  std::optional<Generation> contextGeneration;
  for (;;) {
    std::shared_ptr<const Work> work;
    {
      std::unique_lock lock(s.mutex);
      s.wake.wait(lock, [&] { return s.cancelled || s.depth; });
      if (s.cancelled) break;
      work = std::move(s.queue[s.head]); s.head = (s.head + 1) % s.queue.size(); --s.depth; s.queued = s.depth;
    }
    s.startedNs = nowNs(); s.stallReported = false; s.busy = true;
    bool success = false;
    try {
      if (!contextGeneration || *contextGeneration != work->generation) {
        renderer.reset(); contextGeneration.reset();
        renderer = factory(work->generation, s.cancelled);
        if (renderer) contextGeneration = work->generation;
      }
      if (renderer && !s.cancelled) success = renderer->render(*work, s.cancelled);
    } catch (...) { renderer.reset(); contextGeneration.reset(); }
    s.reportStall(); s.busy = false;
    std::shared_ptr<const Work> retired;
    {
      std::lock_guard lock(s.mutex);
      if (s.cancelled || work->generation != s.generation) { ++s.staleCompletions; ++s.skipped; }
      else if (!success) { ++s.failed; ++s.skipped; }
      else {
        ++s.rendered;
        if (nowNs() > work->deadlineNs) ++s.deadlineMisses;
        retired = s.latest.exchange(work);
      }
    }
  }
  // Even a late cooperative exit destroys the context on its owner thread.
  renderer.reset(); factory = {};
  { std::lock_guard lock(s.mutex); s.complete = true; }
  s.finished.notify_all();
}
ProgramRenderWorker::Diagnostics ProgramRenderWorker::diagnostics() const {
  auto& s = *state_; s.reportStall();
  return {s.accepted,s.coalesced,s.rendered,s.failed,s.stalled,s.skipped,s.contended,s.rejectedStale,
      s.staleCompletions,s.deadlineMisses,s.queued,s.maximumQueued,s.config.enabled,s.busy,s.cancelled,s.complete};
}
std::shared_ptr<const ProgramRenderWorker::Work> ProgramRenderWorker::lastRendered() const {
  return state_->latest.load();
}
// Caller must hold joinMutex_. All thread_ access lives here so that no path,
// including the failure path, can touch the thread outside the lock.
bool ProgramRenderWorker::retireJoined(std::shared_ptr<State> s) noexcept {
  // No allocation is permitted on the destructor path. A stopped state's
  // mailbox can remain empty; submit/reset reject before indexing it.
  std::vector<std::shared_ptr<const Work>> retired;
  std::shared_ptr<const Work> retiredLatest;
  if (!thread_.joinable()) return s->complete;
  try {
    bool done;
    {
      std::unique_lock lock(s->mutex);
      done = s->finished.wait_for(lock, s->config.shutdownBudget, [&] { return s->complete.load(); });
      s->skipped += s->depth; s->head = s->depth = 0; s->queued = 0;
      retired.swap(s->queue);
      retiredLatest = s->latest.exchange({});
    }
    if (thread_.joinable()) { if (done) thread_.join(); else thread_.detach(); }
    return done;
  } catch (...) {
    // Renderer state has shared lifetime even if a platform wait/join fails.
    // Normal operation cannot fail detach: this class owns the joinable thread.
    try { if (thread_.joinable()) thread_.detach(); } catch (...) {}
    return false;
  }
}
bool ProgramRenderWorker::shutdown() noexcept {
  auto s = state_;
  s->cancelled = true; s->wake.notify_all();
  // Concurrent shutdown callers never wait behind another caller's budget.
  std::unique_lock<std::mutex> joinLock(joinMutex_, std::try_to_lock);
  if (!joinLock.owns_lock()) return s->complete;
  return retireJoined(std::move(s));
}
}
