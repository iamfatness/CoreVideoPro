// Zoom talkback ("Comms") in the macOS capture engine: the twin of
// engine-comms.{h,cpp} (Windows). Plain C++ so main-macos.mm (MRC ObjC++) and
// any C++ caller can include it; every Mac SDK type stays in the .mm, which is
// compiled with -fobjc-arc.
//
// Same shape and policy as Windows, only the SDK calls change:
//   - everything runs on the main queue (every Zoom SDK call in this engine
//     does, main-macos.mm's threading note), driven by a 100 ms
//     dispatch_source timer plus the comms_* lines the reader loop routes
//     here before engine dispatch;
//   - the mic chain (miniaudio, CoreAudio backend) runs on its own worker;
//   - the SDK-value decisions live in engine-comms-policy.h (tested on every
//     platform), the send-thread choice in main-queue-talkback-sdk.h.
#pragma once

#include <string>

class EngineCommsMac {
public:
    static EngineCommsMac &instance();

    // Main queue. Creates the core and schedules the 100 ms timer. Runs before
    // initSDKWithParams: the tick touches no SDK object until set_sdk_ready.
    void start();

    // True once initSDKWithParams has succeeded (handle_init).
    void set_sdk_ready(bool ready);

    // Main queue. True for every comms_* line, valid or not.
    bool handle_line(const std::string &line);

    // From the join line. Pro never sends capture_only, so this is false and
    // Private comms governs the mic.
    void set_capture_only(bool capture_only);

    // First statement of -onMeetingStatusChange:, on EVERY status, with the
    // RAW Mac ZoomSDKMeetingStatus (engine_comms::classifyMeetingStatus maps it).
    void on_meeting_status(int macStatus);

    // First statement of the quit block, while IPC is still open (it writes
    // debug lines). Stops the timer, ends the meeting, joins the audio worker.
    void shutdown();

    static bool owns_mic();

private:
    EngineCommsMac() = default;
};
