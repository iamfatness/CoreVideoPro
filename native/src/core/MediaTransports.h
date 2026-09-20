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
    // Published by the worker for snapshot(); -1 when the decoder cannot say.
    int64_t durationMs = 0, positionMs = 0;
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
    // Reserved: the caller's command instant. Every clock in here is the
    // worker's own steady_clock, so nothing reads it yet — it stays in the
    // signature because Task 3's MediaCore has it and a later command-time
    // decision (a go-live instant, say) belongs here rather than in a second
    // parameter added under pressure.
    (void)nowNs;
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
        const auto decision = decideMediaTransport(previous, curr, state);
        switch (decision.action) {
          case MediaTransportAction::None:
            if (found != entries_.end() && curr) {
              std::lock_guard<std::mutex> entryLock(found->second->mutex);
              found->second->desired = *curr;  // bus flags may have changed
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
            if (found != entries_.end()) {
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

  // Operator pause/play on `media:<assetId>`; false + reason when refused.
  bool operatorAction(const std::string& assetId, MediaOperatorAction action, std::string& reason) {
    std::lock_guard<std::mutex> lock(mutex_);
    for (const auto& [id, entry] : entries_) {
      MediaTransportDesired desired;
      MediaTransportState state;
      { std::lock_guard<std::mutex> entryLock(entry->mutex); desired = entry->desired; state = entry->state; }
      if (desired.assetId != assetId) continue;
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
      const auto found = entries_.find(it->first);
      if (found == entries_.end() || !found->second->wantsAudio.load()) it = audioNextTime_.erase(it); else ++it;
    }
    for (const auto& [id, entryPtr] : entries_) {
      if (!entryPtr->wantsAudio.load()) continue;
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
      // No PCM, not even silence, while this clip is not rolling.
      if (entry.state != MediaTransportState::Live || entry.clockFrozen) {
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
    l.mediaAssetPlaying = e.state == MediaTransportState::Live;
    return l;
  }

 private:
  static constexpr std::size_t kMaxDecoders = 16;
  // A non-loop clip that stops producing new pictures for this long while Live
  // with nothing queued has reached the end of its media.
  static constexpr int64_t kEndedAfterNoNewFrameMs = 500;

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
          haveSyncedPlaying = false; playedOnce = false;
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
        for (const auto& sample : video) {
          if (sample.frame.frameId == lastFrameId) continue;
          lastFrameId = sample.frame.frameId; lastNewFrameMs = nowMs;
        }
        bool audioRoom; int64_t audioTarget;
        { std::lock_guard<std::mutex> lock(entry->mutex);
          audioRoom = entry->audio.size() < 2; audioTarget = entry->audioNextTime + static_cast<int64_t>(entry->audio.size()) * 20;
        }
        auto audio = playing && entry->wantsAudio.load() && audioRoom ? decoder->pollMediaAudioFrames({layer}, audioTarget) : std::vector<modules::AudioFrame>{};
        auto warnings = decoder->warnings();
        {
          std::lock_guard<std::mutex> lock(entry->mutex);
          if (!entry->stop.load()) {
            for (auto& frame : video) entry->video.push(std::move(frame));
            if (!audio.empty() && entry->audio.size() < 2 && audioTarget >= entry->audioNextTime) {
              entry->audioEverProduced = true;
              audio.front().timestampMs = audioTarget; entry->audio.push_back(std::move(audio.front()));
            }
            entry->warnings = std::move(warnings);
            // ENDED is bookkeeping, not a teardown: the decoder already holds
            // its last picture at EOS and the worker keeps holding it. The
            // state flip is what snapshot() reports and what makes an operator
            // Play restart from 0 (decideMediaOperator: Ended + Play ->
            // OpenLive) instead of doing nothing.
            if (playing && !layer.mediaAssetLoop && entry->state == MediaTransportState::Live &&
                entry->video.queued() == 0 && nowMs - lastNewFrameMs >= kEndedAfterNoNewFrameMs) {
              entry->state = MediaTransportState::Ended;
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
  std::vector<std::shared_ptr<Entry>> retired_;
  std::vector<std::string> warnings_;
  std::set<std::string> capWarningsLogged_;
  std::thread manager_;
};
}  // namespace corevideo::core
