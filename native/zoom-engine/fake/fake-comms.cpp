#include "fake-comms.h"

#include "engine-writer.h"

#include "comms_core.h"
#include "comms_protocol.h"
#include "talkback_channels.h"
#include "talkback_sdk.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <deque>
#include <memory>
#include <mutex>
#include <thread>

namespace fakecomms {
namespace {

int64_t NowMs() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(
             std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

// Zoom's talkback controller, scripted. Every call is accepted; answers
// arrive as callbacks after a delay, delivered by Pump() on the comms thread.
class ScriptedTalkback : public zc::TalkbackSdk {
 public:
  ScriptedTalkback(uint32_t web_client_id, bool supported)
      : web_client_id_(web_client_id), supported_(supported) {}

  void SetEvents(zc::TalkbackSdkEvents* events) override { events_ = events; }
  bool MeetingSupportsTalkback() override { return supported_; }

  zc::TalkbackResult CreateChannels(unsigned int count) override {
    const int64_t now = NowMs();
    for (unsigned int i = 0; i < count; ++i) {
      const std::string id = "fake-ch-" + std::to_string(++created_) + "-0000-0000-000000000000";
      Queue(now + 1200 + 40 * static_cast<int64_t>(i), [this, id] {
        if (events_) events_->OnCreateChannelResponse(id, zc::TalkbackEvent::Ok);
      });
    }
    return zc::TalkbackCall::Ok;
  }

  zc::TalkbackResult InviteUsers(const std::string& channel_id,
                                 const std::vector<unsigned int>& user_ids) override {
    // The web client cannot receive talkback: Zoom refuses the invite with
    // SDKERR_INVALID_PARAMETER (3), live-observed by ZComms.
    for (const unsigned int uid : user_ids)
      if (uid == web_client_id_) return zc::TalkbackResult(zc::TalkbackCall::Failed, 3);
    const int64_t now = NowMs();
    for (const unsigned int uid : user_ids) {
      Queue(now + 400, [this, channel_id, uid] {
        if (events_) events_->OnChannelUserJoinResponse(channel_id, uid, zc::TalkbackEvent::Ok);
      });
    }
    return zc::TalkbackCall::Ok;
  }

  zc::TalkbackResult RemoveUsers(const std::string& channel_id,
                                 const std::vector<unsigned int>& user_ids) override {
    const int64_t now = NowMs();
    for (const unsigned int uid : user_ids) {
      Queue(now + 400, [this, channel_id, uid] {
        if (events_) events_->OnChannelUserLeaveResponse(channel_id, uid, zc::TalkbackEvent::Ok);
      });
    }
    return zc::TalkbackCall::Ok;
  }

  zc::TalkbackResult DestroyChannels(const std::vector<std::string>&) override { return zc::TalkbackCall::Ok; }

  // TX pacer thread: wait-free, never touches the queue.
  zc::TalkbackResult SendAudio(const std::string&, const int16_t*, int) override {
    sends_.fetch_add(1);
    return zc::TalkbackCall::Ok;
  }

  zc::TalkbackResult SetChannelBackgroundVolume(const std::string&, float) override {
    return zc::TalkbackCall::Ok;
  }

  void Pump(int64_t now) {
    std::vector<std::function<void()>> due;
    {
      std::lock_guard<std::mutex> lock(q_m_);
      for (auto it = queue_.begin(); it != queue_.end();) {
        if (it->first <= now) {
          due.push_back(std::move(it->second));
          it = queue_.erase(it);
        } else {
          ++it;
        }
      }
    }
    for (auto& f : due) f();
  }

 private:
  void Queue(int64_t at, std::function<void()> f) {
    std::lock_guard<std::mutex> lock(q_m_);
    queue_.emplace_back(at, std::move(f));
  }

  uint32_t web_client_id_;
  bool supported_;
  zc::TalkbackSdkEvents* events_ = nullptr;
  int created_ = 0;
  std::atomic<uint64_t> sends_{0};
  std::mutex q_m_;
  std::deque<std::pair<int64_t, std::function<void()>>> queue_;
};

// The mic chain, synthesized: 50 Hz frames into the same bank sink the real
// TX pacer feeds. Keyed: a -12 dBFS 300 Hz "voice"; TONE: the 700 Hz test
// tone at -12 dBFS (ZComms' level); otherwise silence. Frames are handed over
// only while the sink can send, exactly like the pacer's gated ticks.
class SyntheticMic : public edgecomms::CommsAudioPort {
 public:
  explicit SyntheticMic(zc::FrameSink* sink) : sink_(sink) {
    thread_ = std::thread([this] { Run(); });
  }
  ~SyntheticMic() override {
    quit_.store(true);
    if (thread_.joinable()) thread_.join();
  }

  void SetRunning(bool on) override { running_.store(on); }
  void SetTalk(bool on) override { talk_.store(on); }
  void SetMic(const std::string& d) override {
    std::lock_guard<std::mutex> lock(m_);
    mic_ = d;
  }
  void SetSidetoneOut(const std::string& d) override {
    std::lock_guard<std::mutex> lock(m_);
    out_ = d;
  }
  void SetSidetone(bool on) override { sidetone_.store(on); }
  void SetTone(bool on) override { tone_.store(on); }
  void SetGain(int db) override { gain_.store(db); }

  edgecomms::CommsAudioStatus Status() override {
    edgecomms::CommsAudioStatus st;
    std::lock_guard<std::mutex> lock(m_);
    st.devices = Mics();
    st.sidetone_devices = Outs();
    const std::string mic = Match(Mics(), mic_);
    const std::string out = Match(Outs(), out_);
    st.device = mic.empty() ? mic_ : mic;
    st.sidetone_device = out.empty() ? out_ : out;
    if (mic.empty()) st.error = "no capture device matches '" + mic_ + "'";
    st.running = running_.load() && !mic.empty();
    if (st.running) st.peak_dbfs = peak_dbfs_.load();
    st.gain_db = gain_.load();
    st.sidetone = sidetone_.load();
    st.tone = tone_.load();
    st.frames_sent = frames_.load();
    st.underruns = 0;
    return st;
  }

 private:
  static std::vector<std::string> Mics() { return {"Fake Mic (USB Audio)", "Fake Headset Mic"}; }
  static std::vector<std::string> Outs() { return {"Fake Speakers", "Fake Headphones"}; }
  // miniaudio-style pick: "" = the default (first); otherwise a
  // case-insensitive substring; no match = no device.
  static std::string Match(const std::vector<std::string>& list, const std::string& want) {
    if (want.empty()) return list.front();
    auto lower = [](std::string s) {
      std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
      return s;
    };
    for (const auto& d : list)
      if (lower(d).find(lower(want)) != std::string::npos) return d;
    return {};
  }

  void Run() {
    using clock = std::chrono::steady_clock;
    auto next = clock::now();
    double phase = 0.0;
    int16_t frame[960];
    uint64_t n = 0;
    while (!quit_.load()) {
      next += std::chrono::milliseconds(20);
      std::this_thread::sleep_until(next);
      bool device_ok;
      {
        std::lock_guard<std::mutex> lock(m_);
        device_ok = !Match(Mics(), mic_).empty();
      }
      if (!running_.load() || !device_ok) continue;
      const bool tone = tone_.load();
      const bool talk = talk_.load();
      const double hz = tone ? 700.0 : 300.0;
      for (int i = 0; i < 960; ++i) {
        const double v = (talk ? 0.25 * std::sin(phase) : 0.0);
        frame[i] = static_cast<int16_t>(std::lround(v * 32767.0));
        phase += 6.283185307179586 * hz / 48000.0;
        if (phase > 6.283185307179586) phase -= 6.283185307179586;
      }
      // The capture meter (pre-envelope, like the real chain): moves while
      // keyed or toned, a measured quiet room otherwise.
      ++n;
      if (talk || tone) peak_dbfs_.store(-14.0 - 6.0 * std::fabs(std::sin(static_cast<double>(n) * 0.13)));
      else peak_dbfs_.store(-60.0);
      if (!sink_->CanSend()) continue;  // a gated tick, not a send
      sink_->Send(frame, 960);
      frames_.fetch_add(1);
    }
  }

  zc::FrameSink* sink_;
  std::thread thread_;
  std::atomic<bool> quit_{false};
  std::atomic<bool> running_{false};
  std::atomic<bool> talk_{false};
  std::atomic<bool> sidetone_{true};
  std::atomic<bool> tone_{false};
  std::atomic<int> gain_{0};
  std::atomic<double> peak_dbfs_{-60.0};
  std::atomic<uint64_t> frames_{0};
  std::mutex m_;
  std::string mic_, out_;
};

struct State {
  std::mutex m;
  FakeRosterSource source;
  uint32_t web_client_id = 0;
  bool supported = true;
  bool host = true;
  edgecomms::CommsBankSink sink;
  std::unique_ptr<SyntheticMic> mic;
  std::unique_ptr<edgecomms::CommsCore> core;
  std::unique_ptr<ScriptedTalkback> sdk;
  std::unique_ptr<zc::TalkbackChannels> bank;
  std::string station = "CoreVideo Edge";
  edgecomms::CommsStateEmitter emitter;
  std::thread thread;
  std::atomic<bool> quit{false};
};

State& S() {
  static State s;
  return s;
}

void EmitLocked(State& s) {
  std::string json;
  if (s.emitter.Next(*s.core, NowMs(), &json)) EngineIpc::write(json);
}

void TickLocked(State& s) {
  edgecomms::CommsInputs in;
  in.now_ms = NowMs();
  in.in_meeting = s.source.in_meeting && s.source.in_meeting();
  if (!in.in_meeting && s.bank) {
    s.core->DetachBank();
    s.sdk->SetEvents(nullptr);
    s.bank.reset();
    s.sdk.reset();
  }
  if (in.in_meeting) {
    if (!s.bank) {
      s.sdk = std::make_unique<ScriptedTalkback>(s.web_client_id, s.supported);
      s.bank = std::make_unique<zc::TalkbackChannels>(s.sdk.get());
      s.core->AttachBank(s.bank.get());
    }
    s.sdk->Pump(in.now_ms);
    in.controller = true;
    in.supported = s.supported;
    in.host_or_cohost = s.host;
    in.mic_open = s.host && s.supported;  // the real engine opens it behind the silent mic
    if (s.source.roster) {
      in.roster = s.source.roster();
      for (auto& m : in.roster) m.supports_talkback = m.user_id != s.web_client_id;
    }
  }
  s.core->SetStationName(s.station);
  s.core->Tick(in);
  EmitLocked(s);
}

}  // namespace

void Start(FakeRosterSource source, uint32_t web_client_id) {
  State& s = S();
  std::lock_guard<std::mutex> lock(s.m);
  if (s.core) return;
  s.source = std::move(source);
  s.web_client_id = web_client_id;
  if (const char* id = std::getenv("COREVIDEO_FAKE_WEB_CLIENT_ID"))
    s.web_client_id = static_cast<uint32_t>(std::strtoul(id, nullptr, 10));
  if (const char* role = std::getenv("COREVIDEO_FAKE_COMMS_ROLE"))
    s.host = std::string(role) != "attendee";
  if (const char* u = std::getenv("COREVIDEO_FAKE_COMMS_UNSUPPORTED")) s.supported = u[0] != '1';
  s.mic = std::make_unique<SyntheticMic>(&s.sink);
  s.core = std::make_unique<edgecomms::CommsCore>(s.mic.get(), &s.sink);
  s.thread = std::thread([] {
    State& st = S();
    while (!st.quit.load()) {
      std::this_thread::sleep_for(std::chrono::milliseconds(100));
      std::lock_guard<std::mutex> lk(st.m);
      if (st.core) TickLocked(st);
    }
  });
}

bool HandleLine(const std::string& line) {
  if (!edgecomms::IsCommsCommand(line)) return false;
  State& s = S();
  std::lock_guard<std::mutex> lock(s.m);
  if (!s.core) return true;
  const std::string refusal = s.core->Apply(edgecomms::ParseCommsCommand(line), NowMs());
  if (!refusal.empty()) s.core->Log("refused: " + refusal);
  EmitLocked(s);
  return true;
}

void SetStationName(const std::string& name) {
  State& s = S();
  std::lock_guard<std::mutex> lock(s.m);
  if (!name.empty()) s.station = name;
}

void Stop() {
  State& s = S();
  s.quit.store(true);
  if (s.thread.joinable()) s.thread.join();
  std::lock_guard<std::mutex> lock(s.m);
  if (s.core) s.core->DetachBank();
  if (s.sdk) s.sdk->SetEvents(nullptr);
  s.bank.reset();
  s.sdk.reset();
  s.mic.reset();  // joins the synthetic mic thread (the sink is already detached)
  s.core.reset();
}

}  // namespace fakecomms
