// native/src/core/MediaAssetSource.h
#pragma once
#include "core/SourceBus.h"

namespace corevideo::core {

// One media asset (a decoded clip/loop/background from IMediaFrameSource, or a
// route still from StillMediaFrameCache) on the source bus (#535 slice 3a). The
// OWNER holds the frame: for "media" it is the media decoder, which emits a
// frame for every requested key each tick (a paused clip keeps emitting its
// held frame); for "still" it is the still cache, which re-serves its decoded
// frame every tick while the still is requested. This source only mirrors the
// frame MediaCore's media/still poll handed it this tick. Keyed by the owner's
// own "media:<assetId>" / "preview:media:<assetId>" / "background:<assetId>"
// frame key — never re-keyed.
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

  SourceTick poll(int64_t) override {
    SourceTick tick;
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
  modules::VideoFrame latest_;
  bool hasFrame_ = false;
  SourceIngestCounters counters_;
};

}  // namespace corevideo::core
