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
}
