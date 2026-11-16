// ControlApiTestsRC2.swift — remote-control parity cases from RC2 on (session,
// inputs, and the state they publish), kept out of ControlApiTests.swift so the
// slices stay separable. Registered in ShellTests.runAndExit via `controlParityCases`.
// Plan: docs/superpowers/plans/2026-11-16-mac-remote-control-phase1.md.

import Foundation

extension AppModel {
    /// Test seam: set one strip's output level (adding the strip if absent), which
    /// fires objectWillChange exactly like a core meter update does.
    func audioStripLevelForTesting(id: String, level: Int) {
        if let index = strips.firstIndex(where: { $0.id == id }) {
            strips[index].outputLevel = level
        } else {
            strips.append(AudioStrip(id: id, outputLevel: level, manualGainDb: 0, muted: false, solo: false,
                                     pan: 0, status: "live"))
        }
    }
}

extension ShellTests {
    @MainActor
    static var controlParityCases: [(String, () -> Void)] {
        [
            ("control/source-id-and-kinds", testControlSourceIdAndKinds),
            ("control/rc2-state-fields", testControlRc2StateFields),
            ("control/push-key", testControlPushKeyIgnoresMeters),
            ("control/input-actions", testControlInputActions),
            ("control/transition-and-engine", testControlTransitionAndEngine),
            ("control/join-leave-capture-online", testControlJoinLeaveAndCaptureOnline),
            ("control/rtmp-configure", testControlRtmpConfigure),
            ("control/production-actions", testControlProductionActions),
            ("control/monitor-revisioned", testControlMonitorRevisioned),
            ("control/audio-route", testControlAudioRoute),
            ("control/osc-feedback-closure", testControlOscFeedbackCoversEverySupportedField),
        ]
    }

    @MainActor
    static func testControlSourceIdAndKinds() {
        expectEqual(ControlSourceId.parse("zoom:p-1"), .zoom("p-1"), "zoom")
        expectEqual(ControlSourceId.parse("capture:cam1"), .capture("cam1"), "capture")
        expectEqual(ControlSourceId.parse("media:a1"), .media("a1"), "media parses (assign refuses it, D15)")
        for bad in ["zoom:", "capture:", "Zoom:p", "ndi:x", "", "p-1"] {
            expect(ControlSourceId.parse(bad) == nil, "\(bad.debugDescription) is not a source id (Ordinal prefixes)")
        }
        expectEqual(ControlSourceId.canonical(kind: "zoom", sourceId: "p-1"), "zoom:p-1", "canonical zoom")
        expectEqual(ControlSourceId.canonical(kind: "capture", sourceId: "cam1"), "capture:cam1", "canonical capture")
        expect(ControlSourceId.canonical(kind: "unassigned", sourceId: "") == nil, "unassigned has no id")
        expect(ControlSourceId.canonical(kind: "zoom", sourceId: "") == nil, "a blank zoom slot has no id")
        for id in ["zoom:p-1", "capture:cam1", "media:a1"] {
            expectEqual(ControlSourceId.parse(id).map(ControlSourceId.canonical), id, "round trip \(id)")
        }
        func slot(_ kind: String, _ id: String) -> ShowInputSlot { var s = ShowInputSlot(id: 1); s.kind = kind; s.sourceId = id; return s }
        func device(_ id: String, _ vendor: String) -> CaptureDeviceRow {
            CaptureDeviceRow(id: id, name: id, kind: "video", vendor: vendor, connectionState: "connected", signalPresent: true, warning: "")
        }
        let devices = [device("screen:1", ""), device("browser:2", ""), device("bm", "Blackmagic"), device("aj", "AJA"),
                       device("s", "srt"), device("r", "rtmp"), device("n", "ndi"), device("cam", "Apple")]
        let expected: [(ShowInputSlot, String)] = [
            (slot("unassigned", ""), "Unassigned"), (slot("zoom", "p-1"), "ZoomParticipant"),
            (slot("capture", "screen:1"), "Screen"), (slot("capture", "browser:2"), "Browser"),
            (slot("capture", "bm"), "Blackmagic"), (slot("capture", "aj"), "Aja"), (slot("capture", "s"), "SrtIngest"),
            (slot("capture", "r"), "RtmpIngest"), (slot("capture", "n"), "Ndi"), (slot("capture", "cam"), "UvcWebcam"),
            (slot("capture", "gone"), "UvcWebcam"),
        ]
        for (s, kind) in expected {
            expectEqual(ControlSourceId.windowsKind(of: s, devices: devices), kind, "\(s.kind)/\(s.sourceId) -> \(kind)")
        }
    }

    @MainActor
    static func testControlRc2StateFields() {
        let model = sceneEditModel()
        model.roster = [RosterParticipant(id: "p-1", name: "Host", hasVideo: true, muted: false, talking: false, assigned: true),
                        RosterParticipant(id: "p-2", name: "Guest", hasVideo: false, muted: false, talking: false, assigned: false)]
        model.captureDevices = [CaptureDeviceRow(id: "cam1", name: "Desk Cam", kind: "video", vendor: "Apple",
                                                 connectionState: "connected", signalPresent: true, warning: "")]
        model.slots[0].kind = "zoom"; model.slots[0].sourceId = "p-1"; model.slots[0].name = "Host"; model.slots[0].inShow = true
        model.slots[1].kind = "capture"; model.slots[1].sourceId = "cam1"; model.slots[1].name = "Desk"; model.slots[1].inShow = false
        model.meetingState = "in_meeting"; model.captureEnabled = true; model.rawMediaActive = false
        let state = AppModelControlSurface.state(model).dictionary
        expectEqual(state["zoomStatus"] as? String, "Zoom Live", "Windows text while in a meeting")
        expectEqual(state["engineStatus"] as? String,
                    "Engine waiting for Zoom media access — ask the host to approve the request.", "Windows waiting text")
        expectEqual(state["zoomParticipantIds"] as? [String], ["p-1", "p-2"], "roster ids in roster order")
        expectEqual(state["virtualCameraOn"] as? Bool, false, "virtual camera intent")
        let inputs = state["inputs"] as? [[String: Any]] ?? []
        expectEqual(inputs.count, 10, "one row per slot")
        expect(NSDictionary(dictionary: inputs.first ?? [:]).isEqual(to: ["slot": 1, "inShow": true, "kind": "ZoomParticipant", "sourceId": "zoom:p-1", "name": "Host"]), "slot 1")
        expect(inputs.count > 1 && NSDictionary(dictionary: inputs[1]).isEqual(to: ["slot": 2, "inShow": false, "kind": "UvcWebcam", "sourceId": "capture:cam1", "name": "Desk"]), "slot 2")
        expect(inputs.count > 2 && inputs[2]["sourceId"] is NSNull && inputs[2]["kind"] as? String == "Unassigned", "an empty slot is Unassigned with a null id")
        // The Mac core publishes `in_meeting`; the hyphenated spelling is not a meeting.
        model.meetingState = "in-meeting"
        expectEqual(AppModelControlSurface.state(model).dictionary["zoomStatus"] as? String, "Zoom Offline", "only in_meeting is live")
        model.meetingState = "idle"; model.captureEnabled = false
        let offline = AppModelControlSurface.state(model).dictionary
        expectEqual(offline["zoomStatus"] as? String, "Zoom Offline", "Windows offline text")
        expectEqual(offline["engineStatus"] as? String, "Engine off", "engine off")
        // Every supported feedback field is published (D6), driven from the platform file.
        let rows = repoJSON("contracts/control.platforms.json")?["feedbackFields"] as? [[String: Any]] ?? []
        for row in rows where row["mac"] as? String == "supported" {
            let field = row["field"] as? String ?? ""
            let key = field.hasPrefix("input/") ? "inputs" : field
            expect(state[key] != nil, "supported feedback field \(field) is published as \(key)")
        }
        expect(rows.filter { $0["mac"] as? String == "supported" }.count >= 16, "RC2: at least 10 + zoomStatus, engineStatus, 4 input rows")
        // Budget: median of 50 builds with ten assigned slots.
        for i in 2..<10 { model.slots[i].kind = "zoom"; model.slots[i].sourceId = "p-\(i)"; model.slots[i].inShow = true }
        var micros: [Double] = []
        for _ in 0..<50 {
            let t0 = DispatchTime.now().uptimeNanoseconds
            _ = ControlJSON.encode(AppModelControlSurface.state(model).dictionary)
            micros.append(Double(DispatchTime.now().uptimeNanoseconds - t0) / 1000)
        }
        let median = micros.sorted()[25]
        ShellLog.write("control: /state build median \(Int(median)) us (10 slots)")
        FileHandle.standardError.write("control: /state build median \(Int(median)) us (10 slots)\n".data(using: .utf8)!)
        expect(median < 1000, "a /state build with ten inputs stays under 1 ms on the main actor (median \(median) us)")
    }

    @MainActor
    static func testControlPushKeyIgnoresMeters() {
        let base: [String: Any] = ["recording": false, "programLoudnessLufs": -23.0, "programTruePeakDbfs": -3.0,
                                   "audioSources": [["sourceId": "zoom:1", "name": "A", "level": 10, "muted": false, "status": "live"]]]
        var meters = base
        meters["programLoudnessLufs"] = -18.0; meters["programTruePeakDbfs"] = -1.0
        meters["audioSources"] = [["sourceId": "zoom:1", "name": "A", "level": 80, "peakDbfs": -2.0, "rmsDbfs": -9.0, "muted": false, "status": "live"]]
        expectEqual(ControlStatePushKey.key(base), ControlStatePushKey.key(meters), "meter-only changes produce the same key")
        var muted = meters
        muted["audioSources"] = [["sourceId": "zoom:1", "name": "A", "level": 80, "muted": true, "status": "live"]]
        expect(ControlStatePushKey.key(muted) != ControlStatePushKey.key(base), "a mute change is a push")
        var recording = base; recording["recording"] = true
        expect(ControlStatePushKey.key(recording) != ControlStatePushKey.key(base), "a non-meter field is a push")
        // End to end through the host's sink: 20 meter-only model changes push nothing.
        let model = sceneEditModel()
        // The strip exists before the host attaches: a source APPEARING is a real
        // change (since T15 audioSources lists strips); only its level is churn.
        model.audioStripLevelForTesting(id: "zoom:1", level: 0)
        let host = ControlApiHost()
        var pushed = 0
        host.broadcastSinkForTesting = { _ in pushed += 1 }
        host.attachForTesting(model: model)
        spin(until: { pushed >= 1 }, limit: 1)   // the first state is a push
        let first = pushed
        for i in 0..<20 { model.audioStripLevelForTesting(id: "zoom:1", level: i * 5); spin(until: { false }, limit: 0.02) }
        spin(until: { false }, limit: 0.3)
        expectEqual(pushed, first, "meter-only churn never pushes")
        // (plan: programBlackout.toggle(); that setter is private, so a published state field moves instead)
        model.meetingState = "in_meeting"
        spin(until: { pushed > first }, limit: 1)
        expectEqual(pushed, first + 1, "a real change pushes once")
        host.stop()
    }

    @MainActor
    static func inputsModel() -> AppModel {
        let model = sceneEditModel()
        model.roster = [RosterParticipant(id: "p-1", name: "Host", hasVideo: false, muted: false, talking: false, assigned: false),
                        RosterParticipant(id: "p-2", name: "Guest", hasVideo: false, muted: false, talking: false, assigned: false)]
        model.captureDevices = [CaptureDeviceRow(id: "cam1", name: "Desk Cam", kind: "video", vendor: "Apple",
                                                 connectionState: "connected", signalPresent: true, warning: "")]
        return model
    }

    @MainActor
    static func testControlInputActions() {
        let model = inputsModel()
        let s = AppModelControlSurface(model: model, box: model.controlSnapshotBox)
        expectEqual(invoke(s, "input.assign", [0, "zoom:p-1"]), .fail("Show Input slot 0 is out of range (1-10)."), "slot 0")
        expectEqual(invoke(s, "input.assign", [11, "zoom:p-1"]), .fail("Show Input slot 11 is out of range (1-10)."), "slot 11")
        expectEqual(invoke(s, "input.assign", [1, "ndi:x"]),
                    .fail("Unrecognized source id 'ndi:x' (expected zoom:/capture:/media:)."), "Windows unrecognized text")
        expectEqual(invoke(s, "input.assign", [1, "media:a1"]),
                    .fail("Media sources cannot be assigned to Show Inputs on macOS yet (Media clip playback)."), "media refused")
        expect(model.slots.allSatisfy { $0.kind == "unassigned" }, "refusals change nothing")

        expectEqual(invoke(s, "input.assign", [3, "zoom:p-2"]), .success, "assign zoom")
        expectEqual(model.slots[2].kind, "zoom", "kind"); expectEqual(model.slots[2].sourceId, "p-2", "bare id")
        expectEqual(model.slots[2].name, "Guest", "derived from the roster"); expect(model.slots[2].inShow, "assign puts it in the show")
        expectEqual(invoke(s, "input.assign", [4, "capture:cam1"]), .success, "assign capture")
        expectEqual(model.slots[3].name, "Desk Cam", "derived from the capture device")
        expectEqual(invoke(s, "input.assign", [5, "zoom:p-404"]), .success, "an id not in the roster is accepted (Windows does not check)")
        expectEqual(model.slots[4].name, "p-404", "no roster entry: the bare id")

        expectEqual(invoke(s, "input.name", [3, "  Anchor  "]), .success, "rename"); expectEqual(model.slots[2].name, "Anchor", "trimmed")
        expectEqual(invoke(s, "input.name", [3]), .success, "omitted name"); expectEqual(model.slots[2].name, "Guest", "blank resets to derived (D12)")
        expectEqual(invoke(s, "input.name", [3, "   "]), .success, "blank name"); expectEqual(model.slots[2].name, "Guest", "whitespace resets too")
        expectEqual(invoke(s, "input.name", [12, "x"]), .fail("Show Input slot 12 is out of range."), "name range text")

        expectEqual(invoke(s, "input.inShow.set", [3, false]), .success, "take out of show"); expect(!model.slots[2].inShow, "out")
        expectEqual(invoke(s, "input.inShow.set", [3, false]), .success, "idempotent"); expect(!model.slots[2].inShow, "still out")
        expectEqual(invoke(s, "input.inShow.set", [3, true]), .success, "back in"); expect(model.slots[2].inShow, "in")
        expectEqual(invoke(s, "input.inShow.set", [9, true]), .fail("Show Input slot 9 has no source."), "no source to show")
        expectEqual(invoke(s, "input.inShow.set", [0, true]), .fail("Show Input slot 0 is out of range."), "in-show range text")

        // Two slots carry p-2, so "every slot carrying the pid" is observable.
        model.slots[5].kind = "zoom"; model.slots[5].sourceId = "p-2"
        expectEqual(invoke(s, "source.dropout.set", ["zoom:p-2", "black"]), .success, "dropout black")
        expectEqual(model.slots[2].dropoutPolicy, "black", "applied to the slot carrying p-2")
        expectEqual(model.slots[5].dropoutPolicy, "black", "applied to every slot carrying p-2")
        model.slots[5] = ShowInputSlot(id: 6)
        expectEqual(invoke(s, "source.dropout.set", ["zoom:p-2", "freeze"]), .fail("policy must be 'hold' or 'black'."), "bad policy")
        expectEqual(invoke(s, "source.dropout.set", ["capture:cam1", "hold"]),
                    .fail("source.dropout.set: macOS takes a dropout policy for zoom:<pid> sources only (capture:cam1)"), "capture refused (D15)")
        expectEqual(invoke(s, "source.dropout.set", ["media:a1", "hold"]),
                    .fail("source.dropout.set: only zoom:<pid> and capture:<id> sources take a dropout policy (media:a1)"), "Windows text")
        expectEqual(invoke(s, "source.dropout.set", ["zoom:p-9", "hold"]), .fail("No Show Input carries 'zoom:p-9'."), "unslotted pid")

        expectEqual(invoke(s, "input.unassign", [3]), .success, "unassign")
        expectEqual(model.slots[2].kind, "unassigned", "cleared")
        expectEqual(invoke(s, "input.unassign", [99]), .fail("Show input slot is out of range."), "Windows unassign text (lower-case, no number)")
        // The remote unassign is the operator path: auto-assign must not refill it.
        model.roster = [RosterParticipant(id: "p-2", name: "Guest", hasVideo: true, muted: false, talking: false, assigned: false)]
        model.applyZoomForTesting(["meetingState": "in_meeting",
                                   "participants": [["userId": "p-2", "displayName": "Guest", "videoOn": true, "muted": false, "talking": false]]])
        expect(!model.slots.contains { $0.kind == "zoom" && $0.sourceId == "p-2" }, "a remotely unassigned guest is not auto-re-added")
    }

    @MainActor
    static func testControlTransitionAndEngine() {
        let core = FakeTakeCore()
        let model = takeModel(core)
        let s = AppModelControlSurface(model: model, box: model.controlSnapshotBox)
        expectEqual(invoke(s, "transport.transition.set", ["CUT"]), .success, "transition set")
        expectEqual(model.takeTransitionMode, "cut", "normalized like Windows")
        expectEqual(invoke(s, "transport.transition.set", ["sideways"]), .success, "unknown mode is not an error (Windows)")
        expectEqual(model.takeTransitionMode, "fade", "unknown normalizes to fade")
        _ = invoke(s, "transport.transition.set", ["dip"])
        expectEqual(invoke(s, "transport.take"), .success, "take")
        let take = (core.bodies.last?["commands"] as? [[String: Any]])?.first { $0["type"] as? String == "begin-take-transition" }
        expectEqual(take?["mode"] as? String, "dip", "the remote transition rides the Take batch")

        expect(!model.captureEnabled, "engine starts off")
        expectEqual(invoke(s, "transport.engine.set", [false]), .success, "set off while off is a no-op")
        expectEqual(invoke(s, "transport.engine.toggle"), .success, "toggle on"); expect(model.captureEnabled, "on")
        expectEqual(invoke(s, "transport.engine.set", [true]), .success, "set on while on"); expect(model.captureEnabled, "still on")
        expectEqual(invoke(s, "transport.engine.set", [false]), .success, "set off"); expect(!model.captureEnabled, "off")
        // Confirmatory only (rule 8): the zoom-stop-capture rides the bridge, which the seam does not carry.

        let offline = sceneEditModel()
        let o = AppModelControlSurface(model: offline, box: offline.controlSnapshotBox)
        expectEqual(invoke(o, "transport.engine.toggle"),
                    .fail("Cannot toggle capture engine right now (the control is unavailable in the current state)."), "no core: toggle refused")
        expectEqual(invoke(o, "transport.engine.set", [true]),
                    .fail("Cannot start capture engine right now (the control is unavailable in the current state)."), "no core: start refused")
        expect(!offline.captureEnabled, "nothing changed")
    }

    @MainActor
    static func testControlJoinLeaveAndCaptureOnline() {
        let core = FakeTakeCore()
        let model = AppModel()
        model.controlSettleSeconds = 0.4
        var answers: [JSONObject] = [["meetingState": "in_meeting", "warnings": [] as [String]]]
        model.quitRequestForTesting = { body, _ in
            _ = try await core.handle(body)
            return await MainActor.run { ["snapshot": answers.isEmpty ? [:] : answers.removeFirst()] }
        }
        let s = AppModelControlSurface(model: model, box: model.controlSnapshotBox)
        func joins() -> [JSONObject] { core.bodies.filter { $0["type"] as? String == "zoom-join" }.compactMap { $0["payload"] as? JSONObject } }

        expectEqual(invoke(s, "zoom.join", ["   "]), .fail("Zoom join is unavailable in the current state."), "blank target refused")
        expectEqual(joins().count, 0, "nothing sent")
        expectEqual(invoke(s, "zoom.join", ["https://us02web.zoom.us/j/97682593786?pwd=abc", "Remote Desk", true]), .success, "join succeeds")
        expectEqual(joins().first?["meetingId"] as? String, "97682593786", "URL parsed like the UI")
        expectEqual(joins().first?["passcode"] as? String, "abc", "passcode from pwd")
        expectEqual(joins().first?["displayName"] as? String, "Remote Desk", "display name applied")
        expectEqual(joins().first?["webinar"] as? Bool, true, "webinar is the UI's field (D8)")
        expectEqual(model.meetingState, "in_meeting", "model in meeting")

        answers = [["meetingState": "error", "warnings": [accountBusySentence]]]
        expectEqual(invoke(s, "zoom.join", ["97682593786"]), .fail(accountBusySentence), "a failed join answers with Zoom's reason")
        answers = [["meetingState": "joining"]]
        expectEqual(invoke(s, "zoom.join", ["97682593786"]),
                    .fail("Zoom join is still in progress (joining); read zoomStatus for the outcome."), "an unsettled join says so")
        // The settle wait is observable: the meeting lands 150 ms AFTER the reply (a later
        // snapshot), inside the window, so the join answers success, not "still in progress".
        // A wide window here so machine load cannot make the late snapshot miss it; the
        // window's length is not what this pins (no wait at all reads "joining").
        model.controlSettleSeconds = 2.5
        answers = [["meetingState": "joining"]]
        Task { @MainActor in
            try? await Task.sleep(nanoseconds: 150_000_000)
            model.applyZoomForTesting(["meetingState": "in_meeting"])
        }
        expectEqual(invoke(s, "zoom.join", ["97682593786"]), .success, "a join that settles inside the window succeeds")
        model.controlSettleSeconds = 0.4

        let noCore = AppModel()
        expectEqual(invoke(AppModelControlSurface(model: noCore, box: noCore.controlSnapshotBox), "zoom.join", ["123"]),
                    .fail("Zoom join is unavailable in the current state."), "no core")

        model.applyZoomForTesting(["meetingState": "in_meeting"])
        answers = [["meetingState": "idle"]]
        expectEqual(invoke(s, "zoom.leave"), .success, "leave succeeds")
        expect(core.bodies.contains { $0["type"] as? String == "zoom-leave" }, "leave went through the seam")
        let leavesBefore = core.bodies.filter { $0["type"] as? String == "zoom-leave" }.count
        expectEqual(invoke(s, "zoom.leave"), .fail("Zoom leave is unavailable in the current state."), "not in a meeting")
        expectEqual(core.bodies.filter { $0["type"] as? String == "zoom-leave" }.count, leavesBefore, "nothing sent when idle")

        // Capture online/offline through the capture seam.
        model.captureDevices = [CaptureDeviceRow(id: "cam1", name: "Desk Cam", kind: "video", vendor: "Apple",
                                                 connectionState: "detected", signalPresent: false, warning: "")]
        var deviceState = "detected"
        model.captureRequestForTesting = { body in
            switch body["type"] as? String {
            case "connect-capture-device": deviceState = "connected"
            case "disconnect-capture-device": deviceState = "detected"
            default: break
            }
            return ["snapshot": ["captureDevices": [["id": "cam1", "name": "Desk Cam", "kind": "video", "vendor": "Apple",
                                                     "connectionState": deviceState, "signalPresent": true, "warning": ""]]]]
        }
        expectEqual(invoke(s, "source.capture.online.set", ["zoom:p-1", true]), .fail("Choose a capture source ID."), "not a capture id")
        expectEqual(invoke(s, "source.capture.online.set", ["capture:nope", true]), .fail("The capture device is unavailable."), "unknown device")
        expectEqual(invoke(s, "source.capture.online.set", ["capture:cam1", true]), .success, "connect")
        expectEqual(model.captureDevices.first?.connectionState, "connected", "the barrier's snapshot was applied")
        expectEqual(invoke(s, "source.capture.online.set", ["capture:cam1", true]), .success, "already connected: no toggle")
        expectEqual(model.captureDevices.first?.connectionState, "connected", "a matching request never disconnects")
        expectEqual(invoke(s, "source.capture.online.set", ["capture:cam1", false]), .success, "offline")
        expectEqual(model.captureDevices.first?.connectionState, "detected", "offline")
        model.captureRequestForTesting = { _ in ["snapshot": ["captureDevices": [["id": "cam1", "name": "Desk Cam", "kind": "video", "vendor": "Apple",
                                                                                   "connectionState": "failed", "signalPresent": false, "warning": ""]]]] }
        expectEqual(invoke(s, "source.capture.online.set", ["capture:cam1", true]),
                    .fail("Capture device 'cam1' is failed, not connected."), "a connect the core did not honour is reported")
    }

    @MainActor
    static func testControlRtmpConfigure() {
        func fail(_ args: [Any?], _ text: String, busy: Bool = false, _ why: String) {
            let r = RtmpDestinationPolicy.configure(args: args, busy: busy, currentKey: "saved-key")
            if case .failure(let e) = r { expectEqual(e.message, text, why) } else { expect(false, "\(why): expected refusal") }
        }
        let good: [Any?] = [true, "rtmps", "rtmps://example.test/live", "test-key-sentinel"]
        fail(good, "Stop streaming and wait for the transition to finish before configuring destinations.", busy: true, "busy")
        fail([false, "rtmps", "", ""], "macOS streams to one RTMP destination; it cannot be disabled. Send enabled=true to configure it.", "disable (D9)")
        fail([true, "invalid-mode-test-secret", "example.test/live", "k"], "RTMP protocol must be rtmp or rtmps.", "protocol")
        fail([true, "rtmps", "rtmps://user:pw@example.test/live", "k"],
             "Put RTMP credentials in streamKey; serverUrl cannot contain user info, a query, or a fragment.", "user info")
        fail([true, "rtmps", "example.test/live?key=x", "k"],
             "Put RTMP credentials in streamKey; serverUrl cannot contain user info, a query, or a fragment.", "query")
        fail([true, "rtmp", "rtmps://live.example.com/app", "k"], "RTMP protocol selection must match the server URL scheme.", "scheme mismatch")
        fail([true, "rtmps", "live.example.com", "k"], "RTMP server URL must include an application path.", "no app path")
        fail([true, "rtmps", "live.example.com/app", "stream key"], "RTMP stream key cannot contain whitespace.", "whitespace key")
        fail([true, "rtmps", "live.example.com/app", ""], "Configure RTMP stream key before streaming.", "explicit empty key while enabled")
        fail([true, "rtmps", "", "k"], "Configure RTMP server URL before streaming.", "no URL")
        for secret in ["invalid-mode-test-secret", "pw", "key=x"] {
            for args in [[true, "invalid-mode-test-secret", "example.test/live", "k"], [true, "rtmps", "rtmps://user:pw@example.test/live?key=x", "k"]] as [[Any?]] {
                if case .failure(let e) = RtmpDestinationPolicy.configure(args: args, busy: false, currentKey: "") {
                    expect(!e.message.contains(secret), "no refusal echoes caller data (\(secret))")
                }
            }
        }
        expectEqual(RtmpDestinationPolicy.buildUrl(protocol: "rtmps", serverUrl: "live.example.com/app"), "rtmps://live.example.com/app", "Windows BuildRtmpUrl")
        expectEqual(try? RtmpDestinationPolicy.configure(args: [true, "RTMPS ", "live.example.com/app"], busy: false, currentKey: "saved-key").get(),
                    .init(url: "rtmps://live.example.com/app", key: "saved-key"), "omitted key keeps the saved key")

        let model = takeModel(FakeTakeCore())
        var saved: [String] = []
        model.streamKeySaver = { saved.append($0); return true }
        let s = AppModelControlSurface(model: model, box: model.controlSnapshotBox)
        expectEqual(invoke(s, "transport.stream.rtmp.configure", good), .success, "configure")
        expectEqual(model.streamUrl, "rtmps://example.test/live", "URL applied"); expectEqual(model.streamKey, "test-key-sentinel", "key applied")
        expectEqual(saved, ["test-key-sentinel"], "the key went to the Keychain seam, never to prefs")
        expect(!model.streamingDesired, "configuring never starts streaming")
        model.streamKeySaver = { _ in false }
        expectEqual(invoke(s, "transport.stream.rtmp.configure", [true, "rtmp", "rtmp://other.test/app", "new-key"]),
                    .fail("Destination settings were not saved. Previous settings remain active; retry after resolving preference storage."), "storage failure")
        expectEqual(model.streamUrl, "rtmps://example.test/live", "a failed save changes nothing"); expectEqual(model.streamKey, "test-key-sentinel", "key kept")
    }

    @MainActor
    static func testControlProductionActions() {
        let model = takeModel(FakeTakeCore())   // Program s2
        var sent: [JSONObject] = []
        model.coreCommandCaptureForTesting = { sent.append($0) }
        // Overlay commands are observed through the model's own overlay seam (rule 5:
        // sendOverlay does not go through coreCommandCaptureForTesting).
        var overlays: [JSONObject] = []
        model.overlayCommandObserver = { overlays.append($0) }
        let s = AppModelControlSurface(model: model, box: model.controlSnapshotBox)
        func types() -> [String] { sent.flatMap { ($0["commands"] as? [JSONObject]) ?? [$0] }.compactMap { $0["type"] as? String } }

        // Lower third: no Program source -> Windows-shaped refusal with the model's own reason.
        expectEqual(invoke(s, "graphics.lowerThird.set", [true]), .fail(LowerThirdKeyReadout.needsSource), "no Program source")
        // With a Program source the remote key-in is the button's command, byte for byte.
        // (rule 5: the subject comes from programFrame.videoSources, ProgramLowerThirdSource.resolve)
        model.slots[0].kind = "zoom"; model.slots[0].sourceId = "p-1"; model.slots[0].name = "Host"; model.slots[0].inShow = true
        model.roster = [RosterParticipant(id: "p-1", name: "Host", hasVideo: true, muted: false, talking: false, assigned: true)]
        model.applySnapshotForTesting(["programFrame": ["frameNumber": 10, "sceneId": "s2",
            "videoSources": [["sourceId": "zoom:p-1", "participantId": "p-1", "kind": "participant-video", "layerId": "l1"]]]])
        overlays.removeAll()
        model.showLowerThird()
        let buttonCommand = overlays.last
        model.hideLowerThird(); spin(until: { false }, limit: 0.3); overlays.removeAll()
        expectEqual(invoke(s, "graphics.lowerThird.set", [true]), .success, "key in accepted")
        expect(buttonCommand != nil && NSDictionary(dictionary: overlays.last ?? [:]).isEqual(to: buttonCommand ?? [:]),
               "the remote key-in sends exactly the button's set-overlay-asset")
        expectEqual(invoke(s, "graphics.lowerThird.set", [false]), .success, "key out")
        expectEqual(overlays.last?["keyPhase"] as? String, "building-out", "key out starts the build-out")

        expectEqual(invoke(s, "automation.captions.set", [true]),
                    .fail("Automatic captions are unavailable: CoreVideo does not transcribe speech. Use explicit caption cues from a control client or the show engine."),
                    "Windows text")
        expectEqual(invoke(s, "automation.captions.set", [false]), .success, "false is a no-op success")

        expectEqual(invoke(s, "multiview.layout.set", ["pgmPvwSide"]), .success, "layout"); expectEqual(model.multiviewLayoutMode, "pgmPvwSide", "applied")
        expectEqual(invoke(s, "multiview.layout.set", ["mosaic"]), .success, "unknown layout"); expectEqual(model.multiviewLayoutMode, "pgmPvwTop", "normalizes to pgmPvwTop (Windows)")
        for (asked, got) in [(0, 1), (12, 12), (40, 16)] {
            expectEqual(invoke(s, "multiview.tileCount.set", [asked]), .success, "tile count \(asked)")
            expectEqual(model.multiviewTileCount, got, "\(asked) clamps to \(got) (Mac range 1...16, D11)")
        }
        expect(types().contains("configure-multiviewer"), "the tile count reached the core")

        expectEqual(invoke(s, "audio.mastering.set", [true]), .success, "mastering on"); expect(model.mastering.enabled, "on")
        for (index, lufs) in [(0, -14.0), (1, -16.0), (2, -23.0), (7, -23.0), (-3, -14.0)] {
            _ = invoke(s, "audio.mastering.target", [index])
            expectEqual(model.mastering.targetLufs, lufs, "index \(index) -> \(lufs) LUFS (clamped 0...2)")
        }
        var state = AppModelControlSurface.state(model).dictionary
        expectEqual(state["masteringTarget"] as? Int, 0, "-14 is index 0")
        model.editMastering { $0.targetLufs = -18 }
        state = AppModelControlSurface.state(model).dictionary
        expectEqual(state["masteringTarget"] as? Int, -1, "a custom target is -1 (D10)")
        expectEqual(state["masteringOn"] as? Bool, true, "masteringOn"); expectEqual(state["autoCaptions"] as? Bool, false, "autoCaptions false")
        expectEqual(state["multiviewTileCount"] as? Int, 16, "multiviewTileCount")
        expectEqual(state["lowerThirdOnAir"] as? Bool, model.lowerThirdVisible, "confirmatory: mirrors the model field (rule 8)")
        // rule 5: the Mac reads overlayState.overlays[] (AppModel.applySnapshot).
        model.applySnapshotForTesting(["overlayState": ["overlays": [["overlayId": "key:lower-third", "visible": true, "keyPhase": "on-air"]]]])
        expectEqual(AppModelControlSurface.state(model).dictionary["lowerThirdOnAir"] as? Bool, true, "on air once the core says visible")

        expectEqual(invoke(s, "audio.vst.scan"), .success, "scan accepted with a core")
        let noCore = AppModel()
        expectEqual(invoke(AppModelControlSurface(model: noCore, box: noCore.controlSnapshotBox), "audio.vst.scan"),
                    .fail("VST3 scan is unavailable: no media core."), "no core: no false success")
    }

    /// Plays the core's monitor-control contract (node name audioMixSession, which
    /// AppModel.applySnapshot reads, rule 5).
    final class FakeMonitorCore: @unchecked Sendable {
        var epoch = "core-1", revision: Int64 = 3, enabled = false, volume = 0.7, results: [JSONObject] = []
        var forceStatus: String?   // "conflict" / "rejected" / nil (apply)
        var answer = true
        func handle(_ body: JSONObject) -> JSONObject {
            for c in (body["commands"] as? [JSONObject]) ?? [] where c["type"] as? String == "set-audio-monitor-control" {
                let op = c["operationId"] as? String ?? ""
                let ok = c["authorityEpoch"] as? String == epoch && (c["expectedRevision"] as? NSNumber)?.int64Value == revision
                let status = forceStatus ?? (ok ? "applied" : "conflict")
                if status == "applied" { enabled = c["enabled"] as? Bool ?? enabled; volume = (c["volume"] as? NSNumber)?.doubleValue ?? volume; revision += 1 }
                results.append(["operationId": op, "status": status])
            }
            guard answer else { return ["snapshot": [:] as JSONObject] }
            return ["snapshot": ["audioMixSession": ["monitorEnabled": enabled, "monitorVolume": volume,
                                                     "monitorControl": ["authorityEpoch": epoch, "revision": revision, "recentResults": results]]]]
        }
    }

    @MainActor
    static func testControlMonitorRevisioned() {
        let fake = FakeMonitorCore()
        let model = AppModel()
        model.controlSettleSeconds = 0.4
        model.quitRequestForTesting = { body, _ in await MainActor.run { fake.handle(body) } }
        model.status = .connected(renderer: "test")
        let s = AppModelControlSurface(model: model, box: model.controlSnapshotBox)
        // A second client that read epoch core-1 at revision 3 applies.
        expectEqual(invoke(s, "audio.monitor.set", [true, "core-1", 3, "client-a"]), .success, "applied at the expected revision")
        expectEqual(fake.results.last?["operationId"] as? String, "client-a", "the caller's operation id reached the core")
        var state = AppModelControlSurface.state(model).dictionary
        expectEqual(state["audioMonitorOn"] as? Bool, true, "applied state"); expectEqual(state["audioMonitorRevision"] as? Int64, 4, "revision moved to 4")
        expectEqual(state["audioMonitorAuthorityEpoch"] as? String, "core-1", "epoch")
        // A stale revision conflicts (port of CT ControlActionRegistryTests:106 intent).
        expectEqual(invoke(s, "audio.monitor.volume", [0.3, "core-1", 3, "client-b"]),
                    .fail("Monitor control conflict at revision 4; read state and retry with the current epoch and revision."), "conflict")
        state = AppModelControlSurface.state(model).dictionary
        expectEqual(state["audioMonitorVolume"] as? Double, 0.7, "a conflict changes nothing")
        // A conflicting SET leaves the applied on-state alone too (state reads the core, not local intent).
        expectEqual(invoke(s, "audio.monitor.set", [false, "core-1", 3, "client-c"]),
                    .fail("Monitor control conflict at revision 4; read state and retry with the current epoch and revision."), "conflicting set")
        expectEqual(AppModelControlSurface.state(model).dictionary["audioMonitorOn"] as? Bool, true, "a conflicting set changes nothing")
        // Legacy shape: no epoch/revision -> the current values are used (Windows binds them as null).
        expectEqual(invoke(s, "audio.monitor.volume", [1.7]), .success, "legacy volume applies")
        expectEqual(model.monitorControl.applied.volume, 1.0, "volume clamps to 1.0 (Windows Math.Clamp)")
        expectEqual(invoke(s, "audio.monitor.volume", [0.4, "core-1", -1]), .fail("expectedRevision must be a nonnegative safe integer."), "negative revision")
        expectEqual(invoke(s, "audio.monitor.volume", [0.4, "core-1", 2.5]), .fail("expectedRevision must be a nonnegative safe integer."), "fractional revision")
        fake.forceStatus = "rejected"
        expectEqual(invoke(s, "audio.monitor.set", [false]), .fail("Monitor control rejected; application is unconfirmed."), "rejected")
        fake.forceStatus = nil; fake.answer = false
        expectEqual(invoke(s, "audio.monitor.set", [false]), .fail("Monitor control unconfirmed; application is unconfirmed."), "no result inside the settle window")
        // The UI's local intent moves at once; /state still reports what the core applied.
        model.setMonitor(enabled: false, volume: 0.5)
        spin(until: { false }, limit: 0.2)
        expect(!model.monitorEnabled, "the local intent moved")
        expectEqual(AppModelControlSurface.state(model).dictionary["audioMonitorOn"] as? Bool, true,
                    "audioMonitorOn is the core-applied value, not local intent")
    }

    /// Plays the core's route-control contract (node audioRoutingMatrix, which
    /// AppModel.applySnapshot reads, rule 5).
    final class FakeRouteCore: @unchecked Sendable {
        var epoch = "core-1", revision: Int64 = 2, sends: [JSONObject] = [], results: [JSONObject] = []
        func handle(_ body: JSONObject) -> JSONObject {
            for c in (body["commands"] as? [JSONObject]) ?? [] where c["type"] as? String == "set-audio-route-control" {
                let ok = c["authorityEpoch"] as? String == epoch && (c["expectedRevision"] as? NSNumber)?.int64Value == revision
                if ok {
                    sends.removeAll { $0["sourceId"] as? String == c["sourceId"] as? String && $0["busId"] as? String == c["busId"] as? String }
                    if c["enabled"] as? Bool == true { sends.append(["sourceId": c["sourceId"]!, "busId": c["busId"]!, "gainDb": c["gainDb"]!]) }
                    revision += 1
                }
                results.append(["operationId": c["operationId"] as? String ?? "", "status": ok ? "applied" : "conflict"])
            }
            return ["snapshot": ["audioRoutingMatrix": ["sends": sends, "control": ["authorityEpoch": epoch, "revision": revision, "recentResults": results]]]]
        }
    }

    @MainActor
    static func testControlAudioRoute() {
        let fake = FakeRouteCore()
        let model = AppModel()
        model.controlSettleSeconds = 0.4
        model.quitRequestForTesting = { body, _ in await MainActor.run { fake.handle(body) } }
        model.status = .connected(renderer: "test")
        model.applySnapshotForTesting(fake.handle([:])["snapshot"] as? JSONObject ?? [:])   // learn epoch core-1 / revision 2
        let s = AppModelControlSurface(model: model, box: model.controlSnapshotBox)
        expectEqual(invoke(s, "audio.route.set", ["zoom:42", "mon", true, -6, "core-1", 2, "client-b"]), .success, "applied")
        var state = AppModelControlSurface.state(model).dictionary
        let routes = state["audioRoutes"] as? [[String: Any]] ?? []
        expect(NSArray(array: routes).isEqual(to: [["sourceId": "zoom:42", "busId": "mon", "gainDb": -6.0]]), "audioRoutes is the core's applied set")
        expectEqual(state["audioRouteRevision"] as? Int64, 3, "revision 3")
        expectEqual(model.audioSends["zoom:42"]?["mon"], -6, "the UI grid shows the remote edit")
        expectEqual(invoke(s, "audio.route.set", ["zoom:42", "mon", true, -3, "core-1", 2, "client-c"]),
                    .fail("Audio route conflict at revision 3; read state and retry."), "stale revision conflicts")
        for (args, text) in [
            (["", "mon", true, 0, "core-1", 3, "op"], "sourceId, busId, authorityEpoch and operationId are required."),
            (["zoom:42", "mon", true, 0, "core-1", 3, " "], "sourceId, busId, authorityEpoch and operationId are required."),
            (["zoom:42", "mon", true, 11, "core-1", 3, "op"], "gainDb must be between -60 and +10."),
            (["zoom:42", "mon", true, -61, "core-1", 3, "op"], "gainDb must be between -60 and +10."),
            (["zoom:42", "mon", true, 0, "core-1", 3.5, "op"], "expectedRevision must be a nonnegative safe integer."),
            (["zoom:42", "booth", true, 0, "core-1", 3, "op"], "busId must be one of master, pgm-l, pgm-r, stream, mon, iso-1."),
        ] as [([Any?], String)] {
            expectEqual(invoke(s, "audio.route.set", args), .fail(text), text)
        }
        expectEqual(invoke(s, "audio.route.set", ["zoom:42", "mon", false, 0, "core-1", 3, "client-d"]), .success, "un-route")
        state = AppModelControlSurface.state(model).dictionary
        expectEqual((state["audioRoutes"] as? [Any])?.count, 0, "removed")
        // audioSources: names from slots, no peak/rms keys, sorted.
        model.slots[0].kind = "zoom"; model.slots[0].sourceId = "42"; model.slots[0].name = "Host"
        model.audioStripLevelForTesting(id: "zoom:42", level: 55)
        model.audioStripLevelForTesting(id: "capture:mic", level: 10)
        let sources = AppModelControlSurface.state(model).dictionary["audioSources"] as? [[String: Any]] ?? []
        expectEqual(sources.compactMap { $0["sourceId"] as? String }, ["capture:mic", "zoom:42"], "sorted by id")
        expectEqual(sources.last?["name"] as? String, "Host", "slot name"); expectEqual(sources.first?["name"] as? String, "capture:mic", "else the id")
        expect(sources.allSatisfy { $0["peakDbfs"] == nil && $0["rmsDbfs"] == nil }, "no fabricated peak/RMS on the Mac")
        // End-to-end push key (deferred from T7): level churn pushes nothing.
        let host = ControlApiHost()
        var pushed = 0
        host.broadcastSinkForTesting = { _ in pushed += 1 }
        host.attachForTesting(model: model)
        spin(until: { pushed >= 1 }, limit: 1)
        let first = pushed
        for i in 0..<20 { model.audioStripLevelForTesting(id: "zoom:42", level: i * 5); spin(until: { false }, limit: 0.02) }
        spin(until: { false }, limit: 0.3)
        expectEqual(pushed, first, "meter churn on audioSources never pushes")
        host.stop()
        // Before the core answers, the grid still shows the remote edit as a draft.
        model.quitRequestForTesting = { _, _ in [:] }
        expectEqual(invoke(s, "audio.route.set", ["zoom:42", "master", true, -2, "core-1", 4, "client-e"]),
                    .fail("Audio route unconfirmed; application is unconfirmed."), "no answer is unconfirmed")
        expectEqual(model.audioSends["zoom:42"]?["master"], -2, "the grid shows the unconfirmed remote edit")
    }

    @MainActor
    static func testControlOscFeedbackCoversEverySupportedField() {
        let supported = ControlManifestGenerated.feedbackFields.filter { $0.availability == .supported }.map(\.field)
        var state: [String: Any] = ["inputs": [["slot": 7, "inShow": true, "kind": "ZoomParticipant", "sourceId": "zoom:x", "name": "N"]]]
        for field in supported where !field.hasPrefix("input/") { state[field] = field == "multiviewTileCount" ? 9 : (field == "audioMonitorVolume" ? 0.25 : "v-\(field)") }
        let addresses = Set(ControlOscFeedback.encode(state).map(\.address))
        for field in supported {
            let address = ControlOscAddressMap.stateAddress(field.replacingOccurrences(of: "{slot}", with: "7"))
            expect(addresses.contains(address), "supported feedback field \(field) is encoded (\(address))")
        }
        expectEqual(supported.count, 21, "after RC3: 21 supported feedback rows")
    }
}
