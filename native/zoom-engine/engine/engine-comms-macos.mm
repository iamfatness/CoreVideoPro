// See engine-comms-macos.h. The Windows twin is engine-comms.cpp; keep the
// policy and ordering identical and change only SDK calls (plan
// docs/superpowers/plans/2026-11-22-mac-comms-social-phase4.md, boundary
// table rows 1-18).
#include "engine-comms-macos.h"

#import <Foundation/Foundation.h>
#import <ZoomSDK/ZoomSDK.h>

#include "engine-comms-policy.h"
#include "engine-writer.h"

#include "comms/comms_core.h"
#include "comms/comms_protocol.h"
#include "comms/mic_guard.h"

#include <atomic>
#include <chrono>
#include <exception>
#include <memory>

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

// Placeholder until the mic chain lands: no device, never running. The core
// then reports talkback as down for want of audio, never as healthy.
class CommsAudioIdle : public edgecomms::CommsAudioPort {
public:
    void SetRunning(bool) override {}
    void SetTalk(bool) override {}
    void SetMic(const std::string &) override {}
    void SetSidetoneOut(const std::string &) override {}
    void SetSidetone(bool) override {}
    void SetTone(bool) override {}
    void SetGain(int) override {}
    edgecomms::CommsAudioStatus Status() override { return {}; }
};

std::atomic<bool> g_owns_mic{false};

struct State {
    dispatch_source_t timer = nil;
    bool sdk_ready = false;
    bool in_tick = false;
    bool capture_only = false;
    edgecomms::CommsBankSink sink;
    std::unique_ptr<edgecomms::CommsAudioPort> audio;
    std::unique_ptr<edgecomms::CommsCore> core;
    edgecomms::MicGuard guard;
    edgecomms::CommsStateEmitter emitter;
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

void end_meeting()
{
    auto &s = S();
    g_owns_mic.store(false);
    s.guard.Reset();
}

void tick()
{
    auto &s = S();
    if (!s.core || s.in_tick) return;
    s.in_tick = true;
    edgecomms::CommsInputs in;
    in.now_ms = now_ms();
    ZoomSDKMeetingService *svc = s.sdk_ready ? [[ZoomSDK sharedSDK] getMeetingService] : nil;
    in.in_meeting = svc && static_cast<int>([svc getMeetingStatus]) ==
                               static_cast<int>(engine_comms::MacMeetingStatus::InMeeting);
    in.private_mode = s.capture_only || s.core->private_requested();
    s.core->Tick(in);
    emit_state();
    s.in_tick = false;
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
    const auto action = engine_comms::classifyMeetingStatus(macStatus);
    if (action == engine_comms::StatusAction::Ignore) return;
    auto &s = S();
    g_owns_mic.store(false);
    s.guard.Reset();
    if (action == engine_comms::StatusAction::ResetAndEndMeeting) end_meeting();
}

void EngineCommsMac::start()
{
    auto &s = S();
    if (s.core) return;
    s.audio = std::make_unique<CommsAudioIdle>();
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
    s.core.reset();
    s.audio.reset();
}
