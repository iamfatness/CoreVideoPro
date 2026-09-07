#pragma once
#include "modules/Interfaces.h"
#include "modules/MediaPlaybackTimeline.h"
#include "modules/MediaVideoPresentation.h"
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <functional>
#include <map>
#include <mutex>
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
    for (const auto& [id, layer] : videoRequests_) {
      const auto found = entries_.find(id); if (found == entries_.end()) continue;
      std::lock_guard<std::mutex> entryLock(found->second->mutex);
      const auto& selected = found->second->video.select(timestamp100ns);
      found->second->wake = true; found->second->changed.notify_all();
      if (selected.hasPixels()) {
        result.push_back(selected); result.back().timestampMs = timestamp100ns / 10000;
      }
    }
    changed_.notify_all(); return result;
  }
 public:
  std::vector<AudioFrame> pollMediaAudioFrames(const std::vector<CompositorRenderPlanLayer>& layers, int64_t timestampMs) override {
    std::vector<AudioFrame> result;
    std::lock_guard<std::mutex> lock(mutex_);
    audioRequests_ = requests(layers, true);
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
        std::fprintf(stderr, "[media-playback] audio_windows_expired=%llu source=%s\n",
            static_cast<unsigned long long>(clock->second.skipped()), layer.mediaAssetId.c_str());
      const auto found = entries_.find(id); if (found == entries_.end()) continue;
      std::lock_guard<std::mutex> entryLock(found->second->mutex);
      auto& entry = *found->second;
      entry.wake = true; entry.changed.notify_all();
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
    for (const auto& [id, entry] : entries_) {
      std::lock_guard<std::mutex> entryLock(entry->mutex);
      result.insert(result.end(), entry->warnings.begin(), entry->warnings.end());
    }
    return result;
  }
 private:
  struct Entry {
    CompositorRenderPlanLayer layer;
    std::mutex mutex;
    std::condition_variable changed;
    bool wake = false;
    MediaVideoPresentation video;
    std::deque<AudioFrame> audio;
    int64_t audioNextTime = 0;
    bool audioEverProduced = false;
    std::vector<std::string> warnings;
    std::atomic<bool> stop{false}, finished{false}, wantsVideo{false}, wantsAudio{false};
    std::thread thread;
  };
  using Requests = std::map<std::string, CompositorRenderPlanLayer>;
  static Requests requests(const std::vector<CompositorRenderPlanLayer>& layers, bool audio = false) {
    Requests result;
    for (const auto& layer : layers) {
      if (layer.mediaAssetId.empty() || layer.mediaAssetPath.empty() ||
          (audio && (layer.kind != "media-video" || !layer.mediaAssetPlaying))) continue;
      const auto id = (layer.sourceId.empty() ? "media:" + layer.mediaAssetId : layer.sourceId) + "|" +
          layer.mediaAssetPath + "|" + layer.mediaAssetId + "|" + layer.mediaPlaybackKey + (layer.mediaAssetPlaying ? "|playing" : "|paused") +
          (layer.mediaAssetLoop ? "|loop" : "|once");
      result[id] = layer;
    }
    return result;
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
      while (!entry->stop.load()) {
        const auto nowMs = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
        std::vector<ScheduledMediaVideo> video;
        bool videoRoom;
        { std::lock_guard<std::mutex> lock(entry->mutex); videoRoom = entry->video.hasRoom(); }
        if (entry->wantsVideo.load() && videoRoom) {
          if (auto* prefetch = dynamic_cast<IMediaVideoPrefetch*>(decoder.get())) {
            video = prefetch->prefetchMediaVideo({entry->layer}, nowMs);
          } else {
            for (auto& frame : decoder->pollMediaFrames({entry->layer}, nowMs)) video.push_back({std::move(frame), nowMs * 10000});
          }
        }
        bool audioRoom; int64_t audioTarget;
        { std::lock_guard<std::mutex> lock(entry->mutex);
          audioRoom = entry->audio.size() < 2; audioTarget = entry->audioNextTime + static_cast<int64_t>(entry->audio.size()) * 20;
        }
        auto audio = entry->wantsAudio.load() && audioRoom ? decoder->pollMediaAudioFrames({entry->layer}, audioTarget) : std::vector<AudioFrame>{};
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
        for (auto it = entries_.begin(); it != entries_.end();) {
          if (!desired.count(it->first)) {
            it->second->stop.store(true); it->second->changed.notify_all(); retired.push_back(it->second); it = entries_.erase(it);
          } else ++it;
        }
        warnings_.clear();
        for (const auto& [id, layer] : desired) {
          auto found = entries_.find(id);
          if (found == entries_.end()) {
            if (entries_.size() + retired.size() >= 16) { warnings_.push_back("Media decoder capacity reached (16 active/retiring assets)."); continue; }
            auto entry = std::make_shared<Entry>(); entry->layer = layer;
            if (audioNextTime_.count(id)) entry->audioNextTime = audioNextTime_.at(id).nextTimeMs();
            found = entries_.emplace(id, entry).first;
            entry->wantsVideo.store(videoRequests_.count(id) != 0); entry->wantsAudio.store(audioRequests_.count(id) != 0);
            starting.emplace_back(id, entry);
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
  std::thread manager_;
};
} // namespace corevideo::modules
