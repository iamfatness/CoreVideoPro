// native/src/core/MediaAssetSource.h
#pragma once
#include "core/MediaTransports.h"
#include "core/SourceBus.h"

#include <memory>
#include <optional>
#include <string>
#include <utility>

namespace corevideo::core {

// One media asset on the source bus. Two shapes, by kind:
//
//  - "media" (#535 slice 3b) is COMMAND-DRIVEN: the source holds a
//    core::MediaTransports::Entry — one decoder, one clock, one transport
//    state, created and retired by MediaTransports::apply() at command time —
//    and poll(ts) asks it for the frame due at ts (or its held frame).
//    Nothing pushes frames into it.
//  - "still" MIRRORS the cache: StillMediaFrameCache re-serves its decoded
//    frame every tick while the still is requested, and MediaCore's still
//    poll pushes it here with setLatest().
//
// Keyed by the owner's own "media:<assetId>" / "background:<assetId>" frame
// key — never re-keyed.
class MediaAssetSource final : public ISource {
 public:
  MediaAssetSource(std::string sourceId, std::string kind, int width, int height) {
    descriptor_.sourceId = std::move(sourceId);
    descriptor_.kind = std::move(kind);
    descriptor_.width = width;
    descriptor_.height = height;
    descriptor_.pixelFormat = "bgra";
    descriptor_.hasVideo = true;
  }
  // kind "media": the transport entry IS the frame source (no setLatest path).
  // Width/height are unknown until the first decoded frame and are filled in
  // then — a decoder cannot be asked for its dimensions before it has one.
  MediaAssetSource(std::string sourceId, std::shared_ptr<MediaTransports::Entry> entry)
      : entry_(std::move(entry)) {
    descriptor_.sourceId = std::move(sourceId);
    descriptor_.kind = "media";
    descriptor_.pixelFormat = "bgra";
    descriptor_.hasVideo = true;
  }
  const SourceDescriptor& descriptor() const override { return descriptor_; }

  void setLatest(modules::VideoFrame frame) {
    latest_ = std::move(frame);  // shared_ptr payload: no pixel copy
    hasFrame_ = true;
    // diagnostic only: counts setLatest calls, NOT the deduped per-frame
    // count the snapshot publishes (SourceBus::Entry::counters) — the owner
    // re-emits the requested key's held frame every tick it is still
    // requested, so this increments once per tick regardless of whether the
    // frameId actually advanced.
    counters_.framesIngested += 1;
    counters_.lastFrameId = latest_.frameId;
  }

  SourceTick poll(int64_t timestamp100ns) override {
    SourceTick tick;
    if (entry_) {
      auto frame = MediaTransports::selectVideo(*entry_, timestamp100ns);
      if (!frame) {
        tick.health = SourceHealth::Warming;
        return tick;
      }
      if (descriptor_.width <= 0 || descriptor_.height <= 0) {
        descriptor_.width = frame->pixelWidth > 0 ? frame->pixelWidth : frame->i420Width;
        descriptor_.height = frame->pixelHeight > 0 ? frame->pixelHeight : frame->i420Height;
      }
      counters_.framesIngested += 1;
      counters_.lastFrameId = frame->frameId;
      tick.video.push_back(std::move(*frame));
      tick.health = SourceHealth::Producing;
      return tick;
    }
    if (hasFrame_) {
      tick.video.push_back(latest_);
      tick.health = SourceHealth::Producing;
    } else {
      tick.health = SourceHealth::Warming;
    }
    return tick;
  }
  SourceIngestCounters counters() const override { return counters_; }

 private:
  SourceDescriptor descriptor_;
  std::shared_ptr<MediaTransports::Entry> entry_;
  modules::VideoFrame latest_;
  bool hasFrame_ = false;
  SourceIngestCounters counters_;
};

}  // namespace corevideo::core
