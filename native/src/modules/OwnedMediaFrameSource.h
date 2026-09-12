#pragma once

#include "core/BoundedAsyncLog.h"
#include "modules/Interfaces.h"
#include "modules/MediaCueHandoff.h"
#include "modules/MediaPlaybackTimeline.h"
#include "modules/MediaVideoPresentation.h"
#include "modules/StillMediaFrameCache.h"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <functional>
#include <map>
#include <mutex>
#include <set>
#include <thread>
#include <stdexcept>
#include <cstdio>
#include <vector>

namespace corevideo::modules {
// Each admitted asset owns a decoder thread; one slow source cannot block
// other sources or the render caller. Only the manager creates/joins workers.
class OwnedMediaFrameSource final : public IMediaFrameSource {
 public:
  using Factory = std::function<std::unique_ptr<IMediaFrameSource>()>;
  explicit OwnedMediaFrameSource(Factory factory) : factory_(std::move(factory)), manager_([this] { manage(); }) {}
  ~OwnedMediaFrameSource() override {
    { std::lock_guard<std::mutex> lock(mutex_); stopped_ = true; }
    changed_.notify_all(); manager_.join();
  }
  std::vector<VideoFrame> pollMediaFrames(const std::vector<CompositorRenderPlanLayer>& layers, int64_t timestampMs) override {
    return selectVideo(layers, timestampMs * 10000);
  }
  std::vector<VideoFrame> pollMediaFramesAt100ns(const std::vector<CompositorRenderPlanLayer>& layers, int64_t timestamp100ns) override {
    return selectVideo(layers, timestamp100ns);
  }
 private:
  std::vector<VideoFrame> selectVideo(const std::vector<CompositorRenderPlanLayer>& layers, int64_t timestamp100ns) {
    std::vector<VideoFrame> result;
    std::lock_guard<std::mutex> lock(mutex_);
    videoRequests_ = requests(layers);
    adoptCuedDecoders();
    rebuildCollisionWarnings();
    for (const auto& [id, layer] : videoRequests_) {
      const auto found = entries_.find(id); if (found == entries_.end()) continue;
      std::lock_guard<std::mutex> entryLock(found->second->mutex);
      // PAUSE HOLDS THE ON-AIR FRAME (T1.2). A paused layer never advances
      // the presentation, and neither does a resumed one until its worker
      // has re-timed the frames prepared before the pause (clockFrozen).
      const bool hold = !layer.mediaAssetPlaying || found->second->clockFrozen;
      const auto& selected = hold ? found->second->video.hold() : found->second->video.select(timestamp100ns);
      found->second->wake = true; found->second->changed.notify_all();
      if (selected.hasPixels()) {
        result.push_back(selected); result.back().timestampMs = timestamp100ns / 10000;
        // THE OWNER NAMES THE SOURCE, NOT THE DECODER. Every decoder stamps
        // `participantId` from the layer it was handed, so this is normally a
        // no-op — but an ADOPTED cue's held poster was decoded under the
        // `preview:` id, and the compositor looks a media layer up by the
        // live source id (D3D11CompositorAdapter::resolveLayers). Left alone,
        // the hand-off would deliver a frame nothing could match and Program
        // would paint the very placeholder the hand-off exists to remove.
        result.back().participantId = layer.sourceId.empty() ? "media:" + layer.mediaAssetId : layer.sourceId;
      }
    }
    changed_.notify_all(); return result;
  }
 public:
  std::vector<AudioFrame> pollMediaAudioFrames(const std::vector<CompositorRenderPlanLayer>& layers, int64_t timestampMs) override {
    std::vector<AudioFrame> result;
    std::lock_guard<std::mutex> lock(mutex_);
    audioRequests_ = requests(layers, true);
    adoptCuedDecoders();
    rebuildCollisionWarnings();
    for (auto it = audioNextTime_.begin(); it != audioNextTime_.end();) {
      if (!audioRequests_.count(it->first)) it = audioNextTime_.erase(it); else ++it;
    }
    const auto nowMs = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
    for (const auto& [id, layer] : audioRequests_) {
      auto [clock, inserted] = audioNextTime_.try_emplace(id, nowMs);
      const auto skippedBefore = clock->second.skipped();
      const auto due = clock->second.takeDue(nowMs);
      if (!due) continue;
      const auto target = *due;
      if (clock->second.skipped() != skippedBefore &&
          (skippedBefore == 0 || clock->second.skipped() / 100 != skippedBefore / 100))
        ::corevideo::core::nativeLogf("[media-playback] audio_windows_expired=%llu source=%s\n",
            static_cast<unsigned long long>(clock->second.skipped()), layer.mediaAssetId.c_str());
      const auto found = entries_.find(id); if (found == entries_.end()) continue;
      std::lock_guard<std::mutex> entryLock(found->second->mutex);
      auto& entry = *found->second;
      entry.wake = true; entry.changed.notify_all();
      // The video path has already paused this source (its layer reaches the
      // entry every render tick): no PCM, not even silence, while paused.
      if (!entry.layer.mediaAssetPlaying || entry.clockFrozen) { entry.audioNextTime = clock->second.nextTimeMs(); continue; }
      while (!entry.audio.empty() && entry.audio.front().timestampMs < target) entry.audio.pop_front();
      if (!entry.audio.empty() && entry.audio.front().timestampMs == target) {
        result.push_back(std::move(entry.audio.front())); entry.audio.pop_front();
      } else {
        if (!entry.audioEverProduced) { entry.audioNextTime = clock->second.nextTimeMs(); continue; }
        AudioFrame silence;
        silence.participantId = layer.sourceId.empty() ? "media:" + layer.mediaAssetId : layer.sourceId;
        silence.sampleRate = 48000; silence.channels = 2; silence.sampleCount = 960; silence.pcm.resize(1920, 0.f);
        result.push_back(std::move(silence));
      }
      entry.audioNextTime = clock->second.nextTimeMs();
      result.back().timestampMs = timestampMs;
    }
    changed_.notify_all(); return result;
  }
  std::vector<std::string> warnings() const override {
    std::lock_guard<std::mutex> lock(mutex_);
    auto result = warnings_;
    result.insert(result.end(), collisionWarnings_.begin(), collisionWarnings_.end());
    for (const auto& [id, entry] : entries_) {
      std::lock_guard<std::mutex> entryLock(entry->mutex);
      result.insert(result.end(), entry->warnings.begin(), entry->warnings.end());
    }
    return result;
  }
 private:
  struct Entry {
    CompositorRenderPlanLayer layer; // Guarded by mutex: manage() refreshes it, the worker reads a copy.
    std::mutex mutex;
    std::condition_variable changed;
    bool wake = false;
    // Set by the worker when it freezes the decoder clock for a pause that
    // follows playback; cleared once it has resumed that clock and re-timed
    // the prepared frames by the same paused duration (frozenAtMs -> resume).
    bool clockFrozen = false;
    int64_t frozenAtMs = 0;
    MediaVideoPresentation video;
    std::deque<AudioFrame> audio;
    int64_t audioNextTime = 0;
    bool audioEverProduced = false;
    std::vector<std::string> warnings;
    // Set by the worker the first tick this decoder is actually playing, and
    // never cleared. A cue that has rolled can no longer be handed to Program
    // (MediaCueHandoff): it is at an arbitrary position, not at frame 0.
    std::atomic<bool> everPlayed{false};
    std::atomic<bool> stop{false}, finished{false}, wantsVideo{false}, wantsAudio{false};
    std::thread thread;
  };
  using Requests = std::map<std::string, CompositorRenderPlanLayer>;
  static Requests requests(const std::vector<CompositorRenderPlanLayer>& layers, bool audio = false) {
    Requests result;
    for (const auto& layer : layers) {
      if (layer.mediaAssetId.empty() || layer.mediaAssetPath.empty() ||
          (audio && (layer.kind != "media-video" || !layer.mediaAssetPlaying))) continue;
      // Still-image ROUTE layers are served by MediaCore's StillMediaFrameCache
      // — the same filter MediaFoundationMediaFrameSourceAdapter applies. A
      // still on both buses arrives playing (Program) and paused (Preview);
      // requesting it here would start two dead decoders and call the pair a
      // playback-identity collision. Background stills keep the decoder path.
      if (layer.kind == "media-video" && isStillImageMediaAsset(layer.mediaAssetKind, layer.mediaAssetPath)) continue;
      // Playing/paused is NOT part of the identity (T1.2): a pause must reach
      // the running decoder as state, never open a new one. Restart from the
      // top is a go-live policy carried by a new mediaPlaybackKey.
      const auto id = (layer.sourceId.empty() ? "media:" + layer.mediaAssetId : layer.sourceId) + "|" +
          layer.mediaAssetPath + "|" + layer.mediaAssetId + "|" + layer.mediaPlaybackKey +
          (layer.mediaAssetLoop ? "|loop" : "|once");
      // The FIRST request for a key wins. MediaCore passes Program's layers
      // before Preview's, so a paused copy of the same source on Preview can
      // never pause Program's roll.
      result.emplace(id, layer);
    }
    return result;
  }
  // Called under mutex_ whenever videoRequests_ or audioRequests_ is
  // refreshed. Two DIFFERENT request keys sharing one effective source id
  // mean two decoders would publish under one frame id — loud, not silent.
  // Rebuilt from BOTH maps every time (never cleared by manage(), which owns
  // warnings_ only) so the warning survives until the collision clears.
  void rebuildCollisionWarnings() {
    std::map<std::string, std::set<std::string>> keysBySource;
    auto collect = [&](const Requests& reqs) {
      for (const auto& [key, layer] : reqs) {
        const auto sourceId = layer.sourceId.empty() ? "media:" + layer.mediaAssetId : layer.sourceId;
        keysBySource[sourceId].insert(key);
      }
    };
    collect(videoRequests_); collect(audioRequests_);
    collisionWarnings_.clear();
    for (const auto& [sourceId, keys] : keysBySource) {
      if (keys.size() <= 1) continue;
      collisionWarnings_.push_back("Media source " + sourceId +
          " is requested with two different playback identities; two decoders will publish under one id.");
      if (collisionWarningsLogged_.insert(sourceId).second) {
        ::corevideo::core::nativeLogf(
            "[media-playback] source=%s requested with two different playback identities; two decoders will publish under one id.\n",
            sourceId.c_str());
      }
    }
  }
  void run(const std::shared_ptr<Entry>& entry) {
    try {
      // Factory, reader opens and decoder destruction all occur on this COM
      // owner. MF uses async callbacks, so polls never wait for ReadSample.
      auto decoder = factory_();
      if (!decoder) throw std::runtime_error("Media decoder unavailable.");
      auto* prefetchDecoder = dynamic_cast<IMediaVideoPrefetch*>(decoder.get());
      if (prefetchDecoder) {
        const std::weak_ptr<Entry> weakEntry = entry;
        prefetchDecoder->setMediaWakeCallback([weakEntry] {
          if (const auto owner = weakEntry.lock()) {
            { std::lock_guard<std::mutex> lock(owner->mutex); owner->wake = true; }
            owner->changed.notify_all();
          }
        });
      }
      bool haveSyncedPlaying = false, syncedPlaying = false, playedOnce = false;
      while (!entry->stop.load()) {
        const auto nowMs = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
        CompositorRenderPlanLayer layer;
        { std::lock_guard<std::mutex> lock(entry->mutex); layer = entry->layer; }
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
        if (playing) { playedOnce = true; entry->everPlayed.store(true); }
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
        std::vector<ScheduledMediaVideo> video;
        bool videoRoom;
        { std::lock_guard<std::mutex> lock(entry->mutex); videoRoom = entry->video.hasRoom(); }
        if (entry->wantsVideo.load() && videoRoom) {
          if (prefetchDecoder) {
            video = prefetchDecoder->prefetchMediaVideo({layer}, nowMs);
          } else {
            for (auto& frame : decoder->pollMediaFrames({layer}, nowMs)) video.push_back({std::move(frame), nowMs * 10000});
          }
        }
        bool audioRoom; int64_t audioTarget;
        { std::lock_guard<std::mutex> lock(entry->mutex);
          audioRoom = entry->audio.size() < 2; audioTarget = entry->audioNextTime + static_cast<int64_t>(entry->audio.size()) * 20;
        }
        auto audio = playing && entry->wantsAudio.load() && audioRoom ? decoder->pollMediaAudioFrames({layer}, audioTarget) : std::vector<AudioFrame>{};
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
          }
        }
        if (prefetchDecoder) {
          // Sample completion and consumption both wake this worker. A fixed
          // Sleep(1) can become a15.6ms quantum on Windows and starve60fps.
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
  static MediaSourceRequest requestOf(const CompositorRenderPlanLayer& layer) {
    return MediaSourceRequest{
        layer.sourceId.empty() ? "media:" + layer.mediaAssetId : layer.sourceId,
        layer.mediaAssetId, layer.mediaAssetPath, layer.mediaPlaybackKey, layer.mediaAssetLoop};
  }
  // T1.11 / #449. Re-keys a Preview cue's WARM decoder onto the live request
  // that has just replaced it, instead of letting it retire while a cold one
  // opens and the compositor paints colorFromParticipantId over Program. Entry
  // is a shared_ptr and its worker holds its own reference, so this is a map
  // re-key: the decoder and its held poster never notice.
  //
  // Called under mutex_ from the REQUEST sites (selectVideo /
  // pollMediaAudioFrames), never from manage(). That is deliberate, and it is
  // the difference between one flashed frame and none: the request set changes
  // on the take tick, while manage() is a separate thread on a 2 ms wait, so an
  // adoption deferred to it lands a tick late — at 60 Hz, exactly one frame of
  // the placeholder this exists to remove. Adoption starts no thread and does
  // no I/O, so it is safe on the caller's path in a way creating a worker
  // would not be.
  void adoptCuedDecoders() {
    if (entries_.empty()) return;
    // Candidates are the entries nothing requests any more. manage() has not
    // reaped them yet, and that window is exactly what this runs in.
    std::vector<std::shared_ptr<Entry>> candidates;
    for (const auto& [id, entry] : entries_) {
      if (!videoRequests_.count(id) && !audioRequests_.count(id)) candidates.push_back(entry);
    }
    if (candidates.empty()) return;
    Requests desired = videoRequests_;
    desired.insert(audioRequests_.begin(), audioRequests_.end());
    std::vector<std::pair<std::string, std::shared_ptr<Entry>>> adopted;
    for (const auto& [arrivingId, arrivingLayer] : desired) {
      if (entries_.count(arrivingId)) continue;  // already running: nothing to adopt
      const auto arriving = requestOf(arrivingLayer);
      std::size_t matches = 0, matched = 0;
      for (std::size_t i = 0; i < candidates.size(); ++i) {
        const auto& candidate = candidates[i];
        if (!candidate) continue;
        MediaSourceRequest cue;
        { std::lock_guard<std::mutex> entryLock(candidate->mutex); cue = requestOf(candidate->layer); }
        if (!isCueHandoff(cue, arriving, candidate->everPlayed.load())) continue;
        ++matches; matched = i;
      }
      // NEVER GUESS. Two cues that both claim to be this clip's predecessor is
      // a state we cannot disambiguate, and adopting the wrong one puts the
      // wrong pictures on air. Cold-start instead, and say so.
      if (matches != 1) {
        if (matches > 1) {
          warnings_.push_back("Media source " + arriving.sourceId +
                              " has more than one cued decoder to adopt; starting a fresh one.");
          ::corevideo::core::nativeLogf(
              "[media-playback] source=%s has %zu cued decoders to adopt; refusing the hand-over.\n",
              arriving.sourceId.c_str(), matches);
        }
        continue;
      }
      auto entry = candidates[matched];
      candidates[matched].reset();  // claimed: it cannot be adopted twice
      {
        std::lock_guard<std::mutex> entryLock(entry->mutex);
        entry->layer = arrivingLayer;
        // The queued frames were scheduled against the cue's paused epoch and
        // can never come due on the go-live clock. The held poster stays on
        // air; the decoder refills from the new identity.
        entry->video.dropQueued();
        entry->wake = true;
        entry->changed.notify_all();
      }
      adopted.emplace_back(arrivingId, entry);
      ::corevideo::core::nativeLogf("[media-playback] cue hand-off source=%s key=%s (warm decoder adopted)\n",
                                    arriving.sourceId.c_str(), arriving.mediaPlaybackKey.c_str());
    }
    // Re-key only after the matching pass: erasing from entries_ while the loop
    // above still walks it would invalidate what it is reading.
    for (const auto& [arrivingId, entry] : adopted) {
      for (auto it = entries_.begin(); it != entries_.end();) {
        if (it->second == entry) it = entries_.erase(it); else ++it;
      }
      entries_.emplace(arrivingId, entry);
    }
  }
  void manage() {
    std::vector<std::shared_ptr<Entry>> retired;
    for (;;) {
      std::vector<std::pair<std::string, std::shared_ptr<Entry>>> starting;
      {
        std::unique_lock<std::mutex> lock(mutex_);
        changed_.wait_for(lock, std::chrono::milliseconds(2));
        if (stopped_) {
          for (auto& [id, entry] : entries_) { entry->stop.store(true); entry->changed.notify_all(); retired.push_back(entry); }
          entries_.clear();
          break;
        }
        auto desired = videoRequests_;
        desired.insert(audioRequests_.begin(), audioRequests_.end());
        // A cue whose clip has gone live was already re-keyed onto the live
        // request by adoptCuedDecoders, on the request path, so anything still
        // unwanted here is genuinely dead.
        for (auto it = entries_.begin(); it != entries_.end();) {
          if (!desired.count(it->first)) {
            it->second->stop.store(true); it->second->changed.notify_all(); retired.push_back(it->second); it = entries_.erase(it);
          } else ++it;
        }
        warnings_.clear();
        for (const auto& [id, layer] : desired) {
          auto found = entries_.find(id);
          if (found == entries_.end()) {
            if (entries_.size() + retired.size() >= 16) {
              warnings_.push_back("Media decoder capacity reached (16 active/retiring assets); not starting " +
                                  (layer.sourceId.empty() ? "media:" + layer.mediaAssetId : layer.sourceId) + ".");
              continue;
            }
            auto entry = std::make_shared<Entry>(); entry->layer = layer;
            if (audioNextTime_.count(id)) entry->audioNextTime = audioNextTime_.at(id).nextTimeMs();
            found = entries_.emplace(id, entry).first;
            entry->wantsVideo.store(videoRequests_.count(id) != 0); entry->wantsAudio.store(audioRequests_.count(id) != 0);
            starting.emplace_back(id, entry);
          }
          {
            // An existing entry keeps its decoder; a play/pause change reaches
            // its worker as state (the key no longer carries it), and wakes it.
            auto& entry = *found->second;
            std::lock_guard<std::mutex> entryLock(entry.mutex);
            if (entry.layer.mediaAssetPlaying != layer.mediaAssetPlaying) {
              entry.layer = layer; entry.wake = true; entry.changed.notify_all();
            }
          }
          found->second->wantsVideo.store(videoRequests_.count(id) != 0);
          const bool audioWanted = audioRequests_.count(id) != 0;
          if (audioWanted && !found->second->wantsAudio.load()) {
            std::lock_guard<std::mutex> entryLock(found->second->mutex);
            found->second->audio.clear(); found->second->audioNextTime = audioNextTime_.at(id).nextTimeMs();
          }
          found->second->wantsAudio.store(audioWanted);
        }
      }
      for (auto& [id, entry] : starting) {
        try { entry->thread = std::thread([this, entry] { run(entry); }); }
        catch (const std::exception& error) {
          std::lock_guard<std::mutex> lock(mutex_);
          entries_.erase(id); warnings_.push_back(std::string("Media worker could not start: ") + error.what());
        }
      }
      for (auto it = retired.begin(); it != retired.end();) {
        if ((*it)->finished.load()) { (*it)->thread.join(); it = retired.erase(it); } else ++it;
      }
    }
    // Outside the render/core lock. Async sample callbacks own only their
    // independent result slot, and cannot publish into retired entries.
    for (auto& entry : retired) { entry->stop.store(true); entry->changed.notify_all(); if (entry->thread.joinable()) entry->thread.join(); }
  }
  Factory factory_;
  mutable std::mutex mutex_;
  std::condition_variable changed_;
  bool stopped_ = false;
  Requests videoRequests_, audioRequests_;
  std::map<std::string, MediaAudioDemandClock> audioNextTime_;
  std::map<std::string, std::shared_ptr<Entry>> entries_;
  std::vector<std::string> warnings_;
  std::vector<std::string> collisionWarnings_;
  std::set<std::string> collisionWarningsLogged_;
  std::thread manager_;
};
} // namespace corevideo::modules
