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
        expectEqual(rows.filter { $0["mac"] as? String == "supported" }.count, 16, "RC2: 10 + zoomStatus, engineStatus, 4 input rows")
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
        answers = [["meetingState": "joining"]]
        Task { @MainActor in
            try? await Task.sleep(nanoseconds: 150_000_000)
            model.applyZoomForTesting(["meetingState": "in_meeting"])
        }
        expectEqual(invoke(s, "zoom.join", ["97682593786"]), .success, "a join that settles inside the window succeeds")

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
}
