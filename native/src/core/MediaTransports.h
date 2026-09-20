#pragma once

#include "core/BoundedAsyncLog.h"
#include "core/MediaTransportPolicy.h"
#include "modules/Interfaces.h"
#include "modules/MediaPlaybackTimeline.h"
#include "modules/MediaVideoPresentation.h"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <functional>
#include <map>
#include <mutex>
#include <optional>
#include <set>
#include <thread>
#include <stdexcept>
#include <cstdio>
#include <utility>
#include <vector>

namespace corevideo::core {
// Evolved from modules::OwnedMediaFrameSource (#535 slice 3b). The decoder
// owner no longer derives what exists from the render plan's media layers
// every tick: MediaCore computes a DESIRED SET at command time and hands it to
// apply(), which owns one entry per source id, its worker thread and its
// transport state (Cued / Live / Paused / Ended).
//
// Each admitted asset owns a decoder thread; one slow source cannot block
// other sources or the render caller. Only apply() creates workers and only
// manage() joins them.
//
// The #449 cue hand-off is GONE, and deliberately: a cue and its live clip are
// the SAME entry now (one source id, one decoder), so the transition that used
// to re-key a retiring decoder onto an arriving request is an in-place Resume.
// Playback-identity collisions are impossible by construction for the same
// reason — one id, one entry — so their warnings are gone too.
class MediaTransports final {
 public:
  using DecoderFactory = std::function<std::unique_ptr<modules::IMediaFrameSource>()>;

  struct Entry {
    // Guarded by `mutex`, all of it. The worker builds the decoder layer from
    // `desired` + `state`; apply()/operatorAction() are the only writers of
    // `desired`, `state`, `restartRequested` and `resumeRequested`.
    MediaTransportDesired desired;
    MediaTransportState state = MediaTransportState::Cued;
    bool restartRequested = false;  // worker: a NEW decoder instance at 0, behind the held frame
    bool resumeRequested = false;   // worker: cued -> live: dropQueued + audio on + clock resume
    std::mutex mutex;
    std::condition_variable changed;
    bool wake = false;
    // Set by the worker when it freezes the decoder clock for a pause that
    // follows playback; cleared once it has resumed that clock and re-timed
    // the prepared frames by the same paused duration (frozenAtMs -> resume).
    bool clockFrozen = false;
    int64_t frozenAtMs = 0;
    modules::MediaVideoPresentation video;
    std::deque<modules::AudioFrame> audio;
    int64_t audioNextTime = 0;
    bool audioEverProduced = false;
    std::vector<std::string> warnings;
    // Published by the worker for snapshot(); -1 when the decoder cannot say
    // — which is what they START as, because a transport that has not yet run
    // a worker iteration has measured nothing and must not report 0.
    int64_t durationMs = -1, positionMs = -1;
    std::atomic<bool> stop{false}, finished{false}, wantsVideo{false}, wantsAudio{false};
    std::thread thread;
  };

  // Bus membership changes: `added == false` means the entry was retired.
  struct Change { std::string sourceId; bool added = false; std::shared_ptr<Entry> entry; };
  struct Status {
    std::string sourceId, assetId;
    MediaTransportState state = MediaTransportState::Cued;
    bool loop = false, onProgram = false, onPreview = false;
    int64_t positionMs = 0, durationMs = 0;
  };

  explicit MediaTransports(DecoderFactory factory)
      : factory_(std::move(factory)), manager_([this] { manage(); }) {}
  ~MediaTransports() {
    { std::lock_guard<std::mutex> lock(mutex_); stopped_ = true; }
    changed_.notify_all();
    manager_.join();
  }

  // Command-time desired set (Program + Preview). Applies decideMediaTransport
  // per source id, starts/stops workers (never opens a decoder inline) and
  // returns the bus membership changes for the caller to add to / remove from
  // the source bus.
  std::vector<Change> apply(const std::vector<MediaTransportDesired>& desired, int64_t nowNs) {
    // The caller's command instant (steady_clock nanoseconds). It is what the
    // release grace below is measured against, and it is deliberately the
    // CALLER's clock rather than one read in here, so the grace is measured on
    // the same timeline as collectExpiredReleases()'s render-tick sweep.
    std::vector<Change> changes;
    std::vector<std::pair<std::string, std::shared_ptr<Entry>>> starting;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      std::map<std::string, MediaTransportDesired> current;
      for (const auto& row : desired) {
        if (row.sourceId.empty() || row.path.empty()) continue;
        current.emplace(row.sourceId, row);  // one id, one entry: the first row wins
      }
      warnings_.clear();
      // Every id either side of the transition, in one ordered pass.
      std::set<std::string> ids;
      for (const auto& [id, row] : previousDesired_) ids.insert(id);
      for (const auto& [id, row] : current) ids.insert(id);
      for (const auto& id : ids) {
        const auto previousIt = previousDesired_.find(id);
        const auto currentIt = current.find(id);
        const std::optional<MediaTransportDesired> previous =
            previousIt == previousDesired_.end() ? std::nullopt : std::optional<MediaTransportDesired>(previousIt->second);
        const std::optional<MediaTransportDesired> curr =
            currentIt == current.end() ? std::nullopt : std::optional<MediaTransportDesired>(currentIt->second);
        const auto found = entries_.find(id);
        MediaTransportState state = MediaTransportState::Cued;
        if (found != entries_.end()) {
          std::lock_guard<std::mutex> entryLock(found->second->mutex);
          state = found->second->state;
        }
        // Still (or again) desired: it is not going anywhere, so cancel any
        // release grace it was sitting in and give it its audio back if it was
        // rolling. Everything else about it was left untouched while it waited.
        if (curr && releasePendingNs_.erase(id) > 0 && found != entries_.end()) {
          found->second->wantsAudio.store(state == MediaTransportState::Live);
        }
        const auto decision = decideMediaTransport(previous, curr, state);
        switch (decision.action) {
          case MediaTransportAction::None:
            if (!curr) break;
            if (found != entries_.end()) {
              std::lock_guard<std::mutex> entryLock(found->second->mutex);
              found->second->desired = *curr;  // bus flags may have changed
              break;
            }
            // Still desired, unchanged, and yet NOT RUNNING: the cap refused it
            // or its worker failed to start. The transition table cannot see
            // that (it only compares desired rows), so a plain `None` would
            // strand the source forever AND lose its refusal warning on the
            // next apply, since warnings_ is rebuilt every pass. Treat it as a
            // fresh open: it re-pushes the refusal while the cap still bites,
            // and opens the moment a decoder frees. This is what the ported
            // manage() loop did implicitly by re-deriving the desired set every
            // 2 ms.
            {
              const auto reopen = decideMediaTransport(std::nullopt, curr, MediaTransportState::Cued);
              openLocked(id, *curr, reopen.next, starting, changes);
            }
            break;
          case MediaTransportAction::OpenLive:
          case MediaTransportAction::OpenCued:
            if (curr) openLocked(id, *curr, decision.next, starting, changes);
            break;
          case MediaTransportAction::Resume:
            if (found != entries_.end() && curr) {
              {
                std::lock_guard<std::mutex> entryLock(found->second->mutex);
                found->second->desired = *curr;
                found->second->state = MediaTransportState::Live;
                found->second->resumeRequested = true;
                found->second->wake = true;
              }
              found->second->wantsAudio.store(true);
              found->second->changed.notify_all();
            }
            break;
          case MediaTransportAction::RestartCued:
            if (found != entries_.end() && curr) {
              {
                std::lock_guard<std::mutex> entryLock(found->second->mutex);
                found->second->desired = *curr;
                found->second->state = MediaTransportState::Cued;
                found->second->restartRequested = true;
                found->second->audio.clear();
                found->second->wake = true;
              }
              found->second->wantsAudio.store(false);
              found->second->changed.notify_all();
            }
            break;
          case MediaTransportAction::Reopen:
            // The path (or the loop mode) changed: the held picture belongs to
            // a different file, so nothing survives. Retire, then open fresh
            // under the same id — two Change rows, remove before add.
            if (found != entries_.end()) {
              auto retiring = found->second;
              entries_.erase(found);
              retireLocked(retiring);
              changes.push_back({id, false, retiring});
            }
            if (curr) openLocked(id, *curr, decision.next, starting, changes);
            break;
          case MediaTransportAction::Pause:
          case MediaTransportAction::Play:
            if (found != entries_.end()) {
              {
                std::lock_guard<std::mutex> entryLock(found->second->mutex);
                if (curr) found->second->desired = *curr;
                found->second->state = decision.next;
                found->second->wake = true;
              }
              found->second->wantsAudio.store(decision.next == MediaTransportState::Live);
              found->second->changed.notify_all();
            }
            break;
          case MediaTransportAction::Release:
            // RELEASE HAS A SHORT GRACE, and it is load-bearing for the
            // headline promise. `applyPreviewScene` is reached from the
            // shell's Take batch (load-scene-graph + set-preview-scene in ONE
            // sync) but ALSO from `syncZoomMediaSpine`, a separate REPEATING
            // channel — CLAUDE.md documents that exact interleave for the take
            // record. One spine tick carrying the post-swap Preview while
            // Program still holds the outgoing scene leaves a CUED clip absent
            // from both desired sets for that single tick; retiring on the
            // spot destroys the warm decoder the Take is about to claim and
            // cold-starts the clip into the warming slate. Slice 3b removed
            // the cue hand-off that used to be the second chance, so this IS
            // the second chance. A source really gone is still released — just
            // one beat later, by a subsequent apply() or by the render tick's
            // collectExpiredReleases() sweep.
            //
            // It goes SILENT immediately. A clip genuinely cut off Program
            // must not keep feeding the mix for the length of the grace; the
            // re-claim turns its audio back on (above, and via Resume/Play).
            if (found != entries_.end()) {
              // PRESENCE, never a zero sentinel: `nowNs` is the caller's clock
              // and a caller that passes 0 is legitimate (every unit test does).
              auto since = releasePendingNs_.find(id);
              if (since == releasePendingNs_.end()) {
                since = releasePendingNs_.emplace(id, nowNs).first;
                found->second->wantsAudio.store(false);
                std::lock_guard<std::mutex> entryLock(found->second->mutex);
                found->second->audio.clear();
                found->second->wake = true;
              }
              if (nowNs - since->second < kReleaseGraceMs * 1000000) {
                // Carry the row forward so `previousDesired_` still holds it:
                // the re-claim is then an ordinary transition against the row
                // it actually had, not a cold `!previous` open onto an entry
                // that already exists.
                current.emplace(id, previousIt->second);
                found->second->changed.notify_all();
                break;
              }
              releasePendingNs_.erase(since);
              auto retiring = found->second;
              entries_.erase(found);
              retireLocked(retiring);
              changes.push_back({id, false, retiring});
            }
            audioNextTime_.erase(id);
            break;
        }
      }
      previousDesired_ = std::move(current);
    }
    // Threads start outside the lock, exactly as manage() used to do it.
    for (auto& [id, entry] : starting) {
      try {
        entry->thread = std::thread([this, entry] { run(entry); });
      } catch (const std::exception& error) {
        std::lock_guard<std::mutex> lock(mutex_);
        entries_.erase(id);
        warnings_.push_back(std::string("Media worker could not start: ") + error.what());
        for (auto it = changes.begin(); it != changes.end();) {
          if (it->added && it->sourceId == id) it = changes.erase(it); else ++it;
        }
      }
    }
    changed_.notify_all();
    return changes;
  }

  // Render-tick sweep (cheap: a usually-empty map under the owner lock).
  // apply() only runs at COMMAND time, so without this a source that left both
  // buses and is never mentioned again would hold its decoder — and its bus
  // entry — until some unrelated command happened to arrive. Returns the bus
  // removals for the caller, exactly like apply().
  std::vector<Change> collectExpiredReleases(int64_t nowNs) {
    std::vector<Change> changes;
    std::lock_guard<std::mutex> lock(mutex_);
    for (auto it = releasePendingNs_.begin(); it != releasePendingNs_.end();) {
      if (nowNs - it->second < kReleaseGraceMs * 1000000) { ++it; continue; }
      const auto id = it->first;
      it = releasePendingNs_.erase(it);
      const auto found = entries_.find(id);
      if (found == entries_.end()) continue;
      auto retiring = found->second;
      entries_.erase(found);
      retireLocked(retiring);
      audioNextTime_.erase(id);
      // The row was carried forward through the grace so a re-claim would be
      // an ordinary transition; now that it really is gone, forget it, or the
      // next apply() would re-open a grace for an entry that no longer exists.
      previousDesired_.erase(id);
      changes.push_back({id, false, retiring});
    }
    if (!changes.empty()) changed_.notify_all();
    return changes;
  }

  // Operator pause/play on `media:<assetId>`; false + reason when refused.
  bool operatorAction(const std::string& assetId, MediaOperatorAction action, std::string& reason) {
    std::lock_guard<std::mutex> lock(mutex_);
    // ONE ASSET CAN BE TWO SOURCES: a clip routed as `media:<id>` and the same
    // file behind a scene as `background:<id>`. The operator transport names
    // the ROUTE, so resolve it explicitly — `background:` sorts first in the
    // map, and taking the first match refused every pause as "it is a loop"
    // while the pausable copy was never consulted. Order: the exact
    // `media:<assetId>` source, else the first non-loop source for the asset,
    // else the first match at all (so the refusal still carries a real reason).
    std::shared_ptr<Entry> target;
    bool targetIsLoop = true;
    for (const auto& [id, entry] : entries_) {
      MediaTransportDesired desired;
      { std::lock_guard<std::mutex> entryLock(entry->mutex); desired = entry->desired; }
      if (desired.assetId != assetId) continue;
      if (desired.sourceId == "media:" + assetId) { target = entry; break; }
      if (!target || (targetIsLoop && !desired.loop)) { target = entry; targetIsLoop = desired.loop; }
    }
    if (target) {
      const auto& entry = target;
      MediaTransportDesired desired;
      MediaTransportState state;
      { std::lock_guard<std::mutex> entryLock(entry->mutex); desired = entry->desired; state = entry->state; }
      const auto decision = decideMediaOperator(desired, state, action, reason);
      if (!decision) return false;
      {
        std::lock_guard<std::mutex> entryLock(entry->mutex);
        entry->state = decision->next;
        // Ended -> Play is OpenLive on an entry that already exists: a new
        // decoder instance from 0, which is exactly what restartRequested is.
        if (decision->action == MediaTransportAction::OpenLive) entry->restartRequested = true;
        if (decision->next != MediaTransportState::Live) entry->audio.clear();
        entry->wake = true;
      }
      entry->wantsAudio.store(decision->next == MediaTransportState::Live);
      entry->changed.notify_all();
      return true;
    }
    reason = "No media transport is running for asset " + assetId + ".";
    return false;
  }

  // Render tick (under coreMutex, cheap): the frame due at ts for one entry,
  // or the held frame. PAUSE HOLDS THE ON-AIR FRAME (T1.2) — a paused (or
  // cued, or ended) entry never advances the presentation, and neither does a
  // resumed one until its worker has re-timed the frames prepared before the
  // pause (clockFrozen).
  static std::optional<modules::VideoFrame> selectVideo(Entry& entry, int64_t timestamp100ns) {
    std::lock_guard<std::mutex> lock(entry.mutex);
    const bool hold = entry.state != MediaTransportState::Live || entry.clockFrozen;
    const auto& selected = hold ? entry.video.hold() : entry.video.select(timestamp100ns);
    entry.wake = true;
    entry.changed.notify_all();
    if (!selected.hasPixels()) return std::nullopt;
    auto frame = selected;
    frame.timestampMs = timestamp100ns / 10000;
    // THE OWNER NAMES THE SOURCE, NOT THE DECODER. The decoder stamps
    // participantId from the layer it was handed, which is normally the same
    // string — but the owner is the authority on which bus source this is, and
    // the compositor looks a media layer up by exactly that id.
    frame.participantId = entry.desired.sourceId;
    return frame;
  }

  // Audio worker (under coreMutex, cheap): one 20 ms window per LIVE entry
  // that is due.
  std::vector<modules::AudioFrame> popAudio(int64_t nowMs) {
    std::vector<modules::AudioFrame> result;
    std::lock_guard<std::mutex> lock(mutex_);
    for (auto it = audioNextTime_.begin(); it != audioNextTime_.end();) {
      if (!entries_.count(it->first)) it = audioNextTime_.erase(it); else ++it;
    }
    // Every entry keeps a demand clock, wanted or not. Dropping a paused
    // entry's clock and re-anchoring it on resume left `entry.audioNextTime`
    // stale, so the worker stamped its first resumed window at an old target
    // and this loop then discarded it as expired.
    for (const auto& [id, entryPtr] : entries_) {
      auto [clock, inserted] = audioNextTime_.try_emplace(id, nowMs);
      const auto skippedBefore = clock->second.skipped();
      const auto due = clock->second.takeDue(nowMs);
      if (!due) continue;
      const auto target = *due;
      auto& entry = *entryPtr;
      std::lock_guard<std::mutex> entryLock(entry.mutex);
      if (clock->second.skipped() != skippedBefore &&
          (skippedBefore == 0 || clock->second.skipped() / 100 != skippedBefore / 100))
        ::corevideo::core::nativeLogf("[media-playback] audio_windows_expired=%llu source=%s\n",
            static_cast<unsigned long long>(clock->second.skipped()), entry.desired.assetId.c_str());
      entry.wake = true;
      entry.changed.notify_all();
      // No PCM, not even silence, while this clip is not rolling (or while
      // nothing wants its audio) — but the clock still advances, above.
      if (!entryPtr->wantsAudio.load() || entry.state != MediaTransportState::Live || entry.clockFrozen) {
        entry.audioNextTime = clock->second.nextTimeMs();
        continue;
      }
      while (!entry.audio.empty() && entry.audio.front().timestampMs < target) entry.audio.pop_front();
      if (!entry.audio.empty() && entry.audio.front().timestampMs == target) {
        result.push_back(std::move(entry.audio.front())); entry.audio.pop_front();
      } else {
        if (!entry.audioEverProduced) { entry.audioNextTime = clock->second.nextTimeMs(); continue; }
        modules::AudioFrame silence;
        silence.participantId = entry.desired.sourceId;
        silence.sampleRate = 48000; silence.channels = 2; silence.sampleCount = 960; silence.pcm.resize(1920, 0.f);
        result.push_back(std::move(silence));
      }
      entry.audioNextTime = clock->second.nextTimeMs();
      result.back().timestampMs = nowMs;
    }
    changed_.notify_all();
    return result;
  }

  std::vector<std::string> warnings() const {
    std::lock_guard<std::mutex> lock(mutex_);
    auto result = warnings_;
    for (const auto& [id, entry] : entries_) {
      std::lock_guard<std::mutex> entryLock(entry->mutex);
      result.insert(result.end(), entry->warnings.begin(), entry->warnings.end());
    }
    return result;
  }

  std::vector<Status> snapshot() const {
    std::vector<Status> result;
    std::lock_guard<std::mutex> lock(mutex_);
    result.reserve(entries_.size());
    for (const auto& [id, entry] : entries_) {
      std::lock_guard<std::mutex> entryLock(entry->mutex);
      result.push_back(Status{id, entry->desired.assetId, entry->state, entry->desired.loop,
                              entry->desired.onProgram, entry->desired.onPreview,
                              entry->positionMs, entry->durationMs});
    }
    return result;
  }

  // The layer the decoder is handed. Play state == (state == Live), NEVER the
  // render plan's. Caller holds `e.mutex`.
  static modules::CompositorRenderPlanLayer decoderLayerOf(const Entry& e) {
    modules::CompositorRenderPlanLayer l;
    l.kind = e.desired.sourceId.rfind("background:", 0) == 0 ? "media-background" : "media-video";
    l.sourceId = e.desired.sourceId;
    l.mediaAssetId = e.desired.assetId;
    l.mediaAssetPath = e.desired.path;
    l.mediaAssetKind = e.desired.kind;
    l.mediaAssetLoop = e.desired.loop;
    // ENDED IS STILL "PLAYING" TO THE DECODER, deliberately. Ended means the
    // MEDIA ran out, not that an operator stopped it: the decoder must stay in
    // its playing mode (holding its last picture at EOS) rather than fall into
    // the paused/poster branch, and the worker must keep polling it so that a
    // decoder which was merely stalled — an FFmpeg resume, a NAS hiccup — can
    // hand back a new frame and bring the transport back to Live on its own.
    // The picture is held and the audio silenced by `state`, in selectVideo()
    // and popAudio(), not here.
    l.mediaAssetPlaying =
        e.state == MediaTransportState::Live || e.state == MediaTransportState::Ended;
    return l;
  }

 private:
  static constexpr std::size_t kMaxDecoders = 16;
  // ENDED IS TAKEN FROM THE DECODER (IMediaVideoPrefetch::mediaEnded). This
  // window is only a BACKSTOP for a decoder that cannot say, and it is
  // deliberately far longer than anything a healthy clip can hit. It used to
  // be 500 ms, which is shorter than a cold Media Foundation open (95-250 ms)
  // plus an FFmpeg process spawn, shorter than one rung of the FFmpeg
  // resume ladder (250 ms / 500 ms / 1 s / 2 s over 5 attempts, ~3.75 s in
  // total), and shorter than an ordinary hiccup on a loaded box — and the
  // presentation queue is only 3 deep (~50 ms at 60 fps), so `queued() == 0`
  // is the ordinary state of a HEALTHY clip and the guard reduced to "no new
  // frame for 500 ms". On air that froze and silenced a Program clip whose
  // only recovery gesture restarts it from 0.
  static constexpr int64_t kEndedBackstopNoNewFrameMs = 6000;
  // How long a source absent from BOTH desired sets is kept before its decoder
  // is retired. See the Release case in apply() for why this is not zero.
  static constexpr int64_t kReleaseGraceMs = 750;

  // Caller holds mutex_.
  void openLocked(const std::string& id, const MediaTransportDesired& desired, MediaTransportState next,
                  std::vector<std::pair<std::string, std::shared_ptr<Entry>>>& starting,
                  std::vector<Change>& changes) {
    if (entries_.size() + retired_.size() >= kMaxDecoders) {
      warnings_.push_back("Media decoder capacity reached (16 active/retiring assets); not starting " + id + ".");
      // #473. A refusal here looks EXACTLY like a broken decoder from the
      // outside: no frames, a slate, and (before this) not one line in
      // media-core.log. Rate-limited per source so a busy show cannot flood
      // the log with the same refusal every command.
      if (capWarningsLogged_.insert(id).second) {
        ::corevideo::core::nativeLogf(
            "[media-decoder] capacity reached (16 active/retiring); refusing %s\n", id.c_str());
      }
      return;
    }
    auto entry = std::make_shared<Entry>();
    entry->desired = desired;
    entry->state = next;
    const auto known = audioNextTime_.find(id);
    if (known != audioNextTime_.end()) entry->audioNextTime = known->second.nextTimeMs();
    entry->wantsVideo.store(true);
    entry->wantsAudio.store(next == MediaTransportState::Live);
    entries_.emplace(id, entry);
    starting.emplace_back(id, entry);
    changes.push_back({id, true, entry});
  }

  // Caller holds mutex_. The entry is already out of entries_.
  void retireLocked(const std::shared_ptr<Entry>& entry) {
    entry->stop.store(true);
    { std::lock_guard<std::mutex> entryLock(entry->mutex); entry->wake = true; }
    entry->changed.notify_all();
    retired_.push_back(entry);
  }

  void run(const std::shared_ptr<Entry>& entry) {
    try {
      // Factory, reader opens and decoder destruction all occur on this COM
      // owner. MF uses async callbacks, so polls never wait for ReadSample.
      auto decoder = factory_();
      if (!decoder) throw std::runtime_error("Media decoder unavailable.");
      auto* prefetchDecoder = dynamic_cast<modules::IMediaVideoPrefetch*>(decoder.get());
      const std::weak_ptr<Entry> weakEntry = entry;
      const auto attachWake = [&] {
        if (!prefetchDecoder) return;
        prefetchDecoder->setMediaWakeCallback([weakEntry] {
          if (const auto owner = weakEntry.lock()) {
            { std::lock_guard<std::mutex> lock(owner->mutex); owner->wake = true; }
            owner->changed.notify_all();
          }
        });
      };
      attachWake();
      bool haveSyncedPlaying = false, syncedPlaying = false, playedOnce = false;
      // A clip CANNOT reach Ended before it has ever produced a picture: until
      // then "no new frame" is an open that has not finished, never the end of
      // the media. `playedOnce` only says the transport was rolling, which is
      // true the instant it opens.
      bool everProducedFrame = false;
      int64_t lastFrameId = 0, lastNewFrameMs = 0;
      while (!entry->stop.load()) {
        const auto nowMs = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
        if (lastNewFrameMs == 0) lastNewFrameMs = nowMs;
        modules::CompositorRenderPlanLayer layer;
        bool restart = false, resume = false;
        {
          std::lock_guard<std::mutex> lock(entry->mutex);
          restart = std::exchange(entry->restartRequested, false);
          resume = std::exchange(entry->resumeRequested, false);
          layer = decoderLayerOf(*entry);
        }
        if (restart) {
          // RestartCued: a NEW decoder instance at 0. video.current_ (the held
          // frame) survives; the queue was scheduled on the old clock and can
          // never come due on the new one.
          decoder = factory_();
          if (!decoder) throw std::runtime_error("Media decoder unavailable.");
          prefetchDecoder = dynamic_cast<modules::IMediaVideoPrefetch*>(decoder.get());
          attachWake();
          haveSyncedPlaying = false; playedOnce = false; everProducedFrame = false;
          lastFrameId = 0; lastNewFrameMs = nowMs;
          std::lock_guard<std::mutex> lock(entry->mutex);
          entry->video.dropQueued(); entry->clockFrozen = false; entry->audio.clear();
        }
        if (resume) {
          // Cued -> Live (#449 hand-off, now in-place): the poster frames were
          // scheduled on the paused epoch and can never come due on the
          // rolling one. The held poster stays on air while the decoder
          // refills.
          lastNewFrameMs = nowMs;
          std::lock_guard<std::mutex> lock(entry->mutex);
          entry->video.dropQueued(); entry->audio.clear();
        }
        const bool playing = layer.mediaAssetPlaying;
        // PAUSE / RESUME (T1.2). Every transition is carried to the decoder's
        // clock at one instant (nowMs) and the SAME instant is recorded here,
        // so the prepared frames are re-timed by exactly the paused duration
        // the decoder's epoch moves by. One decoder throughout.
        if (!haveSyncedPlaying || playing != syncedPlaying) {
          if (prefetchDecoder) prefetchDecoder->syncMediaClock({layer}, nowMs);
          std::lock_guard<std::mutex> lock(entry->mutex);
          if (!playing && playedOnce && !entry->clockFrozen) {
            entry->clockFrozen = true; entry->frozenAtMs = nowMs;
          } else if (playing && entry->clockFrozen) {
            entry->video.shift((nowMs - entry->frozenAtMs) * 10000);
            entry->clockFrozen = false;
          }
          // THE ENDED WINDOW IS RE-ARMED ON EVERY PLAY/PAUSE TRANSITION. It
          // measures "the decoder has stopped producing", and a clip that was
          // deliberately not being read produced nothing for a reason that is
          // not the end of its media.
          lastNewFrameMs = nowMs;
          haveSyncedPlaying = true; syncedPlaying = playing;
        }
        if (playing) playedOnce = true;
        {
          std::lock_guard<std::mutex> lock(entry->mutex);
          entry->positionMs = prefetchDecoder ? prefetchDecoder->playbackPositionMs() : -1;
          entry->durationMs = prefetchDecoder ? prefetchDecoder->mediaDurationMs() : -1;
        }
        // A paused clip that has rolled holds its on-air frame: prefetch
        // nothing (no video, no audio) until it resumes. A clip that has
        // never played (a cue poster) still decodes and holds one frame.
        bool holding;
        { std::lock_guard<std::mutex> lock(entry->mutex); holding = !playing && playedOnce && entry->video.hasFrame(); }
        if (holding) {
          // Held, so nothing is being read: keep the ended window anchored at
          // now. Without this a pause longer than kEndedAfterNoNewFrameMs made
          // the FIRST resumed iteration — before any frame could arrive — look
          // like the end of the clip, which froze and silenced it ON AIR and
          // made the next operator Play restart from 0.
          lastNewFrameMs = nowMs;
          std::unique_lock<std::mutex> lock(entry->mutex);
          entry->changed.wait_for(lock, std::chrono::milliseconds(20), [&] { return entry->stop.load() || entry->wake; });
          entry->wake = false;
          continue;
        }
        std::vector<modules::ScheduledMediaVideo> video;
        bool videoRoom;
        { std::lock_guard<std::mutex> lock(entry->mutex); videoRoom = entry->video.hasRoom(); }
        if (entry->wantsVideo.load() && videoRoom) {
          if (prefetchDecoder) {
            video = prefetchDecoder->prefetchMediaVideo({layer}, nowMs);
          } else {
            for (auto& frame : decoder->pollMediaFrames({layer}, nowMs)) video.push_back({std::move(frame), nowMs * 10000});
          }
        }
        bool sawNewFrame = false;
        for (const auto& sample : video) {
          if (sample.frame.frameId == lastFrameId) continue;
          lastFrameId = sample.frame.frameId; lastNewFrameMs = nowMs;
          sawNewFrame = true; everProducedFrame = true;
        }
        bool audioRoom; int64_t audioTarget;
        { std::lock_guard<std::mutex> lock(entry->mutex);
          audioRoom = entry->audio.size() < 2; audioTarget = entry->audioNextTime + static_cast<int64_t>(entry->audio.size()) * 20;
        }
        auto audio = playing && entry->wantsAudio.load() && audioRoom ? decoder->pollMediaAudioFrames({layer}, audioTarget) : std::vector<modules::AudioFrame>{};
        auto warnings = decoder->warnings();
        // Read AFTER the poll that could have moved it: the decoder reaches
        // EOS inside prefetchMediaVideo/pollMediaFrames.
        const bool decoderEnded = prefetchDecoder && prefetchDecoder->mediaEnded();
        {
          std::lock_guard<std::mutex> lock(entry->mutex);
          if (!entry->stop.load()) {
            for (auto& frame : video) entry->video.push(std::move(frame));
            if (!audio.empty() && entry->audio.size() < 2 && audioTarget >= entry->audioNextTime) {
              entry->audioEverProduced = true;
              audio.front().timestampMs = audioTarget; entry->audio.push_back(std::move(audio.front()));
            }
            entry->warnings = std::move(warnings);
            // ENDED IS RECOVERABLE, and that is checked FIRST. A decoder that
            // was merely stalled (an FFmpeg resume off the ladder, a NAS
            // hiccup, a loaded box) hands back a new picture when it comes
            // round; the transport goes straight back to Live, keeping its
            // position, with no operator gesture and no restart from 0. Before
            // this, Ended was terminal: `decideMediaTransport` returns None
            // for an unchanged desired row, so a re-sent scene graph recovered
            // nothing and the only exit was an operator Play, which is
            // OpenLive — a frozen, silent clip on air whose recovery gesture
            // restarts it from the top.
            if (entry->state == MediaTransportState::Ended && sawNewFrame) {
              entry->state = MediaTransportState::Live;
              entry->wantsAudio.store(true);
            } else if (playing && !layer.mediaAssetLoop && everProducedFrame &&
                       entry->state == MediaTransportState::Live &&
                       (decoderEnded ||
                        (entry->video.queued() == 0 &&
                         nowMs - lastNewFrameMs >= kEndedBackstopNoNewFrameMs))) {
              // ENDED is bookkeeping, not a teardown: the decoder already holds
              // its last picture at EOS and the worker keeps holding it. The
              // state flip is what snapshot() reports and what makes an
              // operator Play restart from 0 (decideMediaOperator: Ended +
              // Play -> OpenLive) instead of doing nothing. Audio stops here;
              // the picture is held by selectVideo, which never advances a
              // non-Live entry.
              entry->state = MediaTransportState::Ended;
              entry->wantsAudio.store(false);
            }
          }
        }
        if (prefetchDecoder) {
          // Sample completion and consumption both wake this worker. A fixed
          // Sleep(1) can become a 15.6ms quantum on Windows and starve 60fps.
          std::unique_lock<std::mutex> lock(entry->mutex);
          entry->changed.wait_for(lock, std::chrono::milliseconds(20), [&] { return entry->stop.load() || entry->wake; });
          entry->wake = false;
        } else {
          std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
      }
    } catch (const std::exception& error) {
      std::lock_guard<std::mutex> lock(entry->mutex); entry->warnings = {std::string("Media decoder failed: ") + error.what()};
    } catch (...) {
      std::lock_guard<std::mutex> lock(entry->mutex); entry->warnings = {"Media decoder failed with an unknown error."};
    }
    entry->finished.store(true);
  }

  // The reaper. apply() owns the desired-set diff now; this thread only joins
  // workers that have finished, and stops everything at destruction.
  void manage() {
    std::vector<std::shared_ptr<Entry>> joinable;
    for (;;) {
      bool done = false;
      {
        std::unique_lock<std::mutex> lock(mutex_);
        changed_.wait_for(lock, std::chrono::milliseconds(2));
        if (stopped_) {
          for (auto& [id, entry] : entries_) {
            entry->stop.store(true);
            { std::lock_guard<std::mutex> entryLock(entry->mutex); entry->wake = true; }
            entry->changed.notify_all();
            joinable.push_back(entry);
          }
          entries_.clear();
          joinable.insert(joinable.end(), retired_.begin(), retired_.end());
          retired_.clear();
          done = true;
        } else {
          for (auto it = retired_.begin(); it != retired_.end();) {
            if ((*it)->finished.load()) { joinable.push_back(*it); it = retired_.erase(it); } else ++it;
          }
        }
      }
      // Joining happens outside mutex_: a worker's last act can still take its
      // own entry lock, and nothing here may hold the owner lock while it does.
      for (auto& entry : joinable) { if (entry->thread.joinable()) entry->thread.join(); }
      joinable.clear();
      if (done) break;
    }
  }

  DecoderFactory factory_;
  mutable std::mutex mutex_;
  std::condition_variable changed_;
  bool stopped_ = false;
  std::map<std::string, MediaTransportDesired> previousDesired_;
  std::map<std::string, modules::MediaAudioDemandClock> audioNextTime_;
  std::map<std::string, std::shared_ptr<Entry>> entries_;
  // sourceId -> the command instant it went absent from both desired sets.
  std::map<std::string, int64_t> releasePendingNs_;
  std::vector<std::shared_ptr<Entry>> retired_;
  std::vector<std::string> warnings_;
  std::set<std::string> capWarningsLogged_;
  std::thread manager_;
};
}  // namespace corevideo::core
