// Comms (Zoom talkback), plan 2026-11-22 Tasks 6-10. The fixture is
// contracts/comms/zoom-comms-state.fixtures.json, read by this file AND by
// native-shell/CoreVideoPro.MediaCore.Tests/ZoomCommsProtocolTests.cs
// (SharedFixtureParses): the drift guard between the two parsers.

import Foundation

extension ShellTests {
    static var commsCases: [(String, () -> Void)] {
        [
            ("comms/protocol-desk-fixture", testCommsProtocolDeskFixture),
            ("comms/protocol-not-host", testCommsProtocolNotHost),
            ("comms/protocol-captured-sixteen-channels", testCommsProtocolCapturedSixteen),
            ("comms/protocol-null-is-dropped", testCommsProtocolNullIsDropped),
            ("comms/protocol-response-is-not-a-fact", testCommsProtocolResponseIsNotAFact),
            ("comms/protocol-private-flag", testCommsProtocolPrivateFlag),
            ("comms/protocol-mic-error", testCommsProtocolMicError),
            ("comms/protocol-verbs", testCommsProtocolVerbs),
            ("comms/handle-event-forwards-the-fact", testCommsHandleEventForwardsTheFact),
            ("comms/engine-path-preset-wins", testCommsEnginePathPresetWins),
        ]
    }

    // ── fixture ──────────────────────────────────────────────────────────

    static func commsFixtureEvent(_ name: String) -> JSONObject {
        let url = repoRoot.appendingPathComponent("contracts/comms/zoom-comms-state.fixtures.json")
        guard let data = try? Data(contentsOf: url),
              let doc = try? JSONSerialization.jsonObject(with: data) as? JSONObject,
              let facts = doc["facts"] as? [JSONObject],
              let line = facts.first(where: { $0["name"] as? String == name })?["line"] as? String,
              let event = try? JSONSerialization.jsonObject(with: Data(line.utf8)) as? JSONObject
        else { expect(false, "fixture fact \(name) must be readable"); return [:] }
        return event
    }

    static func commsFixture(_ name: String) -> CommsState?? { ZoomCommsProtocol.parseFact(commsFixtureEvent(name)) }

    // ── parser ───────────────────────────────────────────────────────────

    private static func testCommsProtocolDeskFixture() {
        guard let parsed = commsFixture("desk"), let s = parsed else { expect(false, "desk parses to a state"); return }
        expectEqual(s.channels.count, 7, "seven channels")
        expectEqual(s.channels.map(\.slot), [0, 1, 2, 3, 4, 5, 6], "slots 0-6")
        expectEqual(s.channels[0].label, "Ana Lucia Ferreira", "label")
        expectEqual(s.channels[4].label, nil, "null label stays nil")
        expectEqual(s.channels[3].room, "Green room", "room")
        expectEqual(s.channels[4].reachDark, 1, "reachDark")
        expectEqual(s.mic.peakDbfs, -18, "peak")
        expectEqual(s.mic.gainDb, 3, "gain")
        expectEqual(s.mic.sidetoneDevices, ["Headphones (USB Audio)", "Speakers"], "sidetone devices")
        expectEqual(s.sendErrors, 2, "send errors")
        expectEqual(s.framesSent, 51200, "frames sent")
        expectEqual(s.roster.map(\.participantId), [201, 202, 205], "roster ids")
        expect(!s.roster[2].talkback, "Wes is web client")
        expectEqual(s.roster[0].channels, [0, 4], "roster channels")
        expect(s.lampLink && s.lampMeetingMic && s.lampChannel && s.lampTx, "lamps")
        expectEqual(s.log.last, "21:40:15 audio flowing into CH 1", "log order kept")
        expect(!s.isPrivate, "private absent = false")
    }

    private static func testCommsProtocolNotHost() {
        guard let parsed = commsFixture("not-host"), let s = parsed else { expect(false, "not-host parses"); return }
        expect(!s.available, "unavailable")
        expectEqual(s.reason, "Make this account host or co-host to bring talkback up.", "reason verbatim")
        expect(s.lampLink && !s.lampMeetingMic, "lamps")
        expect(s.channels.isEmpty && s.roster.isEmpty, "absent arrays are empty")
        expectEqual(s.mic.peakDbfs, nil, "absent peak stays nil")
    }

    private static func testCommsProtocolCapturedSixteen() {
        guard let parsed = commsFixture("probe-joined"), let s = parsed else { expect(false, "probe-joined parses"); return }
        expectEqual(s.channels.count, 16, "16 slots")
        expect(s.available, "captured bank is up")
        guard let keyedFact = commsFixture("probe-keyed"), let k = keyedFact else { expect(false, "probe-keyed parses"); return }
        expect(k.channels.contains { $0.keyed }, "a captured held key")
    }

    private static func testCommsProtocolNullIsDropped() {
        let r = ZoomCommsProtocol.parseFact(commsFixtureEvent("dropped"))
        expect(r != nil, "comms:null is a fact")
        if let r { expect(r == nil, "the fact is 'dropped'") }
    }

    private static func testCommsProtocolResponseIsNotAFact() {
        expect(ZoomCommsProtocol.parseFact(commsFixtureEvent("response-not-fact")) == nil, "id = response")
        expect(ZoomCommsProtocol.parseFact(["type": "zoom-comms-state", "comms": "nope"]) == nil, "comms must be object or null")
        expect(ZoomCommsProtocol.parseFact(["type": "other", "comms": NSNull()]) == nil, "other event types are not comms")
    }

    private static func testCommsProtocolPrivateFlag() {
        guard let parsed = commsFixture("probe-private-on"), let s = parsed else { expect(false, "private parses"); return }
        expect(s.isPrivate, "private true")
    }

    private static func testCommsProtocolMicError() {
        let event: JSONObject = ["type": "zoom-comms-state", "comms": [
            "available": true, "mic": ["error": "Microphone access is off for CoreVideo Pro.", "peakDbfs": NSNull()],
        ] as JSONObject]
        guard let parsed = ZoomCommsProtocol.parseFact(event), let s = parsed else { expect(false, "parses"); return }
        expectEqual(s.mic.error, "Microphone access is off for CoreVideo Pro.", "mic error verbatim")
        expectEqual(s.mic.peakDbfs, nil, "null peak is nil")
        guard let joined = commsFixture("probe-joined"), let j = joined else { return }
        expectEqual(j.mic.error, nil, "captured null error is nil")
        let clamped = ZoomCommsProtocol.parseState(["roster": [["participantId": -5, "name": "x", "channels": [1, "2", true, 3]]]])
        expectEqual(clamped.roster.first?.participantId, 0, "participantId clamped at 0")
        expectEqual(clamped.roster.first?.channels, [1, 3], "roster channels keep numbers only")
    }

    private static func testCommsProtocolVerbs() {
        expectEqual(ZoomCommsProtocol.verbs, ["comms-key", "comms-latch", "comms-assign", "comms-mic",
                                              "comms-sidetone-out", "comms-sidetone", "comms-tone", "comms-gain",
                                              "comms-release-all", "comms-private"], "ten verbs")
    }

    @MainActor
    private static func testCommsHandleEventForwardsTheFact() {
        let m = AppModel()
        m.handleEventForTesting(commsFixtureEvent("not-host"))
        expectEqual(m.comms.state?.reason, "Make this account host or co-host to bring talkback up.", "fact reached CommsModel")
    }

    private static func testCommsEnginePathPresetWins() {
        var paths = ShellPaths.Paths()
        paths.engineBinaryPath = "/y/real"
        let preset = CoreLaunchEnvironment.extras(paths: paths, processEnvironment: ["COREVIDEO_ZOOM_ENGINE_PATH": "/x/fake"],
                                                  zoomPublicAppKey: nil)
        expectEqual(preset["COREVIDEO_ZOOM_ENGINE_PATH"], "/x/fake", "pre-set engine path wins (Windows run-studio rule)")
        let found = CoreLaunchEnvironment.extras(paths: paths, processEnvironment: [:], zoomPublicAppKey: nil)
        expectEqual(found["COREVIDEO_ZOOM_ENGINE_PATH"], "/y/real", "otherwise the found engine")
        let empty = CoreLaunchEnvironment.extras(paths: paths, processEnvironment: ["COREVIDEO_ZOOM_ENGINE_PATH": ""],
                                                 zoomPublicAppKey: nil)
        expectEqual(empty["COREVIDEO_ZOOM_ENGINE_PATH"], "/y/real", "an empty preset is not a preset")
    }
}
