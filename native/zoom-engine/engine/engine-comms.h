// CoreVideo Edge delta 7 (zoom-engine README): Zoom talkback ("Comms") in
// the capture engine.
//
// Talkback is sent by a meeting participant that is host or co-host, and the
// SDK is one participant per process, so the talkback station has to be THIS
// process -- the same participant that receives the video (design:
// docs/superpowers/specs/2026-11-09-comms-design.md). The policy lives in
// comms/comms_core (ported ZComms orchestration, shared with the fake); this
// file is only the Zoom glue:
//
//   - everything runs on the engine's SDK thread, from a 100 ms thread timer
//     dispatched by the command loop's own message pump, plus the comms_*
//     command lines routed here before the engine's substring dispatch;
//   - the mic chain (miniaudio capture -> gain -> limiter -> PTT envelope ->
//     20 ms pacer) lives on its own worker thread: device enumeration and
//     WASAPI opens are somebody else's driver code and must never stall the
//     SDK thread that video callbacks share;
//   - delivery law 1 (talkback is SILENT unless our meeting audio is open):
//     the engine joins capture-only with our mic muted (delta 1). When the
//     station can run talkback it installs a never-fed virtual mic
//     (setExternalAudioSource -- open but silent to the room, the ZoomISO /
//     ZComms auto-suppress) and only once Zoom confirms it initialised
//     (onMicInitialize) unmutes us and keeps us unmuted (re-asserted every
//     2 s). Without that proof the meeting mic is NEVER opened -- the room
//     would hear this PC's mic -- and the reason is the comms state reason
//     (comms/mic_guard decides all of it);
//   - a comms failure never touches subscriptions, ISO or raw media: no call
//     here reaches EngineVideo/EngineAudio/EngineShare, and every tick is
//     exception-fenced.
#pragma once

#include <string>

#include <windows.h>  // the SDK headers use HWND/RECT bare
#include <zoom_sdk.h>
#include <meeting_service_interface.h>

class EngineComms {
public:
    static EngineComms &instance();

    // Starts the 100 ms timer on the calling (SDK) thread.
    void start(ZOOMSDK::IMeetingService **meeting_svc);

    // True when `line` is a comms_* command (handled here, whatever its
    // validity). Call BEFORE the engine's own dispatch: that one matches
    // "join" / "quit" as substrings, which a device name could contain.
    bool handle_line(const std::string &line);

    // Capture-only (delta 1) must leave our mic open while this is true: the
    // silent virtual mic is installed, so "open" is silent to the room.
    static bool owns_mic();

    // From the join line: Edge joins capture-only (the virtual mic + unmute
    // apply). A join that is NOT capture-only (an operator who speaks in the
    // meeting) keeps its own mic: no virtual mic, no unmute, only a report.
    void set_capture_only(bool capture_only);

    // From onMeetingStatusChanged, first thing, on EVERY status: drops mic
    // ownership synchronously and, away from INMEETING, releases the bank.
    void on_meeting_status(int status);

    // Before CleanUPSDK(): stops the timer, releases the bank, joins the
    // audio worker (and with it the TX pacer thread), removes the silent mic.
    void shutdown();

private:
    EngineComms() = default;
};
