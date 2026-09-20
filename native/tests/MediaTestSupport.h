#pragma once
#include "core/MediaCore.h"
#include "modules/Interfaces.h"
#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>
namespace corevideo::testing {
template <typename Decoder, typename... Args>
std::function<std::unique_ptr<modules::IMediaFrameSource>()> mediaFactoryOf(Args... args) {
  return [=] { return std::unique_ptr<modules::IMediaFrameSource>(new Decoder(args...)); };
}
// Renders display ticks until `done(core)` is true or `timeoutMs` elapses; returns whether it became true.
// Media frames are produced by a worker thread (exactly as in production), so a test that wants pixels
// after load-scene-graph pumps here instead of assuming the first tick has them.
inline bool renderUntil(core::MediaCore& core, const std::function<bool(core::MediaCore&)>& done, int timeoutMs = 2000) {
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
  while (std::chrono::steady_clock::now() < deadline) {
    core.renderDisplayTick();
    if (done(core)) return true;
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
  }
  return false;
}
// Full (audio-bearing) ticks until `done(core)` is true or `timeoutMs` elapses.
// renderUntil drives VIDEO-only display ticks; media AUDIO is gathered only on
// a full tick, which a direct caller gets from applyCommands with no commands.
// `lastState`, when given, receives the snapshot of the tick that satisfied
// `done` - NOT a later one. Media audio is a window that is only due on some
// ticks, so a caller that re-reads the state after the pump can legitimately
// find the tap empty again; the satisfying tick is the honest answer.
inline bool applyUntil(core::MediaCore& core, const std::function<bool(core::MediaCore&)>& done,
                       int timeoutMs = 3000, rpc::Json* lastState = nullptr) {
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
  while (std::chrono::steady_clock::now() < deadline) {
    auto state = core.applyCommands(rpc::Json::Array{});
    if (done(core)) {
      if (lastState != nullptr) *lastState = std::move(state);
      return true;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
  }
  return false;
}
// Full ticks, unconditionally: for an assertion that something must NOT appear
// (silence on every bus, say), where waiting for a predicate is impossible and
// asserting on tick zero would pass vacuously.
inline void applyTicks(core::MediaCore& core, int ticks) {
  for (int i = 0; i < ticks; ++i) {
    (void)core.applyCommands(rpc::Json::Array{});
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
  }
}
// A decoder that knows nothing about pause: every video poll yields a NEW,
// increasing frameId and every audio poll a non-silent window, so anything
// that holds a picture or silences a clip has to be MediaTransports' doing and
// never the fake's. Ported here from MediaPlaybackTimelineTest.cpp (#535 slice
// 3b Task 5) so the transport unit tests and the MediaCore behaviour tests
// share ONE fake instead of two copies that can drift.
//
// THE BOOK IS STATIC because the module set carries a decoder FACTORY: a test
// never holds the instance it injected, and a restart builds a NEW one. It is
// also the only place a test can read "the frame id on Program" — `sources[]`
// publishes framesIngested (a deduped per-frame COUNT) but not lastFrameId,
// and this task deliberately does not widen the wire to see it.
class CountingDecoder final : public modules::IMediaFrameSource {
 public:
  // maxFrames == 0 means unlimited; a positive value stops producing NEW
  // pictures after that many — the end of the media, with the decoder still
  // alive and still holding its last picture.
  explicit CountingDecoder(int64_t maxFrames = 0) : maxFrames_(maxFrames) { ++created; }

  std::vector<modules::VideoFrame> pollMediaFrames(
      const std::vector<modules::CompositorRenderPlanLayer>& layers, int64_t timestampMs) override {
    if (layers.empty() || layers.front().mediaAssetId.empty()) return {};
    if (maxFrames_ > 0 && frameId_ >= maxFrames_) return {};
    const auto sourceId = sourceIdOf(layers.front());
    modules::VideoFrame frame;
    frame.participantId = sourceId;
    frame.width = frame.pixelWidth = frame.naturalWidth = kWidth;
    frame.height = frame.pixelHeight = frame.naturalHeight = kHeight;
    frame.pixelStride = kWidth * 4;
    frame.timestampMs = timestampMs;
    frame.frameId = ++frameId_;
    auto pixels = std::make_shared<std::vector<std::uint8_t>>(
        static_cast<std::size_t>(kWidth) * static_cast<std::size_t>(kHeight) * 4u, 0x40);
    for (std::size_t i = 3; i < pixels->size(); i += 4) (*pixels)[i] = 0xff;
    frame.pixels = std::move(pixels);
    record(sourceId, frame.frameId);
    return {frame};
  }

  std::vector<modules::AudioFrame> pollMediaAudioFrames(
      const std::vector<modules::CompositorRenderPlanLayer>& layers, int64_t timestampMs) override {
    if (layers.empty() || layers.front().mediaAssetId.empty()) return {};
    modules::AudioFrame frame;
    frame.participantId = sourceIdOf(layers.front());
    frame.sampleRate = 48000;
    frame.channels = 2;
    frame.sampleCount = 960;
    frame.timestampMs = timestampMs;
    frame.voiceActive = true;
    frame.pcm.assign(1920, 0.5f);
    ++audioPolls;
    return {frame};
  }

  std::vector<std::string> warnings() const override { return {}; }

  // Per-source frame-id book. `restarts` counts every id REGRESSION, i.e.
  // every new decoder instance that began at 1 behind an id already past it.
  struct Book {
    std::int64_t last = 0;
    std::int64_t firstAfterRestart = 0;
    int restarts = 0;
  };
  static void resetAll() {
    std::lock_guard<std::mutex> lock(bookMutex());
    books().clear();
    created = 0;
    audioPolls = 0;
  }
  static Book bookFor(const std::string& sourceId) {
    std::lock_guard<std::mutex> lock(bookMutex());
    const auto found = books().find(sourceId);
    return found == books().end() ? Book{} : found->second;
  }
  static std::int64_t lastFrameIdFor(const std::string& sourceId) { return bookFor(sourceId).last; }
  static int restartsFor(const std::string& sourceId) { return bookFor(sourceId).restarts; }
  static std::int64_t firstFrameIdAfterRestartFor(const std::string& sourceId) {
    return bookFor(sourceId).firstAfterRestart;
  }

  static constexpr int kWidth = 16;
  static constexpr int kHeight = 16;
  static inline std::atomic<int> created{0};
  static inline std::atomic<std::int64_t> audioPolls{0};

 private:
  static std::string sourceIdOf(const modules::CompositorRenderPlanLayer& layer) {
    return layer.sourceId.empty() ? "media:" + layer.mediaAssetId : layer.sourceId;
  }
  static std::mutex& bookMutex() {
    static std::mutex mutex;
    return mutex;
  }
  static std::map<std::string, Book>& books() {
    static std::map<std::string, Book> map;
    return map;
  }
  static void record(const std::string& sourceId, std::int64_t frameId) {
    std::lock_guard<std::mutex> lock(bookMutex());
    auto& book = books()[sourceId];
    if (frameId <= book.last) {
      ++book.restarts;
      book.firstAfterRestart = frameId;
    }
    book.last = frameId;
  }
  std::int64_t maxFrames_ = 0;
  std::int64_t frameId_ = 0;
};

// The row for `sourceId` in the snapshot's `sources[]` bus node, or nullptr.
inline const rpc::Json* busSourceRow(const rpc::Json& state, const std::string& sourceId) {
  const auto* sources = state.get("sources");
  if (sources == nullptr) return nullptr;
  for (const auto& row : sources->asArray()) {
    if (row.getString("sourceId") == sourceId) return &row;
  }
  return nullptr;
}
// The bus's DEDUPED per-frame count for `sourceId` (-1 when it is not on the
// bus at all). SourceBus::ingest only bumps this when the frameId actually
// changed, so it is the honest on-the-wire answer to "is the picture Program
// renders advancing?" — a held frame does not move it.
inline std::int64_t busFramesIngested(core::MediaCore& core, const std::string& sourceId) {
  const auto state = core.sessionState();
  const auto* row = busSourceRow(state, sourceId);
  return row == nullptr ? -1 : static_cast<std::int64_t>(row->getNumber("framesIngested"));
}
// True once the source bus reports a frame for `sourceId` (health producing).
inline bool busSourceProducing(core::MediaCore& core, const std::string& sourceId) {
  const auto state = core.sessionState(); const auto* sources = state.get("sources");
  if (!sources) return false;
  for (const auto& s : sources->asArray()) if (s.getString("sourceId") == sourceId) return s.getString("health") == "producing";
  return false;
}
}
