#pragma once

#include "modules/Interfaces.h"
#include "rpc/Json.h"
#include "compositor/ColorGradeParams.h"
#include "graphics/GraphicsTimeline.h"
#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <functional>
#include <map>

namespace corevideo::core {

// Control facts only. Pixels and device ownership belong to the isolated GPU
// monitor worker. A lost UI close cannot leave unbounded monitor demand alive.
class GradePreviewController {
 public:
  using Clock = std::chrono::steady_clock;
  static constexpr size_t Capacity = 3;
  static constexpr size_t EventCapacity = 16, EventByteBudget = 512 * 1024;
  static constexpr auto Lease = std::chrono::seconds(3);
  static constexpr auto Interval = std::chrono::milliseconds(100);
  static constexpr auto PlayInterval = std::chrono::milliseconds(33);
  // Live authored canvas (perf/designer-live-preview step 2). One draft
  // revision is in the render pipeline at a time: newer drafts that arrive
  // while it is in flight wait in a single latest-value slot (`next`), never a
  // queue, and the newest is admitted as soon as the in-flight one settles. A
  // revision that has not settled within InFlightBound is replaced anyway, so a
  // lost result cannot pin the canvas. While a revision is in flight the
  // controller submits on every render tick (bounded by the render budget);
  // a settled canvas falls back to Interval, so a parked Designer costs nothing.
  static constexpr auto InFlightBound = std::chrono::milliseconds(500);
  using PrepareDraft = std::function<std::optional<modules::CompositorOverlayContent>(const rpc::Json&, bool& busy)>;
  // Designer preview timing (perf/designer-live-preview). Always on and fixed
  // size: counters plus a ring of admitted-to-ready latencies, reduced to
  // percentiles only when a snapshot asks. Nothing allocates per tick.
  static constexpr size_t LatencySamples = 64;
  struct Stats {
    uint64_t admitted = 0, ready = 0, supersededBeforeReady = 0, coalesced = 0;
    double admitToReadyP50Ms = 0, admitToReadyP95Ms = 0, admitToReadyMaxMs = 0, lastPrepareMs = 0;
    size_t samples = 0;
  };
  Stats stats() const {
    Stats result;
    result.admitted = admitted_; result.ready = readyRevisions_;
    result.supersededBeforeReady = supersededBeforeReady_; result.coalesced = coalesced_;
    result.lastPrepareMs = lastPrepareMs_; result.samples = latencyCount_;
    const size_t n = (std::min)(latencyCount_, LatencySamples);
    if (n == 0) return result;
    std::array<double, LatencySamples> sorted{};
    std::copy(latencies_.begin(), latencies_.begin() + static_cast<std::ptrdiff_t>(n), sorted.begin());
    std::sort(sorted.begin(), sorted.begin() + static_cast<std::ptrdiff_t>(n));
    const auto at = [&](double q) { return sorted[(std::min)(n - 1, size_t(q * double(n - 1) + 0.5))]; };
    result.admitToReadyP50Ms = at(0.5); result.admitToReadyP95Ms = at(0.95); result.admitToReadyMaxMs = sorted[n - 1];
    return result;
  }
  bool renew(const rpc::Json& command, Clock::time_point now = Clock::now()) {
    const auto found = active_.find(command.getString("instanceId"));
    const auto revision = command.getNumber("revision", -1);
    if (found == active_.end() || found->second.demand.sourceId != command.getString("sourceId") ||
        (revision != found->second.demand.revision && (!found->second.next || revision != found->second.next->demand.revision)) ||
        now-found->second.renewed >= Lease) return false;
    found->second.renewed = now; return true;
  }

  // Admits each waiting draft whose in-flight revision has settled (or
  // exceeded InFlightBound). Runs off the render thread: the RPC resource pump
  // calls it under its opportunistic core lock, and configure() admits an
  // incoming draft directly when nothing is in flight. A busy private compiler
  // leaves the draft waiting for the next call; a refused draft reports its own
  // revision unavailable (the accepted preview stays) and is dropped.
  size_t promote(Clock::time_point now, const PrepareDraft& prepareDraft) {
    size_t promoted = 0;
    for (auto& [id, entry] : active_) {
      if (!entry.next || (!entry.settled && now - entry.admitted < InFlightBound)) continue;
      bool busy = false;
      const auto started = Clock::now();
      auto overlay = prepareDraft ? prepareDraft(entry.next->draft, busy) : std::nullopt;
      const double prepareMs = lastPrepareMs_ = std::chrono::duration<double, std::milli>(Clock::now() - started).count();
      if (!overlay) {
        if (busy) continue;
        emit({entry.next->demand, 0, 0, 0, {}, "unavailable", "graphics-draft-refused"});
        entry.next.reset();
        continue;
      }
      if (!entry.settled) ++supersededBeforeReady_;
      auto demand = std::move(entry.next->demand);
      demand.lowerThirdPreview = std::move(*overlay);
      entry.demand = std::move(demand);
      entry.playStarted = now;
      entry.next.reset();
      admit(entry, now, prepareMs);
      ++promoted;
    }
    return promoted;
  }
  // An authored revision is travelling the pipeline (compile, raster, render,
  // export): poll it every render tick, for at most InFlightBound.
  bool inFlight(Clock::time_point now) const {
    return std::any_of(active_.begin(), active_.end(), [&](const auto& entry) {
      return entry.second.authored && !entry.second.settled && now - entry.second.admitted < InFlightBound;
    });
  }
  bool waiting() const {
    return std::any_of(active_.begin(), active_.end(), [](const auto& entry) { return entry.second.next.has_value(); });
  }

  bool configure(const rpc::Json& command, modules::CompositorColorGrade grade,
                 bool supported, Clock::time_point now = Clock::now(),
                 std::function<std::optional<modules::CompositorOverlayContent>(const rpc::Json&)> prepareDraft = {}) {
    const rpc::Json* draftInput = nullptr;
    const auto id = command.getString("instanceId"), source = command.getString("sourceId");
    const auto number = command.getNumber("revision", -1);
    if (id.empty() || id.size() > 64 || source.empty() || source.size() > 256 ||
        !std::isfinite(number) || number < 0 || number > 2147483647 || std::floor(number) != number) return false;
    modules::GradePreviewDemand demand{id, source, static_cast<int64_t>(number), std::move(grade)};
    if (const auto* preview=command.get("lowerThirdPreview"); preview && !preview->isNull()) {
      const auto* style=preview->get("appearance");
      if (source!="preview:lower-third" || !style || (!style->isObject() && !style->isNull())) return false;
      modules::CompositorOverlayContent overlay;
      if (const auto* graphics = preview->get("graphics"); graphics && !graphics->isNull()) {
        if (!prepareDraft) return false;
        draftInput = graphics;
      }
      if (const auto* graphics = preview->get("graphics")) {
        if (const auto* play = graphics->get("play"); play && !play->isNull())
          demand.playMode = play->getString("mode");
        // animations.speed (graphics styles slice 11): the private Play in/out
        // runs at the same speed as Program. Range-checked by the draft reader.
        if (const auto* animations = graphics->get("animations"); animations && animations->isObject())
          if (const auto* speed = animations->get("speed"); speed && speed->isNumber() && std::isfinite(speed->asNumber()) &&
              speed->asNumber() >= graphics::kGraphicsMotionSpeedMin && speed->asNumber() <= graphics::kGraphicsMotionSpeedMax)
            demand.playSpeed = speed->asNumber();
      }
      if (!draftInput) {
      if (!style->isNull()) {overlay.appearance=compositor::LowerThirdAppearance::parse(*style);if (!overlay.appearance.valid()) return false;}
      overlay.text=preview->getString("name","Example guest"); overlay.title=preview->getString("title","Producer");
      overlay.brandColor=preview->getString("brandColor","#44C1A1"); overlay.brandBackgroundColor=preview->getString("backgroundColor","#0C1118");
      overlay.imageUri=preview->getString("imageUri"); overlay.keyPosition=preview->getString("position","lower-left");
      overlay.fontFamily=preview->getString("fontFamily","Inter");
      if (overlay.text.size()>256 || overlay.title.size()>256 || overlay.imageUri.size()>4096 || overlay.fontFamily.size()>128) return false;
      }
      demand.lowerThirdPreview=std::move(overlay);
      demand.lowerThirdPreviewSignature=preview->stringify();
      for (const auto& [other,entry]:active_) if (other!=id && entry.demand.lowerThirdPreview) return false;
    }
    demand.scopesEnabled = command.get("scopesEnabled") && command.get("scopesEnabled")->asBool();
    demand.scopesOriginal = command.get("scopesOriginal") && command.get("scopesOriginal")->asBool();
    demand.compareOriginal = command.get("compareOriginal") && command.get("compareOriginal")->asBool();
    const double hm = command.getNumber("histogramMode", 1), wm = command.getNumber("waveformMode", 1), sv = command.getNumber("scopeView", 0);
    if (!std::isfinite(hm) || hm < 0 || hm > 1 || std::floor(hm) != hm ||
        !std::isfinite(wm) || wm < 0 || wm > 2 || std::floor(wm) != wm || !std::isfinite(sv) || sv<0 || sv>3 || std::floor(sv)!=sv) return false;
    demand.histogramMode = int(hm); demand.waveformMode = int(wm); demand.scopeView = int(sv);
    if (const auto* roi = command.get("scopeRoi")) {
      if (!roi->isObject()) return false;
      demand.scopeRoi.enabled = roi->get("enabled") && roi->get("enabled")->asBool();
      demand.scopeRoi.x = roi->getNumber("x",0); demand.scopeRoi.y = roi->getNumber("y",0);
      demand.scopeRoi.width = roi->getNumber("width",1); demand.scopeRoi.height = roi->getNumber("height",1);
      if (const auto* shape=roi->get("shape")) {
        if (!shape->isString()) return false;
        demand.scopeRoi.shape=shape->asString();
      }
      const auto rr = roi->getNumber("revision",0);
      if (!std::isfinite(rr) || rr<0 || rr>2147483647 || std::floor(rr)!=rr) return false;
      demand.scopeRoi.revision = int64_t(rr);
      if (!demand.scopeRoi.valid()) return false;
    }
    const bool enabled = !command.get("enabled") || command.get("enabled")->asBool();
    if (!enabled) { active_.erase(id); return true; }
    if (!supported) return emit({demand, 0, 0, 0, {}, "unavailable", "native-grade-preview-not-built"});
    auto found = active_.find(id);
    if (found == active_.end()) {
      if (active_.size() >= Capacity) {
        return emit({demand, 0, 0, 0, {}, "unavailable", "grade-preview-capacity"});
      }
    } else {
      if (demand.revision < found->second.demand.revision) return true;
      // Equal revision is a lease refresh, never permission to replace a draft.
      if (demand.revision == found->second.demand.revision && (source != found->second.demand.sourceId ||
          demand.lowerThirdPreview.has_value()!=found->second.demand.lowerThirdPreview.has_value() ||
          demand.lowerThirdPreviewSignature!=found->second.demand.lowerThirdPreviewSignature ||
          !modules::colorGradesEqual(demand.grade, found->second.demand.grade) ||
          demand.scopesEnabled != found->second.demand.scopesEnabled || demand.scopesOriginal != found->second.demand.scopesOriginal || demand.compareOriginal != found->second.demand.compareOriginal ||
          !(demand.scopeRoi == found->second.demand.scopeRoi) || demand.histogramMode != found->second.demand.histogramMode || demand.waveformMode != found->second.demand.waveformMode || demand.scopeView != found->second.demand.scopeView)) return false;
      if (demand.revision == found->second.demand.revision) {
        found->second.renewed = now;
        return true;
      }
      if (auto& next = found->second.next) {
        // The waiting draft follows the same rules: older is ignored, equal is
        // a lease refresh and may not change content.
        if (demand.revision < next->demand.revision) return true;
        if (demand.revision == next->demand.revision) {
          if (demand.lowerThirdPreviewSignature != next->demand.lowerThirdPreviewSignature) return false;
          found->second.renewed = now;
          return true;
        }
      }
      // An authored revision is in flight: hold this newer draft in the single
      // waiting slot (replacing an older waiting draft) instead of preparing it
      // now. promote() admits it when the in-flight revision settles.
      if (draftInput && found->second.authored && !found->second.settled && now - found->second.admitted < InFlightBound) {
        if (found->second.next) ++coalesced_;
        found->second.next = Waiting{std::move(demand), *draftInput};
        found->second.renewed = now;
        return true;
      }
    }
    // Preparation can start a worker and replace its wanted revision. Validate
    // the entire control envelope before those effects; unchanged lease renewals
    // above retain their accepted draft without touching its compiler.
    double prepareMs = 0;
    if (draftInput) {
      const auto started = Clock::now();
      auto graphicsDraft = prepareDraft(*draftInput);
      prepareMs = lastPrepareMs_ = std::chrono::duration<double, std::milli>(Clock::now() - started).count();
      if (!graphicsDraft) return false;
      demand.lowerThirdPreview = std::move(*graphicsDraft);
    }
    if (found == active_.end()) found = active_.emplace(id, Entry{std::move(demand), now, now}).first;
    else {
      if (!found->second.settled) ++supersededBeforeReady_;
      if (found->second.next) { ++coalesced_; found->second.next.reset(); } // this draft is newer
      found->second.demand = std::move(demand);
      found->second.playStarted = now;
      found->second.renewed = now;
    }
    found->second.authored = draftInput != nullptr;
    admit(found->second, now, prepareMs);
    return true;
  }

  void tick(const std::vector<modules::VideoFrame>& frames, modules::ICompositor& compositor,
            int64_t sequence, Clock::time_point now = Clock::now()) {
    const bool playing = std::any_of(active_.begin(), active_.end(), [](const auto& entry) {
      return !entry.second.demand.playMode.empty();
    });
    if (!inFlight(now) && now - lastTick_ < (playing ? PlayInterval : Interval)) return;
    lastTick_ = now;
    for (auto it = active_.begin(); it != active_.end();) {
      if (now - it->second.renewed >= Lease) it = active_.erase(it); else ++it;
    }
    if (active_.empty() && !submitted_) return;
    modules::MonitorRenderRequest request;
    request.sequence = sequence;
    for (const auto& [id, entry] : active_) {
      auto& demand = request.gradePreviews.emplace_back(entry.demand);
      if (!demand.playMode.empty())
        demand.playElapsedMs = std::chrono::duration<double, std::milli>(now - entry.playStarted).count() * demand.playSpeed;
    }
    for (const auto& frame : frames) {
      if (std::any_of(request.gradePreviews.begin(), request.gradePreviews.end(),
          [&](const auto& demand) { return demand.sourceId == frame.participantId; })) request.frames.push_back(frame);
    }
    compositor.submitGradePreviews(std::move(request));
    submitted_ = !active_.empty();
    const auto result = compositor.latestGradePreviews();
    const auto health = compositor.gradePreviewDiagnostics();
    for (auto& item : active_) {
      const auto& id = item.first;
      auto& entry = item.second;
      const auto match = result ? std::find_if(result->gradePreviews.begin(), result->gradePreviews.end(),
          [&](const auto& surface) { return surface.demand.instanceId == id &&
              surface.demand.sourceId == entry.demand.sourceId && surface.demand.revision == entry.demand.revision; })
          : std::vector<modules::GradePreviewSurface>::const_iterator{};
      if (!result || match == result->gradePreviews.end()) {
        const modules::GradePreviewSurface pending{entry.demand, 0, 0, 0, {}, health.failed ? "unavailable" : "preparing",
              health.failed ? health.failureReason : "awaiting-native-grade"};
        settle(entry, pending, now);
        emit(pending, timingJson(entry, result ? result->workMs : 0));
        continue;
      }
      auto surface = *match;
      if (surface.status == "ready" && surface.demand.playMode.empty())
        entry.demand.playMode.clear();
      if (surface.scopes.texture.width > 0) {
        if (entry.scopeExport != surface.scopes.texture.frameNumber) {
          entry.scopeExport = surface.scopes.texture.frameNumber;
          entry.scopeCompletionObservedAtMs = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
        }
        if (!entry.scopeObserved || entry.scopeEpoch != surface.scopes.sourceEpoch || entry.scopeFrame != surface.scopes.sourceFrameId) {
          entry.scopeObserved = true; entry.scopeEpoch = surface.scopes.sourceEpoch; entry.scopeFrame = surface.scopes.sourceFrameId; entry.scopeAdvanced = now;
        } else if (now-entry.scopeAdvanced >= std::chrono::seconds(1)) surface.scopes.status = "stale";
      }
      // An authored canvas is intentionally a still until the draft changes.
      // Its stable revision is not a stalled live-video source.
      if (surface.status == "ready" && !entry.demand.lowerThirdPreview) {
        if (!entry.observed || entry.epoch != surface.sourceEpoch || entry.frame != surface.sourceFrameId) {
          entry.observed = true; entry.epoch = surface.sourceEpoch; entry.frame = surface.sourceFrameId;
          entry.advanced = now;
        } else if (now - entry.advanced >= std::chrono::seconds(1)) {
          surface.status = "stale"; surface.reason = "source-not-advancing";
        }
      }
      settle(entry, surface, now);
      emit(surface, timingJson(entry, result->workMs));
    }
  }
  // Off the render thread (the RPC resource pump, every ~8 ms): an authored
  // revision that settled on the worker since the last tick is published now
  // rather than a tick later, so its waiting successor can be admitted in the
  // same pass. Only settling surfaces are emitted here; tick() still owns the
  // periodic status, staleness and lease work.
  size_t observe(const modules::ICompositor& compositor, Clock::time_point now) {
    if (!inFlight(now)) return 0;
    const auto result = compositor.latestGradePreviews();
    if (!result) return 0;
    size_t settled = 0;
    for (auto& [id, entry] : active_) {
      if (!entry.authored || entry.settled) continue;
      const auto match = std::find_if(result->gradePreviews.begin(), result->gradePreviews.end(), [&](const auto& surface) {
        return surface.demand.instanceId == id && surface.demand.sourceId == entry.demand.sourceId &&
            surface.demand.revision == entry.demand.revision;
      });
      if (match == result->gradePreviews.end() ||
          (!(match->status == "ready" && match->texture.width > 0) && match->status != "unavailable")) continue;
      auto surface = *match;
      if (surface.status == "ready" && surface.demand.playMode.empty()) entry.demand.playMode.clear();
      settle(entry, surface, now);
      emit(surface, timingJson(entry, result->workMs));
      ++settled;
    }
    return settled;
  }
  std::vector<rpc::Json> drain() { auto result = std::move(events_); events_.clear(); eventBytes_ = 0; return result; }
  size_t pendingEventBytes() const { return eventBytes_; }
  uint64_t eventsRefused() const { return eventsRefused_; }
  rpc::Json diagnostics(const modules::ICompositor& compositor) const {
    return diagnosticsFor(compositor, active_.size(), eventBytes_, eventsRefused_, stats());
  }
  // `authoring` (Designer timing) is the controller's stats plus the private
  // template compiler's last compile time, captured under coreMutex.
  static rpc::Json diagnosticsFor(const modules::ICompositor& compositor, size_t editors, size_t eventBytes = 0, uint64_t eventsRefused = 0,
                                  const Stats& stats = {}, double lastCompileMs = 0) {
    const auto worker = compositor.gradePreviewDiagnostics();
    rpc::Json authoring = rpc::Json::Object{{"inFlightCadence", "render-tick"},
      {"inFlightBoundMs", double(std::chrono::milliseconds(InFlightBound).count())}, {"revisionsAdmitted", double(stats.admitted)}, {"revisionsReady", double(stats.ready)},
      {"revisionsSupersededBeforeReady", double(stats.supersededBeforeReady)}, {"revisionsCoalesced", double(stats.coalesced)},
      {"latencySamples", double((std::min)(stats.samples, LatencySamples))},
      {"admittedToReadyP50Ms", stats.admitToReadyP50Ms}, {"admittedToReadyP95Ms", stats.admitToReadyP95Ms},
      {"admittedToReadyMaxMs", stats.admitToReadyMaxMs}, {"lastPrepareMs", stats.lastPrepareMs}, {"lastCompileMs", lastCompileMs}};
    return rpc::Json::Object{{"observationVersion", "grade-preview-v1"}, {"authoring", std::move(authoring)},
      {"supported", compositor.supportsGradePreview()}, {"activeEditors", double(editors)},
      {"editorCapacity", double(Capacity)}, {"leaseMs", 3000}, {"requestedIntervalMs", 100}, {"playIntervalMs", 33},
      {"eventCapacity", double(EventCapacity)}, {"eventByteCapacity", double(EventByteBudget)},
      {"eventOwnedByteBound", double(eventBytes)}, {"eventsRefused", double(eventsRefused)},
      {"submitted", double(worker.submitted)}, {"completed", double(worker.completed)},
      {"superseded", double(worker.superseded)}, {"failed", double(worker.failed)},
      {"pending", worker.pending}, {"pendingCapacity", 1}, {"lastWorkMs", worker.lastWorkMs},
      {"retainedInputs", double(worker.retainedInputs)}, {"retainedInputBytes", double(worker.retainedInputBytes)},
      {"failureReason", worker.failureReason}, {"displayPresentationVerified", false}};
  }
  size_t size() const { return active_.size(); }
 private:
  // A validated control envelope whose draft has not been prepared yet.
  struct Waiting { modules::GradePreviewDemand demand; rpc::Json draft; };
  struct Entry {
    modules::GradePreviewDemand demand;
    Clock::time_point renewed, playStarted{}, advanced{};
    uint64_t epoch = 0;
    int64_t frame = 0;
    bool observed = false;
    bool scopeObserved = false;
    uint64_t scopeEpoch = 0;
    int64_t scopeFrame = 0, scopeExport = -1, scopeCompletionObservedAtMs = 0;
    Clock::time_point scopeAdvanced{};
    // Designer timing: when the current revision entered the pipeline, what its
    // draft preparation cost on the command thread, and whether it has settled
    // (first ready texture or unavailable verdict for this exact revision).
    Clock::time_point admitted{};
    double prepareMs = 0, admittedToReadyMs = 0;
    bool settled = false;
    bool authored = false;           // the current revision came from a graphics draft
    std::optional<Waiting> next;     // newest draft held while `demand` is in flight
  };
  void admit(Entry& entry, Clock::time_point now, double prepareMs) {
    entry.admitted = now; entry.prepareMs = prepareMs; entry.admittedToReadyMs = 0; entry.settled = false;
    ++admitted_;
  }
  void settle(Entry& entry, const modules::GradePreviewSurface& surface, Clock::time_point now) {
    if (entry.settled || surface.demand.revision != entry.demand.revision) return;
    const bool ready = surface.status == "ready" && surface.texture.width > 0;
    if (!ready && surface.status != "unavailable") return;
    entry.settled = true;
    if (!ready) return;
    entry.admittedToReadyMs = std::chrono::duration<double, std::milli>(now - entry.admitted).count();
    latencies_[latencyCount_++ % LatencySamples] = entry.admittedToReadyMs;
    ++readyRevisions_;
  }
  // Per-revision timing rides each authored-canvas observation (three fixed
  // numeric keys, inside emit's byte bound). Source grade events are unchanged.
  static rpc::Json timingJson(const Entry& entry, double workerMs) {
    if (!entry.demand.lowerThirdPreview) return {};
    return rpc::Json::Object{{"prepareMs", entry.prepareMs}, {"admittedToReadyMs", entry.admittedToReadyMs},
        {"workerMs", workerMs}};
  }
  bool emit(const modules::GradePreviewSurface& surface, rpc::Json timing = {}) {
    // Fixed schema: 16 KiB covers map nodes/keys and fixed diagnostic strings;
    // eight bytes per varying character bounds allocation and JSON escaping.
    size_t bound = 16384;
    for (const auto* text : {&surface.demand.instanceId, &surface.demand.sourceId,
         &surface.status, &surface.reason, &surface.texture.sharedHandleHex, &surface.texture.format,
         &surface.scopes.texture.sharedHandleHex, &surface.scopes.texture.format,
         &surface.scopes.status, &surface.scopes.roi.shape}) {
      bound = text->size() > EventByteBudget / 8 ? EventByteBudget + 1 :
          (std::min)(EventByteBudget + 1, bound + 8 * text->size());
    }
    if (events_.size() == EventCapacity || bound > EventByteBudget || eventBytes_ > EventByteBudget - bound) {
      ++eventsRefused_; return false;
    }
    const auto& t = surface.texture;
    const auto& scope = surface.scopes; const auto& st = scope.texture;
    const auto found = active_.find(surface.demand.instanceId);
    const auto now = Clock::now();
    const auto age = found!=active_.end() && found->second.observed ? double(std::chrono::duration_cast<std::chrono::milliseconds>(now-found->second.advanced).count()) : 0;
    const auto scopeAge = found!=active_.end() && found->second.scopeObserved ? double(std::chrono::duration_cast<std::chrono::milliseconds>(now-found->second.scopeAdvanced).count()) : 0;
    rpc::Json::Object event{
      {"type", "grade-preview"}, {"instanceId", surface.demand.instanceId},
      {"sourceId", surface.demand.sourceId}, {"revision", double(surface.demand.revision)},
      {"sourceEpoch", double(surface.sourceEpoch)}, {"sourceFrameId", double(surface.sourceFrameId)},
      {"captureTimestamp100ns", double(surface.captureTimestamp100ns)},
      {"sourceAgeMs", std::max(0.0,age)},
      {"status", surface.status}, {"reason", surface.reason},
      {"scopes", rpc::Json::Object{{"status", scope.status},
        {"roi", rpc::Json::Object{{"enabled",scope.roi.enabled},{"x",scope.roi.x},{"y",scope.roi.y},{"width",scope.roi.width},{"height",scope.roi.height},{"revision",double(scope.roi.revision)},{"shape",scope.roi.shape}}},
        {"roiPixelX",scope.roiPixels.x},{"roiPixelY",scope.roiPixels.y},{"roiPixelWidth",scope.roiPixels.width},{"roiPixelHeight",scope.roiPixels.height},
        {"reason", scope.status=="unavailable"?"scope-render-or-export-unavailable":scope.status=="stale"?"source-not-advancing":scope.status=="held"?"source-unavailable":st.width==0?"awaiting-native-scopes":""}, {"sourceAgeMs", std::max(0.0,scopeAge)},
        {"completionObservedAtUnixMs", st.width>0 && found!=active_.end()?double(found->second.scopeCompletionObservedAtMs):0},
        {"view", surface.demand.scopeView}, {"revision", double(scope.revision)}, {"sourceEpoch", double(scope.sourceEpoch)}, {"sourceFrameId", double(scope.sourceFrameId)},
        {"captureTimestamp100ns", double(scope.captureTimestamp100ns)}, {"original", scope.original},
        {"sampleWidth", st.width>0?256:0}, {"sampleHeight", st.width>0?144:0}, {"sampleCount", st.width>0?scope.roi.sampleCount():0}, {"colorSpace", "rec709-sdr-assumed"},
        {"units", "encoded 0-1; waveform 0-100%; Cb/Cr"},
        {"texture", textureJson(st)}}},
      {"texture", textureJson(t)}};
    if (!timing.isNull()) event.emplace("timing", std::move(timing));
    events_.emplace_back(std::move(event));
    eventBytes_ += bound;
    return true;
  }
  // macOS adds the IOSurface id only when one is set, so a Windows texture object keeps exactly
  // its five keys (the program-shared-texture rule, ProgramFramePreview.cpp; grading Phase 2, P6).
  static rpc::Json textureJson(const modules::ParticipantSharedTexture& texture) {
    rpc::Json::Object object{{"sharedHandleHex", texture.sharedHandleHex}, {"width", texture.width},
        {"height", texture.height}, {"format", texture.format}, {"frameNumber", double(texture.frameNumber)}};
    if (texture.iosurfaceId != 0) object.emplace("iosurfaceId", double(texture.iosurfaceId));
    return rpc::Json(std::move(object));
  }
  std::map<std::string, Entry> active_;
  std::vector<rpc::Json> events_;
  size_t eventBytes_ = 0;
  uint64_t eventsRefused_ = 0;
  Clock::time_point lastTick_{};
  bool submitted_ = false;
  uint64_t admitted_ = 0, readyRevisions_ = 0, supersededBeforeReady_ = 0, coalesced_ = 0;
  double lastPrepareMs_ = 0;
  std::array<double, LatencySamples> latencies_{};
  size_t latencyCount_ = 0;
};
}
