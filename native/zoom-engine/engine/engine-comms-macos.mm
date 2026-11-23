// See engine-comms-macos.h. The Windows twin is engine-comms.cpp; this file is
// a function-by-function port of it. Keep the policy and ordering IDENTICAL
// and change only SDK calls (plan
// docs/superpowers/plans/2026-11-22-mac-comms-social-phase4.md, boundary table
// rows 1-18). Every decision taken from an SDK value goes through
// engine-comms-policy.h, which is unit-tested on every platform; this file
// only performs.
#include "engine-comms-macos.h"

#import <AVFoundation/AVFoundation.h>
#import <Foundation/Foundation.h>
#import <ZoomSDK/ZoomSDK.h>

#include "engine-comms-policy.h"
#include "engine-writer.h"
#include "main-queue-talkback-sdk.h"

#include "comms/comms_core.h"
#include "comms/comms_protocol.h"
#include "comms/devices.h"
#include "comms/engine.h"
#include "comms/mic_guard.h"
#include "comms/talkback_channels.h"
#include "comms/talkback_sdk_mac.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <exception>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

#if !__has_feature(objc_arc)
#error "engine-comms-macos.mm requires ARC (-fobjc-arc); see CMakeLists.txt"
#endif

namespace {

int64_t now_ms()
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

int64_t now_us()
{
    return std::chrono::duration_cast<std::chrono::microseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

std::string to_utf8(NSString *s)
{
    if (!s) return {};
    const char *c = s.UTF8String;
    return c ? std::string(c) : std::string();
}

void on_silent_mic_dropped();  // below: revoke + mute, immediately

}  // namespace

// The auto-suppress (engine-comms.cpp:61-79): a virtual mic that is installed
// and never fed. The SDK then treats our meeting mic as open (talkback
// delivers, law 1) while the room hears nothing. `initialised` is what
// MicGuard's watchdog trusts. State is in atomics, as on Windows: the SDK may
// call these on any thread.
@interface CVSilentMic : NSObject <ZoomSDKVirtualAudioMicDelegate> {
@public
    std::atomic<bool> initialised;
    std::atomic<bool> sending;
}
@end

@implementation CVSilentMic
- (instancetype)init
{
    if ((self = [super init])) {
        initialised.store(false);
        sending.store(false);
    }
    return self;
}
- (void)onMicInitialize:(ZoomSDKAudioRawDataSender *_Nullable)rawdataSender { initialised.store(true); }
- (void)onMicStartSend { sending.store(true); }
- (void)onMicStopSend { sending.store(false); }
- (void)onMicUninitialized
{
    initialised.store(false);
    sending.store(false);
    on_silent_mic_dropped();
}
@end

namespace {

// The mic chain on its own thread: ZComms' AudioEngine (capture -> gain ->
// limiter -> PTT envelope -> sidetone tap -> 20 ms frames -> pacer -> sink),
// on miniaudio's CoreAudio backend. Opening/closing devices and enumerating
// them happens ONLY here.
//
// Copied from CommsAudioWin, engine-comms.cpp:86-325 -- keep in step; extract
// once the Windows engine builds in CI. The one addition is the microphone
// permission gate in run(): on macOS a denied grant opens the device fine and
// delivers digital silence, so talkback would look healthy (TX lamp on) while
// the guest hears nothing. The worker asks AVFoundation first and reports the
// reason (owner decision N1) instead.
class CommsAudioMac : public edgecomms::CommsAudioPort {
public:
    explicit CommsAudioMac(zc::FrameSink *sink) : sink_(sink) {}
    ~CommsAudioMac() override { shutdown(); }

    void SetRunning(bool on) override { update([&] { want_running_ = on; }); }
    void SetTalk(bool on) override
    {
        talk_.store(on);
        std::unique_lock<std::mutex> lock(engine_m_, std::try_to_lock);
        if (lock.owns_lock() && engine_) engine_->SetTalk(on);
    }
    void SetMic(const std::string &d) override { update([&] { want_mic_ = d; restart_ = true; }); }
    void SetSidetoneOut(const std::string &d) override { update([&] { want_out_ = d; restart_ = true; }); }
    void SetSidetone(bool on) override { update([&] { sidetone_ = on; }); }
    void SetTone(bool on) override { update([&] { tone_ = on; }); }
    void SetGain(int db) override { update([&] { gain_ = db; }); }
    edgecomms::CommsAudioStatus Status() override
    {
        std::lock_guard<std::mutex> lock(m_);
        return status_;
    }

    void ensure_started()
    {
        std::lock_guard<std::mutex> lock(m_);
        if (!worker_.joinable() && !quit_) worker_ = std::thread([this] { run(); });
    }

    // Bounded: a CoreAudio driver that never returns must not turn engine quit
    // into a hang. False = the worker is still stuck; the caller then leaks
    // this object on purpose (the thread still references it).
    bool shutdown(int timeout_ms = 3000)
    {
        {
            std::lock_guard<std::mutex> lock(m_);
            quit_ = true;
        }
        cv_.notify_all();
        if (!worker_.joinable()) return true;
        {
            std::unique_lock<std::mutex> lock(m_);
            if (!done_cv_.wait_for(lock, std::chrono::milliseconds(timeout_ms), [this] { return done_; })) {
                worker_.detach();
                return false;
            }
        }
        worker_.join();
        return true;
    }

private:
    template <typename F> void update(F f)
    {
        {
            std::lock_guard<std::mutex> lock(m_);
            f();
        }
        cv_.notify_all();
    }

    // Mac only. "" = granted. Asks once per process when undetermined (the
    // prompt names CoreVideo Pro.app: the engine is a forked child of the core,
    // itself the shell's child, and TCC attributes the use to the shell).
    std::string mic_permission_error()
    {
        const AVAuthorizationStatus st = [AVCaptureDevice authorizationStatusForMediaType:AVMediaTypeAudio];
        if (st == AVAuthorizationStatusNotDetermined && !asked_) {
            asked_ = true;
            [AVCaptureDevice requestAccessForMediaType:AVMediaTypeAudio completionHandler:^(BOOL) {}];
        }
        return engine_comms::micAccessError(static_cast<int>(st));
    }

    void run()
    {
        int64_t next_enum = 0, next_open = 0;
        std::vector<std::string> mics, outs;
        std::string error;
        while (true) {
            bool running, restart, sidetone, tone;
            int gain;
            std::string mic, out;
            {
                std::unique_lock<std::mutex> lock(m_);
                cv_.wait_for(lock, std::chrono::milliseconds(50));
                if (quit_) break;
                running = want_running_;
                restart = restart_;
                restart_ = false;
                sidetone = sidetone_;
                tone = tone_;
                gain = gain_;
                mic = want_mic_;
                out = want_out_;
            }
            const int64_t now = now_ms();
            if (now >= next_enum) {
                next_enum = now + 5000;
                mics.clear();
                outs.clear();
                for (const auto &d : zc::ListCaptureDevices()) mics.push_back(d.name);
                for (const auto &d : zc::ListPlaybackDevices()) outs.push_back(d.name);
            }
            if ((restart || !running) && engine_) stop_engine();
            if (restart) next_open = 0;
            if (running && !engine_ && now >= next_open) {
                @autoreleasepool {
                    error = mic_permission_error();
                }
                if (error.empty()) error = open_engine(mic, out, gain, sidetone);
                // Retry every 5 s, never spin: a denied grant is re-checked on
                // the same cadence as any open failure, so turning access on in
                // System Settings recovers without a restart.
                if (!error.empty() && !engine_) next_open = now + 5000;
            }
            edgecomms::CommsAudioStatus st;
            {
                std::lock_guard<std::mutex> lock(engine_m_);
                if (engine_) {
                    engine_->SetInputGainDb(gain);
                    engine_->SetSidetoneEnabled(sidetone);
                    engine_->SetTestTone(tone);
                    engine_->SetTalk(talk_.load());
                    const zc::EngineStats es = engine_->stats();
                    st.running = true;
                    st.device = engine_->capture_device_name();
                    st.sidetone_device = engine_->monitor_device_name();
                    st.peak_dbfs = 20.0 * std::log10((std::max)(es.capture_peak, 1.0e-5));
                    st.frames_sent = es.pacer.sends;
                    st.underruns = es.pacer.underruns;
                } else {
                    st.device = mic;
                    st.sidetone_device = out;
                }
            }
            st.devices = mics;
            st.sidetone_devices = outs;
            st.gain_db = gain;
            st.sidetone = sidetone;
            st.tone = tone;
            st.error = error;
            {
                std::lock_guard<std::mutex> lock(m_);
                // Counters survive an engine restart (device switch): a
                // number that drops back to zero reads as a broken TX path.
                if (!st.running) {
                    st.frames_sent = status_.frames_sent;
                    st.underruns = status_.underruns;
                } else {
                    st.frames_sent += frames_base_;
                    st.underruns += underruns_base_;
                }
                status_ = st;
            }
        }
        if (engine_) stop_engine();
        {
            std::lock_guard<std::mutex> lock(m_);
            done_ = true;
        }
        done_cv_.notify_all();
    }

    std::string open_engine(const std::string &mic, const std::string &out, int gain, bool sidetone)
    {
        zc::EngineConfig cfg;
        cfg.capture_device = mic;
        cfg.monitor_device = out;
        cfg.monitor_enabled = true;
        cfg.sidetone_enabled = sidetone;
        cfg.input_gain_db = gain;
        cfg.aec = false;  // not ported in this slice
        std::string err;
        auto engine = std::make_unique<zc::AudioEngine>(cfg, sink_);
        if (!engine->Start(&err)) {
            // A missing or busy headphone output must not cost the operator
            // their talk path: retry without the sidetone monitor and say so.
            cfg.monitor_enabled = false;
            std::string err2;
            engine = std::make_unique<zc::AudioEngine>(cfg, sink_);
            if (!engine->Start(&err2)) return err2.empty() ? err : err2;
            err = "sidetone output unavailable (" + err + "); talking without sidetone";
        }
        std::lock_guard<std::mutex> lock(engine_m_);
        engine_ = std::move(engine);
        return err;
    }

    void stop_engine()
    {
        std::unique_ptr<zc::AudioEngine> old;
        {
            std::lock_guard<std::mutex> lock(engine_m_);
            old = std::move(engine_);
        }
        if (!old) return;
        old->SetTalk(false);
        std::this_thread::sleep_for(std::chrono::milliseconds(30));  // let the release ramp finish
        const zc::EngineStats es = old->stats();
        old->Stop();  // joins the pacer: no Send after this returns
        std::lock_guard<std::mutex> lock(m_);
        frames_base_ += es.pacer.sends;
        underruns_base_ += es.pacer.underruns;
    }

    zc::FrameSink *sink_;
    std::thread worker_;
    std::mutex m_;
    std::condition_variable cv_;
    std::condition_variable done_cv_;
    bool done_ = false;
    bool quit_ = false;
    bool want_running_ = false;
    bool restart_ = false;
    bool asked_ = false;  // worker thread only
    std::string want_mic_, want_out_;
    bool sidetone_ = true, tone_ = false;
    int gain_ = 0;
    uint64_t frames_base_ = 0, underruns_base_ = 0;
    edgecomms::CommsAudioStatus status_;
    std::atomic<bool> talk_{false};
    std::mutex engine_m_;
    std::unique_ptr<zc::AudioEngine> engine_;
};

std::atomic<bool> g_owns_mic{false};

struct State {
    dispatch_source_t timer = nil;
    bool sdk_ready = false;
    bool in_tick = false;
    edgecomms::CommsBankSink sink;
    std::unique_ptr<CommsAudioMac> audio;
    std::unique_ptr<edgecomms::CommsCore> core;

    // Per meeting.
    std::unique_ptr<zc::TalkbackSdkMac> adapter;
    std::unique_ptr<engine_comms::MainQueueTalkbackSdk> decorator;  // what the bank talks to
    std::unique_ptr<zc::TalkbackChannels> bank;
    std::vector<zc::RosterMember> roster;
    int64_t next_roster_ms = 0;
    bool silent_mic_ever = false;  // removed again at shutdown
    bool silent_mic_set = false;   // setExternalAudioSource(silent_mic) is in force now
    bool capture_only = false;     // from the join line; Pro never sends it
    bool pending_end = false;      // a status change arrived mid-tick
    bool was_in_meeting = false;   // for comms_audio_join's InMeeting transition
    int last_audio_type = -1, last_audio_status = -1;
    int64_t next_hop_ms = 0;
    edgecomms::MicGuard guard;
    edgecomms::CommsStateEmitter emitter;
    // Adapters (and their decorators) are never destroyed while the SDK might
    // still hold them as a delegate or a queued send might reach them: a new
    // meeting's pair replaces the registration, the old one is parked here
    // until process exit (engine-comms.cpp:313-316).
    std::vector<std::unique_ptr<zc::TalkbackSdkMac>> retired;
    std::vector<std::unique_ptr<engine_comms::MainQueueTalkbackSdk>> retired_decorators;

    // Strong for the process lifetime: the SDK holds the delegate weakly, and
    // Windows keeps SilentMic in static state for the same reason.
    CVSilentMic *silent_mic = nil;
};

State &S()
{
    static State s;
    return s;
}

ZoomSDKMeetingService *meeting_service()
{
    return S().sdk_ready ? [[ZoomSDK sharedSDK] getMeetingService] : nil;
}

ZoomSDKRawDataAudioSourceController *audio_source_helper()
{
    ZoomSDKRawDataController *rdc = [[ZoomSDK sharedSDK] getRawDataController];
    if (!rdc) return nil;
    ZoomSDKRawDataAudioSourceController *helper = nil;
    if ([rdc getRawDataAudioSourceHelper:&helper] != ZoomSDKError_Success) return nil;
    return helper;
}

void emit_state()
{
    auto &s = S();
    if (!s.core) return;
    std::string json;
    if (s.emitter.Next(*s.core, now_ms(), &json)) EngineIpc::write(json);
}

void flush_guard_log()
{
    auto &s = S();
    for (auto &line : s.guard.TakeLog())
        if (s.core) s.core->Log(line);
}

// Mute OUR OWN mic. A meeting action, not an audio-controller call, on macOS
// (boundary row 10); never userID 0.
void mute_self(const char *why)
{
    ZoomSDKMeetingService *svc = meeting_service();
    ZoomSDKMeetingActionController *ctrl = svc ? [svc getMeetingActionController] : nil;
    ZoomSDKUserInfo *me = ctrl ? [ctrl getMyself] : nil;
    int code = -1;
    if (me && [me getUserID] != 0)
        code = static_cast<int>([ctrl actionMeetingWithCmd:ActionMeetingCmd_MuteAudio
                                                    userID:[me getUserID]
                                                  onScreen:ScreenType_First]);
    EngineIpc::write(std::string(R"({"cmd":"debug","stage":"comms_mic_revoked","why":")") + why +
                     R"(","code":)" + std::to_string(code) + "}");
}

void revoke_mic(const char *why)
{
    g_owns_mic.store(false);  // first: capture-only takes over again
    mute_self(why);
    flush_guard_log();
}

// Private comms off (or a non-capture-only meeting ending): the silent mic
// stops being the meeting mic. Ownership first, then the source, then mute:
// the operator's REAL mic is never unmuted on their behalf.
void remove_silent_mic()
{
    auto &s = S();
    if (!s.silent_mic_set) return;
    s.silent_mic_set = false;
    if (s.silent_mic) s.silent_mic->initialised.store(false);
    ZoomSDKRawDataAudioSourceController *helper = audio_source_helper();
    const int code = helper ? static_cast<int>([helper setExternalAudioSource:nil]) : -1;
    EngineIpc::write(R"({"cmd":"debug","stage":"comms_silent_mic_removed","code":)" + std::to_string(code) + "}");
}

void end_meeting()
{
    auto &s = S();
    s.pending_end = false;
    g_owns_mic.store(false);
    s.guard.Reset();
    // Private comms is per meeting in the SDK: a non-capture-only station must
    // not carry the silent mic into a meeting where private is off.
    if (!s.capture_only) remove_silent_mic();
    if (s.silent_mic) s.silent_mic->initialised.store(false);  // a new meeting must re-prove it
    // Mac addition: no queued main-queue send from this meeting may run
    // after this point (the decorator drops them).
    if (s.decorator) s.decorator->Shutdown();
    if (s.core) s.core->DetachBank();  // the sink lets go first: no send after this
    if (s.decorator) {
        s.decorator->SetEvents(nullptr);  // forwards to the adapter
        s.retired_decorators.push_back(std::move(s.decorator));
    }
    if (s.adapter) s.retired.push_back(std::move(s.adapter));
    s.bank.reset();
    s.roster.clear();
    s.next_roster_ms = 0;
}

void on_silent_mic_dropped()
{
    // Guard state is main-queue state; the SDK's thread for this callback is
    // not documented, so hop when needed. The revoke itself is immediate on
    // the main queue.
    auto act = ^{
        auto &s = S();
        if (s.guard.Dropped()) revoke_mic("virtual_mic_uninitialized");
    };
    if ([NSThread isMainThread]) act();
    else dispatch_async(dispatch_get_main_queue(), act);
}

void refresh_roster(ZoomSDKMeetingActionController *ctrl, unsigned int self_id)
{
    // Polled, never evented: main-macos.mm owns the action controller's
    // delegate slot for the roster.
    auto &s = S();
    s.roster.clear();
    NSArray *list = [ctrl getParticipantsList];
    if (!list) return;
    for (NSNumber *n in list) {
        const unsigned int uid = n.unsignedIntValue;
        if (uid == self_id) continue;
        zc::RosterMember m;
        m.user_id = uid;
        if (ZoomSDKUserInfo *info = [ctrl getUserByUserID:uid]) {
            m.name = to_utf8([info getUserName]);
            m.is_host = [info isHost] ? true : false;
            m.supports_talkback = [info isSupportTalkback] ? true : false;
        }
        if (m.name.empty()) m.name = "user " + std::to_string(uid);
        s.roster.push_back(std::move(m));
    }
}

void post_to_main(std::function<void()> f)
{
    auto block = ^{ f(); };
    dispatch_async(dispatch_get_main_queue(), block);
}

void tick()
{
    auto &s = S();
    if (!s.core || s.in_tick) return;
    s.in_tick = true;
    const int64_t now = now_ms();
    edgecomms::CommsInputs in;
    in.now_ms = now;
    ZoomSDKMeetingService *svc = meeting_service();
    in.in_meeting = svc && static_cast<int>([svc getMeetingStatus]) ==
                               static_cast<int>(engine_comms::MacMeetingStatus::InMeeting);
    in.private_mode = s.capture_only || s.core->private_requested();
    if (!in.in_meeting && (s.bank || s.adapter || s.guard.installed())) end_meeting();
    const bool entered_meeting = in.in_meeting && !s.was_in_meeting;
    s.was_in_meeting = in.in_meeting;

    if (in.in_meeting) {
        ZoomSDKTalkbackController *ctrl = [svc getTalkbackController];
        if (ctrl && !s.adapter) {
            s.adapter = std::make_unique<zc::TalkbackSdkMac>((__bridge void *)ctrl);
            const engine_comms::SendMode mode =
                engine_comms::sendModeFromEnv(std::getenv("COREVIDEO_COMMS_SEND_MODE"));
            s.decorator = std::make_unique<engine_comms::MainQueueTalkbackSdk>(
                s.adapter.get(), mode, post_to_main, now_us);
            s.bank = std::make_unique<zc::TalkbackChannels>(s.decorator.get());
            s.core->AttachBank(s.bank.get());
        }
        in.controller = ctrl != nil;
        in.supported = s.decorator && s.decorator->MeetingSupportsTalkback();

        ZoomSDKMeetingActionController *actions = [svc getMeetingActionController];
        ZoomSDKUserInfo *me = actions ? [actions getMyself] : nil;
        if (me) {
            in.host_or_cohost = engine_comms::hostOrCohost(static_cast<int>([me getUserRole]));
            s.core->SetStationName(to_utf8([me getUserName]));
        }
        if (actions && now >= s.next_roster_ms) {
            s.next_roster_ms = now + 1000;
            refresh_roster(actions, me ? [me getUserID] : 0);
        }
        in.roster = s.roster;

        const int audio_type = me ? static_cast<int>([me getAudioType]) : 0;
        const int audio_status = me ? static_cast<int>([me getAudioStatus]) : 0;
        if (auto line = engine_comms::audioJoinBreadcrumb(s.last_audio_type, s.last_audio_status, audio_type,
                                                          audio_status, entered_meeting))
            EngineIpc::write(*line);
        s.last_audio_type = audio_type;
        s.last_audio_status = audio_status;

        // Law 1: talkback is silent unless our meeting audio is OPEN -- and
        // the REAL mic must never be open to the room. MicGuard decides;
        // this only performs (mic_guard.h).
        if (!s.silent_mic) s.silent_mic = [[CVSilentMic alloc] init];
        edgecomms::MicGuard::In gi;
        gi.in_meeting = true;
        gi.can_talkback = in.controller && in.supported && in.host_or_cohost && me;
        gi.capture_only = s.capture_only;
        gi.private_on = s.core->private_requested();
        ZoomSDKRawDataController *rdc = [[ZoomSDK sharedSDK] getRawDataController];
        gi.licence = gi.can_talkback && !s.guard.installed() && rdc &&
                     engine_comms::licensed(static_cast<int>([rdc hasRawDataLicense]));
        gi.initialised = s.silent_mic->initialised.load();
        gi.self_muted = !me || engine_comms::selfMuted(audio_status);
        gi.now_ms = now;
        switch (s.guard.Next(gi)) {
        case edgecomms::MicGuard::Act::Install: {
            ZoomSDKRawDataAudioSourceController *helper = audio_source_helper();
            const ZoomSDKError err =
                helper ? [helper setExternalAudioSource:s.silent_mic] : ZoomSDKError_Uninit;
            const bool ok = err == ZoomSDKError_Success;
            if (ok) s.silent_mic_ever = s.silent_mic_set = true;
            s.guard.InstallResult(ok, "setExternalAudioSource " + std::to_string(static_cast<int>(err)), now);
            g_owns_mic.store(s.guard.owns());  // false: nothing is owned before onMicInitialize
            EngineIpc::write(R"({"cmd":"debug","stage":"comms_silent_mic","code":)" +
                             std::to_string(static_cast<int>(err)) + "}");
            break;
        }
        case edgecomms::MicGuard::Act::Unmute: {
            // Ownership is only ever taken after onMicInitialize; publish it
            // before unmuting so capture-only does not fight the unmute.
            g_owns_mic.store(s.guard.owns());
            const ZoomSDKError err =
                (actions && me && [me getUserID] != 0)
                    ? [actions actionMeetingWithCmd:ActionMeetingCmd_UnMuteAudio
                                             userID:[me getUserID]
                                           onScreen:ScreenType_First]
                    : ZoomSDKError_Uninit;
            if (err == ZoomSDKError_Success)
                s.core->Log("meeting mic opened behind the silent virtual mic -- talkback delivery needs it");
            else
                s.core->Log("meeting mic is MUTED (talkback is silent until it opens): UnMuteAudio " +
                            std::to_string(static_cast<int>(err)) +
                            " -- if the meeting forbids self-unmute, the host must allow it");
            break;
        }
        case edgecomms::MicGuard::Act::Revoke:
            revoke_mic("virtual_mic_not_initialised");
            break;
        case edgecomms::MicGuard::Act::Mute:
            revoke_mic("private_comms");
            break;
        case edgecomms::MicGuard::Act::Uninstall:
            g_owns_mic.store(false);
            remove_silent_mic();
            revoke_mic("private_comms_off");
            break;
        case edgecomms::MicGuard::Act::None:
            break;
        }
        g_owns_mic.store(s.guard.owns());
        flush_guard_log();
        in.mic_blocked = s.guard.blocked_reason();
        in.mic_open = me && engine_comms::meetingMicOpen(audio_type, audio_status);

        // Mac only: how long main-queue sends wait, while anything is keyed.
        if (s.decorator && s.bank && s.bank->key_mask() != 0 && now >= s.next_hop_ms) {
            if (s.next_hop_ms != 0) {
                const auto h = s.decorator->TakeHopStats();
                EngineIpc::write(std::string(R"({"cmd":"debug","stage":"comms_send_hop","count":)") +
                                 std::to_string(h.count) + R"(,"maxUs":)" + std::to_string(h.maxUs) +
                                 R"(,"p99Us":)" + std::to_string(h.p99Us) + R"(,"mode":")" +
                                 (s.decorator->mode() == engine_comms::SendMode::MainQueue ? "main" : "direct") +
                                 "\"}");
            } else {
                s.decorator->TakeHopStats();  // start a fresh window at key-down
            }
            s.next_hop_ms = now + 10000;
        } else if (!s.bank || s.bank->key_mask() == 0) {
            s.next_hop_ms = 0;
        }
    } else {
        s.last_audio_type = s.last_audio_status = -1;
    }

    s.core->Tick(in);
    emit_state();
    s.in_tick = false;
    if (s.pending_end) end_meeting();
}

void timer_fired()
{
    // Comms must never take the engine down with it (engine-comms.cpp:531-544).
    @try {
        try {
            tick();
        } catch (const std::exception &e) {
            S().in_tick = false;
            EngineIpc::write(std::string(R"({"cmd":"debug","stage":"comms_tick_exception","what":")") +
                             e.what() + "\"}");
        } catch (...) {
            S().in_tick = false;
            EngineIpc::write(R"({"cmd":"debug","stage":"comms_tick_exception"})");
        }
    } @catch (NSException *) {
        S().in_tick = false;
        EngineIpc::write(R"({"cmd":"debug","stage":"comms_tick_exception","what":"NSException"})");
    }
}

}  // namespace

EngineCommsMac &EngineCommsMac::instance()
{
    static EngineCommsMac inst;
    return inst;
}

bool EngineCommsMac::owns_mic() { return g_owns_mic.load(); }

void EngineCommsMac::set_capture_only(bool capture_only) { S().capture_only = capture_only; }

void EngineCommsMac::set_sdk_ready(bool ready) { S().sdk_ready = ready; }

void EngineCommsMac::on_meeting_status(int macStatus)
{
    // Synchronous, before the engine reacts: a leave + rejoin or a reconnect
    // that lands back in InMeeting between two ticks must find the mic
    // un-owned until Comms re-proves the silent mic for this meeting. The
    // Mac enum's in-meeting notification (AudioReady) is ignored, as Windows
    // ignores LOCKED/UNLOCKED (engine-comms-policy.h).
    const auto action = engine_comms::classifyMeetingStatus(macStatus);
    if (action == engine_comms::StatusAction::Ignore) return;
    auto &s = S();
    g_owns_mic.store(false);
    s.guard.Reset();
    if (s.silent_mic) s.silent_mic->initialised.store(false);
    if (action == engine_comms::StatusAction::ResetAndEndMeeting) {
        // Release the bank now: the TX pacer must not reach a controller the
        // SDK is tearing down.
        if (s.in_tick) s.pending_end = true;
        else end_meeting();
    }
}

void EngineCommsMac::start()
{
    auto &s = S();
    if (s.core) return;
    s.audio = std::make_unique<CommsAudioMac>(&s.sink);
    s.audio->ensure_started();
    s.core = std::make_unique<edgecomms::CommsCore>(s.audio.get(), &s.sink);
    s.timer = dispatch_source_create(DISPATCH_SOURCE_TYPE_TIMER, 0, 0, dispatch_get_main_queue());
    dispatch_source_set_timer(s.timer, dispatch_time(DISPATCH_TIME_NOW, 100 * NSEC_PER_MSEC),
                              100 * NSEC_PER_MSEC, 10 * NSEC_PER_MSEC);
    dispatch_source_set_event_handler(s.timer, ^{ timer_fired(); });
    dispatch_resume(s.timer);
}

bool EngineCommsMac::handle_line(const std::string &line)
{
    if (!edgecomms::IsCommsCommand(line)) return false;
    auto &s = S();
    if (!s.core) return true;
    try {
        const edgecomms::CommsCommand c = edgecomms::ParseCommsCommand(line);
        const std::string refusal = s.core->Apply(c, now_ms());
        if (!refusal.empty()) s.core->Log("refused: " + refusal);
        emit_state();
    } catch (...) {
        EngineIpc::write(R"({"cmd":"debug","stage":"comms_command_exception"})");
    }
    return true;
}

void EngineCommsMac::shutdown()
{
    auto &s = S();
    if (!s.core) return;
    if (s.timer) dispatch_source_cancel(s.timer);
    s.timer = nil;
    end_meeting();
    // Joins the worker, which stops the TX pacer -- bounded: a stuck driver
    // is leaked rather than allowed to hang quit (its thread still uses it).
    if (s.audio && !s.audio->shutdown(3000)) {
        EngineIpc::write(R"({"cmd":"debug","stage":"comms_audio_shutdown_timeout"})");
        s.audio.release();
    }
    if (s.silent_mic_ever) {
        if (ZoomSDKRawDataAudioSourceController *helper = audio_source_helper())
            [helper setExternalAudioSource:nil];
    }
    s.core.reset();
}
