// The TalkbackSdk the macOS engine hands TalkbackChannels: a decorator over
// zc::TalkbackSdkMac that can move SendAudio onto the main queue.
//
// Why it exists: the macOS SDK headers say nothing about which thread may call
// -[ZoomSDKTalkbackController sendAudioDataToChannel:] (the ported adapter's
// header says so too). Membership calls already run on the main queue, as
// every Zoom SDK call in main-macos.mm does. The 20 ms TX pacer runs on its
// own thread. If the SDK tolerates the pacer calling send directly, Direct
// mode costs nothing; if it does not, MainQueue mode copies each frame and
// posts it. Which one ships is decided by MEASUREMENT in a live meeting
// (plan Task 5), and COREVIDEO_COMMS_SEND_MODE=direct|main stays as a dev
// switch either way.
//
// Every call except SendAudio forwards unchanged on the caller's thread: the
// glue already calls those on the main queue, and hopping them would reorder
// them against the bank's own bookkeeping.
//
// Lifetime: a posted send captures the shared State, never `this`, and the
// destructor calls Shutdown(), so a send still in the queue when the
// decorator goes away is dropped rather than forwarded to an `inner` that may
// be gone. After Shutdown() no QUEUED send is ever forwarded (dropped and
// counted): the posted block and Shutdown() both run on the main queue, so
// that is exact. Shutdown() is NOT a barrier for a Direct-mode send already
// inside the SDK on the pacer thread; the barrier there is the very next step
// of end_meeting, CommsCore::DetachBank -> CommsBankSink::Attach(nullptr),
// which returns only after any Send in flight has finished (comms_core.h).
// Holding a lock of our own across the SDK call instead would deadlock the
// main queue if the SDK's send turns out to hop to main itself, which is the
// unknown the live spike measures. One decorator per meeting: Shutdown() is a
// one-way latch.
#pragma once

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

#include "comms/talkback_sdk.h"

namespace engine_comms {

enum class SendMode { Direct, MainQueue };

// The shipped default until the live spike decides (plan Task 5 updates this
// and MainQueueTalkbackSdkTest.SendModeFromEnv in the same commit).
inline constexpr SendMode kDefaultSendMode = SendMode::Direct;

// "main" -> MainQueue, "direct" -> Direct; anything else (null included) is
// the shipped default.
inline SendMode sendModeFromEnv(const char* value) {
  if (value && std::strcmp(value, "main") == 0) return SendMode::MainQueue;
  if (value && std::strcmp(value, "direct") == 0) return SendMode::Direct;
  return kDefaultSendMode;
}

class MainQueueTalkbackSdk : public zc::TalkbackSdk {
 public:
  // Post-to-run latency of MainQueue sends, in microseconds, for the
  // comms_send_hop breadcrumb. p99 is over the last kWindow samples.
  struct HopStats {
    uint64_t count = 0;
    int64_t maxUs = 0;
    int64_t p99Us = 0;
    uint64_t droppedAfterShutdown = 0;
  };
  static constexpr size_t kWindow = 512;

  MainQueueTalkbackSdk(zc::TalkbackSdk* inner, SendMode mode,
                       std::function<void(std::function<void()>)> post,
                       std::function<int64_t()> nowUs)
      : state_(std::make_shared<State>()), mode_(mode), post_(std::move(post)) {
    state_->inner = inner;
    state_->nowUs = std::move(nowUs);
  }

  ~MainQueueTalkbackSdk() override { Shutdown(); }
  MainQueueTalkbackSdk(MainQueueTalkbackSdk&&) = default;

  SendMode mode() const { return mode_; }

  void SetEvents(zc::TalkbackSdkEvents* events) override { state_->inner->SetEvents(events); }
  bool MeetingSupportsTalkback() override { return state_->inner->MeetingSupportsTalkback(); }
  zc::TalkbackResult CreateChannels(unsigned int count) override {
    return state_->inner->CreateChannels(count);
  }
  zc::TalkbackResult InviteUsers(const std::string& channel_id,
                                 const std::vector<unsigned int>& user_ids) override {
    return state_->inner->InviteUsers(channel_id, user_ids);
  }
  zc::TalkbackResult RemoveUsers(const std::string& channel_id,
                                 const std::vector<unsigned int>& user_ids) override {
    return state_->inner->RemoveUsers(channel_id, user_ids);
  }
  zc::TalkbackResult DestroyChannels(const std::vector<std::string>& channel_ids) override {
    return state_->inner->DestroyChannels(channel_ids);
  }
  zc::TalkbackResult SetChannelBackgroundVolume(const std::string& channel_id,
                                                float volume) override {
    return state_->inner->SetChannelBackgroundVolume(channel_id, volume);
  }

  zc::TalkbackResult SendAudio(const std::string& channel_id, const int16_t* pcm,
                               int samples) override {
    if (mode_ == SendMode::Direct) {
      {
        std::lock_guard<std::mutex> lk(state_->m);
        if (state_->shutdown) return zc::TalkbackCall::NoController;
      }
      return state_->inner->SendAudio(channel_id, pcm, samples);
    }
    {
      std::lock_guard<std::mutex> lk(state_->m);
      if (state_->shutdown) return zc::TalkbackCall::NoController;
    }
    // The caller reuses its buffer the moment we return: copy, never alias.
    std::vector<int16_t> frame(pcm, pcm + std::max(samples, 0));
    const int64_t postedUs = state_->nowUs();
    std::shared_ptr<State> st = state_;
    post_([st, channel_id, frame = std::move(frame), postedUs]() {
      {
        std::lock_guard<std::mutex> lk(st->m);
        if (st->shutdown) { ++st->droppedAfterShutdown; return; }
        st->recordHop(st->nowUs() - postedUs);
      }
      st->inner->SendAudio(channel_id, frame.data(), static_cast<int>(frame.size()));
    });
    // The SDK's verdict arrives later on the main queue; the pacer counts this
    // as accepted, which is the same thing Direct mode's caller sees from a
    // fire-and-forget SDK call.
    return zc::TalkbackCall::Ok;
  }

  // Returns and resets the window.
  HopStats TakeHopStats() {
    std::lock_guard<std::mutex> lk(state_->m);
    HopStats s;
    s.count = state_->count;
    s.maxUs = state_->maxUs;
    s.droppedAfterShutdown = state_->droppedAfterShutdown;
    if (!state_->samples.empty()) {
      std::vector<int64_t> v = state_->samples;
      std::sort(v.begin(), v.end());
      size_t idx = (v.size() * 99 + 99) / 100;  // ceil(0.99 * n)
      if (idx == 0) idx = 1;
      s.p99Us = v[std::min(idx, v.size()) - 1];
    }
    state_->count = 0;
    state_->maxUs = 0;
    state_->samples.clear();
    state_->next = 0;
    return s;
  }

  void Shutdown() {
    if (!state_) return;  // moved-from
    std::lock_guard<std::mutex> lk(state_->m);
    state_->shutdown = true;
  }

 private:
  struct State {
    zc::TalkbackSdk* inner = nullptr;
    std::function<int64_t()> nowUs;
    std::mutex m;
    bool shutdown = false;
    uint64_t count = 0;
    int64_t maxUs = 0;
    uint64_t droppedAfterShutdown = 0;
    std::vector<int64_t> samples;  // ring of the last kWindow hops
    size_t next = 0;
    void recordHop(int64_t us) {
      ++count;
      maxUs = std::max(maxUs, us);
      if (samples.size() < kWindow) samples.push_back(us);
      else samples[next] = us;
      next = (next + 1) % kWindow;
    }
  };

  std::shared_ptr<State> state_;
  SendMode mode_;
  std::function<void(std::function<void()>)> post_;
};

}  // namespace engine_comms
