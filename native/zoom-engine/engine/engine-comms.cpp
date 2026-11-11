#include "engine-comms.h"

#include "engine-writer.h"

#if __has_include(<meeting_service_components/meeting_audio_interface.h>)
#include <meeting_service_components/meeting_audio_interface.h>
#include <meeting_service_components/meeting_participants_ctrl_interface.h>
#include <meeting_service_components/meeting_talkback_ctrl_interface.h>
#else
#include <meeting_audio_interface.h>
#include <meeting_participants_ctrl_interface.h>
#include <meeting_talkback_ctrl_interface.h>
#endif
#if __has_include(<rawdata/zoom_rawdata_api.h>)
#include <rawdata/zoom_rawdata_api.h>
#include <rawdata/rawdata_audio_helper_interface.h>
#else
#include <zoom_rawdata_api.h>
#include <rawdata_audio_helper_interface.h>
#endif

#include "comms/comms_core.h"
#include "comms/comms_protocol.h"
#include "comms/devices.h"
#include "comms/engine.h"
#include "comms/mic_guard.h"
#include "comms/talkback_channels.h"
#include "comms/talkback_sdk_win.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

namespace {

int64_t now_ms()
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

std::string narrow(const zchar_t *s)
{
    if (!s) return {};
    const int n = WideCharToMultiByte(CP_UTF8, 0, s, -1, nullptr, 0, nullptr, nullptr);
    if (n <= 1) return {};
    std::string out(static_cast<size_t>(n - 1), '\0');
    WideCharToMultiByte(CP_UTF8, 0, s, -1, out.data(), n, nullptr, nullptr);
    return out;
}

void on_silent_mic_dropped();  // below: revoke + mute, immediately

// The auto-suppress: a virtual mic that is installed and never fed. The SDK
// then treats our meeting mic as open (talkback delivers, law 1) while the
// room hears nothing. `initialised` is what MicGuard's watchdog trusts: no
// onMicInitialize within the grace window, or an onMicUninitialized, and
// the mic is muted again at once.
class SilentMic : public ZOOMSDK::IZoomSDKVirtualAudioMicEvent {
public:
    void onMicInitialize(ZOOMSDK::IZoomSDKAudioRawDataSender *) override { initialised.store(true); }
    void onMicStartSend() override { sending.store(true); }
    void onMicStopSend() override { sending.store(false); }
    void onMicUninitialized() override
    {
        initialised.store(false);
        sending.store(false);
        on_silent_mic_dropped();
    }
    std::atomic<bool> initialised{false};
    std::atomic<bool> sending{false};
};

// The mic chain on its own thread: ZComms' AudioEngine (capture -> gain ->
// limiter -> PTT envelope -> sidetone tap -> 20 ms frames -> pacer -> sink).
// Opening/closing WASAPI devices and enumerating them happens ONLY here.
class CommsAudioWin : public edgecomms::CommsAudioPort {
public:
    explicit CommsAudioWin(zc::FrameSink *sink) : sink_(sink) {}
    ~CommsAudioWin() override { shutdown(); }

    void SetRunning(bool on) override { update([&] { want_running_ = on; }); }
    void SetTalk(bool on) override
    {
        talk_.store(on);
        // Immediate when the worker is not mid-restart; otherwise the worker
        // applies talk_ on its next pass (it re-asserts every 50 ms).
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

    // Bounded: a WASAPI driver that never returns must not turn engine quit
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
                error = open_engine(mic, out, gain, sidetone);
                if (!error.empty()) next_open = now + 5000;  // retry, never spin
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
    ZOOMSDK::IMeetingService **meeting_svc = nullptr;
    UINT_PTR timer = 0;
    bool in_tick = false;
    edgecomms::CommsBankSink sink;
    std::unique_ptr<CommsAudioWin> audio;
    std::unique_ptr<edgecomms::CommsCore> core;

    // Per meeting.
    std::unique_ptr<zc::TalkbackSdkWin> adapter;
    std::unique_ptr<zc::TalkbackChannels> bank;
    std::vector<zc::RosterMember> roster;
    int64_t next_roster_ms = 0;
    bool silent_mic_ever = false;  // removed again at shutdown
    bool silent_mic_set = false;   // setExternalAudioSource(&silent_mic) is in force now
    bool capture_only = true;      // from the join line (delta 1)
    bool pending_end = false;      // a status change arrived mid-tick
    edgecomms::MicGuard guard;
    edgecomms::CommsStateEmitter emitter;
    // Adapters are never destroyed while the SDK might still hold them as an
    // event sink: a new meeting's adapter replaces the registration, the old
    // one is parked here until process exit (a few hundred bytes each).
    std::vector<std::unique_ptr<zc::TalkbackSdkWin>> retired;

    SilentMic silent_mic;
};

State &S()
{
    static State s;
    return s;
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

// Mute OUR OWN mic (never MuteAudio(0): that mutes the meeting). The engine
// flag is cleared BEFORE this so capture-only agrees the mic must be closed.
void mute_self(const char *why)
{
    auto &s = S();
    ZOOMSDK::IMeetingService *svc = s.meeting_svc ? *s.meeting_svc : nullptr;
    auto *parts = svc ? svc->GetMeetingParticipantsController() : nullptr;
    auto *me = parts ? parts->GetMySelfUser() : nullptr;
    auto *audio_ctrl = svc ? svc->GetMeetingAudioController() : nullptr;
    int code = -1;
    if (me && audio_ctrl && me->GetUserID() != 0)
        code = static_cast<int>(audio_ctrl->MuteAudio(me->GetUserID(), true));
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
    s.silent_mic.initialised.store(false);
    auto *helper = ZOOMSDK::GetAudioRawdataHelper();
    const int code = helper ? static_cast<int>(helper->setExternalAudioSource(nullptr)) : -1;
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
    s.silent_mic.initialised.store(false);  // a new meeting must re-prove it
    if (s.core) s.core->DetachBank();  // the sink lets go first: no send after this
    if (s.adapter) {
        s.adapter->SetEvents(nullptr);
        s.retired.push_back(std::move(s.adapter));
    }
    s.bank.reset();
    s.roster.clear();
    s.next_roster_ms = 0;
}

void on_silent_mic_dropped()
{
    auto &s = S();
    if (s.guard.Dropped()) revoke_mic("virtual_mic_uninitialized");
}

void refresh_roster(ZOOMSDK::IMeetingParticipantsController *parts, unsigned int self_id)
{
    // Polled, never evented: EngineParticipants owns the participants
    // controller's single event slot, and ZComms refreshes on a timer anyway
    // (capability changes have no callback).
    auto &s = S();
    s.roster.clear();
    auto *list = parts->GetParticipantsList();
    if (!list) return;
    for (int i = 0; i < list->GetCount(); ++i) {
        const unsigned int uid = list->GetItem(i);
        if (uid == self_id) continue;
        zc::RosterMember m;
        m.user_id = uid;
        if (auto *info = parts->GetUserByUserID(uid)) {
            m.name = narrow(info->GetUserName());
            m.is_host = info->IsHost();
            m.supports_talkback = info->IsSupportTalkback();
        }
        if (m.name.empty()) m.name = "user " + std::to_string(uid);
        s.roster.push_back(std::move(m));
    }
}

void tick()
{
    auto &s = S();
    if (!s.core || s.in_tick) return;
    s.in_tick = true;
    const int64_t now = now_ms();
    edgecomms::CommsInputs in;
    in.now_ms = now;
    ZOOMSDK::IMeetingService *svc = s.meeting_svc ? *s.meeting_svc : nullptr;
    in.in_meeting = svc && svc->GetMeetingStatus() == ZOOMSDK::MEETING_STATUS_INMEETING;
    in.private_mode = s.capture_only || s.core->private_requested();
    if (!in.in_meeting && (s.bank || s.adapter || s.guard.installed())) end_meeting();

    if (in.in_meeting) {
        auto *ctrl = svc->GetMeetingTalkbackController();
        if (ctrl && !s.adapter) {
            s.adapter = std::make_unique<zc::TalkbackSdkWin>(ctrl);
            s.bank = std::make_unique<zc::TalkbackChannels>(s.adapter.get());
            s.core->AttachBank(s.bank.get());
        }
        in.controller = ctrl != nullptr;
        in.supported = s.adapter && s.adapter->MeetingSupportsTalkback();

        auto *parts = svc->GetMeetingParticipantsController();
        ZOOMSDK::IUserInfo *me = parts ? parts->GetMySelfUser() : nullptr;
        if (me) {
            const auto role = me->GetUserRole();
            in.host_or_cohost = role == ZOOMSDK::USERROLE_HOST || role == ZOOMSDK::USERROLE_COHOST;
            s.core->SetStationName(narrow(me->GetUserName()));
        }
        if (parts && now >= s.next_roster_ms) {
            s.next_roster_ms = now + 1000;
            refresh_roster(parts, me ? me->GetUserID() : 0);
        }
        in.roster = s.roster;

        // Law 1: talkback is silent unless our meeting audio is OPEN -- and
        // the REAL mic must never be open to the room. MicGuard decides;
        // this only performs (mic_guard.h).
        edgecomms::MicGuard::In gi;
        gi.in_meeting = true;
        gi.can_talkback = in.controller && in.supported && in.host_or_cohost && me;
        gi.capture_only = s.capture_only;
        gi.private_on = s.core->private_requested();
        gi.licence = gi.can_talkback && !s.guard.installed() && ZOOMSDK::HasRawdataLicense();
        gi.initialised = s.silent_mic.initialised.load();
        gi.self_muted = !me || me->IsAudioMuted();
        gi.now_ms = now;
        switch (s.guard.Next(gi)) {
        case edgecomms::MicGuard::Act::Install: {
            auto *helper = ZOOMSDK::GetAudioRawdataHelper();
            const ZOOMSDK::SDKError err =
                helper ? helper->setExternalAudioSource(&s.silent_mic) : ZOOMSDK::SDKERR_UNINITIALIZE;
            const bool ok = err == ZOOMSDK::SDKERR_SUCCESS;
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
            auto *audio_ctrl = svc->GetMeetingAudioController();
            const ZOOMSDK::SDKError err =
                audio_ctrl ? audio_ctrl->UnMuteAudio(me->GetUserID()) : ZOOMSDK::SDKERR_UNINITIALIZE;
            if (err == ZOOMSDK::SDKERR_SUCCESS)
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
        in.mic_open = me && me->GetAudioJoinType() != ZOOMSDK::AUDIOTYPE_NONE && !me->IsAudioMuted();
    }

    s.core->Tick(in);
    emit_state();
    s.in_tick = false;
    if (s.pending_end) end_meeting();
}

void CALLBACK timer_proc(HWND, UINT, UINT_PTR, DWORD)
{
    // Comms must never take the engine down with it.
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
}

}  // namespace

EngineComms &EngineComms::instance()
{
    static EngineComms inst;
    return inst;
}

bool EngineComms::owns_mic() { return g_owns_mic.load(); }

void EngineComms::set_capture_only(bool capture_only) { S().capture_only = capture_only; }

void EngineComms::on_meeting_status(int status)
{
    // Synchronous, from onMeetingStatusChanged, BEFORE the engine reacts
    // (attach() runs enforce_capture_only on INMEETING): a leave + rejoin or
    // a reconnect that lands back in INMEETING between two 100 ms ticks must
    // find the mic un-owned, so capture-only mutes it until Comms re-proves
    // the silent mic for this meeting.
    // LOCKED/UNLOCKED are notifications while still in the meeting, not
    // transitions: tearing the bank down on them would re-request 16 channels.
    if (status == ZOOMSDK::MEETING_STATUS_LOCKED || status == ZOOMSDK::MEETING_STATUS_UNLOCKED) return;
    auto &s = S();
    g_owns_mic.store(false);
    s.guard.Reset();
    s.silent_mic.initialised.store(false);
    if (status != ZOOMSDK::MEETING_STATUS_INMEETING) {
        // Release the bank now: the TX pacer must not reach a controller the
        // SDK is tearing down.
        if (s.in_tick) s.pending_end = true;
        else end_meeting();
    }
}

void EngineComms::start(ZOOMSDK::IMeetingService **meeting_svc)
{
    auto &s = S();
    if (s.core) return;
    s.meeting_svc = meeting_svc;
    s.audio = std::make_unique<CommsAudioWin>(&s.sink);
    s.audio->ensure_started();
    s.core = std::make_unique<edgecomms::CommsCore>(s.audio.get(), &s.sink);
    s.timer = SetTimer(nullptr, 0, 100, timer_proc);
    if (!s.timer)
        EngineIpc::write(R"({"cmd":"debug","stage":"comms_timer","code":)" +
                         std::to_string(static_cast<int>(GetLastError())) + "}");
}

bool EngineComms::handle_line(const std::string &line)
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

void EngineComms::shutdown()
{
    auto &s = S();
    if (!s.core) return;
    if (s.timer) KillTimer(nullptr, s.timer);
    s.timer = 0;
    end_meeting();
    // Joins the worker, which stops the TX pacer -- bounded: a stuck driver
    // is leaked rather than allowed to hang quit (its thread still uses it).
    if (s.audio && !s.audio->shutdown(3000)) {
        EngineIpc::write(R"({"cmd":"debug","stage":"comms_audio_shutdown_timeout"})");
        s.audio.release();
    }
    if (s.silent_mic_ever) {
        if (auto *helper = ZOOMSDK::GetAudioRawdataHelper()) helper->setExternalAudioSource(nullptr);
    }
    s.core.reset();
}
