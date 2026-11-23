// CommsModel, plan 2026-11-22 Task 7: a one-for-one port of
// native-shell/CoreVideoPro.WinUI.Tests/CommsViewModelTests.cs, with the same
// literals. Desk()/Channel() are the C# helpers (:39-44) with the same
// defaults; payloads compare as sorted-key JSON, as the C# FakeTransport does.

import Foundation

extension ShellTests {
    static var commsModelCases: [(String, () -> Void)] {
        [
            ("comms/model-screen-states-are-explicit", testCommsModelScreenStates),
            ("comms/model-only-occupied-named-or-keyed-channels-are-cells", testCommsModelOnlyOccupiedCells),
            ("comms/model-holding-a-cell-keys-with-a-lease", testCommsModelHoldingKeysWithALease),
            ("comms/model-on-air-comes-from-the-engine-only", testCommsModelOnAirFromEngineOnly),
            ("comms/model-latch-mode-toggles", testCommsModelLatchMode),
            ("comms/model-all-call-keys-every-channel-and-latches-all", testCommsModelAllCall),
            ("comms/model-same-cells-never-rebuild", testCommsModelSameCellsNeverRebuild),
            ("comms/model-structure-change-keeps-surviving-cells-and-hold", testCommsModelStructureChangeKeepsHold),
            ("comms/model-release-all-releases-every-held-key", testCommsModelReleaseAll),
            ("comms/model-held-key-renews-lease-released-does-not", testCommsModelRenewLeases),
            ("comms/model-latched-channels-are-not-leased", testCommsModelLatchedNotLeased),
            ("comms/model-shutdown-sends-release-all", testCommsModelShutdownSendsReleaseAll),
            ("comms/model-leaving-forgets-holds-locally-and-sends-nothing", testCommsModelLeavingForgetsHolds),
            ("comms/model-null-report-forgets-holds-locally", testCommsModelNullReportForgetsHolds),
            ("comms/model-someone-in-another-room-cannot-be-keyed", testCommsModelOtherRoom),
            ("comms/model-group-says-how-many-listen-and-who-is-elsewhere", testCommsModelGroupListening),
            ("comms/model-edit-talent-chips-assign-and-explain-web-clients", testCommsModelEditTalent),
            ("comms/model-people-rows-rebuilt-only-when-roster-changes", testCommsModelPeopleRowsGate),
            ("comms/model-settings-send-their-verbs", testCommsModelSettingsVerbs),
            ("comms/model-applying-state-never-sends-a-device-change", testCommsModelApplyNeverSends),
            ("comms/model-refusal-is-shown-verbatim", testCommsModelRefusalVerbatim),
            ("comms/model-meter-lights-only-from-a-measured-level", testCommsModelMeter),
            ("comms/model-ops-history-newest-first", testCommsModelOpsHistory),
            ("comms/model-private-off-by-default-nothing-sent", testCommsModelPrivateOffByDefault),
            ("comms/model-toggling-private-sends-and-asks-to-persist", testCommsModelTogglePrivate),
            ("comms/model-private-reasserted-once-per-2s", testCommsModelPrivateReasserted),
            ("comms/model-outside-a-meeting-toggle-only-records", testCommsModelPrivateOutsideMeeting),
            ("comms/model-meeting-label", testCommsModelMeetingLabel),
            ("comms/model-gain-clamps", testCommsModelGainClamps),
        ]
    }

    // ── harness ──────────────────────────────────────────────────────────

    /// Records every send as (verb, sorted-key JSON). `reply` is what the core answers.
    final class CommsRecorder {
        var sent: [(String, String)] = []
        var reply: String?
        var verbs: [String] { sent.map(\.0) }
        func clear() { sent.removeAll() }
        func payloads(_ verb: String) -> [String] { sent.filter { $0.0 == verb }.map(\.1) }
    }

    static func sortedJSON(_ payload: JSONObject) -> String {
        guard let data = try? JSONSerialization.data(withJSONObject: payload, options: [.sortedKeys]),
              let s = String(data: data, encoding: .utf8) else { return "?" }
        return s
    }

    static func ch(_ slot: Int, _ label: String? = nil, _ listeners: Int = 0, ready: Bool = true, keyed: Bool = false,
                   latched: Bool = false, room: String? = nil, reachOk: Int = -1, reachDark: Int = 0) -> String {
        let l = label.map { "\"\($0)\"" } ?? "null"
        let r = room.map { "\"\($0)\"" } ?? "null"
        return #"{"slot":\#(slot),"label":\#(l),"listeners":\#(listeners),"ready":\#(ready),"keyed":\#(keyed),"latched":\#(latched),"room":\#(r),"reachOk":\#(reachOk < 0 ? listeners : reachOk),"reachDark":\#(reachDark)}"#
    }

    static func desk(_ channels: String, roster: String = "[]", extra: String = "") -> String {
        #"{"available":true,"reason":null,"lamps":{"link":true,"meetingMic":true,"channel":true,"tx":false},"room":null,"channels":[\#(channels)],"roster":\#(roster),"mic":{"device":"Mic A","devices":["Mic A","Mic B"],"peakDbfs":-24,"gainDb":0,"sidetone":true,"sidetoneDevice":"Phones","sidetoneDevices":["Phones","Speakers"],"tone":false},"stats":{"framesSent":10,"channelSends":4,"underruns":0,"sendErrors":0},"log":["a","b"]\#(extra)}"#
    }

    static func commsState(_ json: String) -> CommsState {
        let object = (try? JSONSerialization.jsonObject(with: Data(json.utf8))) as? JSONObject ?? [:]
        return ZoomCommsProtocol.parseState(object)
    }

    static var twoPeople: String { desk(ch(0, "Ana", 1) + "," + ch(1, "Ben", 1) + "," + ch(2)) }

    @MainActor
    static func makeComms(_ comms: String? = nil, inMeeting: Bool = true, clock: (() -> Int64)? = nil)
        -> (CommsModel, CommsRecorder) {
        let rec = CommsRecorder()
        let model = CommsModel(send: { verb, payload in
            rec.sent.append((verb, sortedJSON(payload)))
            return rec.reply
        }, now: clock ?? { 0 })
        model.applyMeeting(inMeeting, label: "Meeting 123 456 7890")
        if let comms { model.apply(.some(commsState(comms))) }
        return (model, rec)
    }

    @MainActor
    static func run(_ body: @escaping @MainActor () async -> Void) {
        _ = awaitMain { await body(); return true }
    }

    static func expectSent(_ rec: CommsRecorder, _ expected: [(String, String)], _ what: String,
                           _ file: StaticString = #file, _ line: UInt = #line) {
        expectEqual(rec.sent.map { "\($0.0) \($0.1)" }, expected.map { "\($0.0) \($0.1)" }, what, file, line)
    }

    // ── ports ────────────────────────────────────────────────────────────

    @MainActor static func testCommsModelScreenStates() {
        let (m, _) = makeComms(inMeeting: false)
        expectEqual(m.pageState, .notInMeeting, "not in a meeting")
        expect(!m.deskVisible, "no desk")
        expectEqual(m.railText, "Not in a meeting", "rail")
        m.applyMeeting(true, label: "Meeting 123 456 7890")
        expectEqual(m.pageState, .waiting, "in the meeting, no report yet: waiting, not unavailable")
        m.apply(.some(commsState(#"{"available":false,"reason":"Make this account host or co-host to bring talkback up.","lamps":{"link":true}}"#)))
        expectEqual(m.pageState, .unavailable(text: "Make this account host or co-host to bring talkback up.", hostHint: true), "unavailable + host hint")
        expect(m.lampLink, "lamps are machine truth even when refused")
        expect(!m.lampChannel, "channel lamp off")
        m.apply(.some(commsState(twoPeople)))
        expectEqual(m.pageState, .desk, "desk")
        expect(m.gridVisible, "grid visible")
        expectEqual(m.railText, "Meeting 123 456 7890", "rail label")
        m.apply(.some(nil))
        expectEqual(m.pageState, .waiting, "dropped report: waiting")
        expect(m.cells.isEmpty, "no cells")
    }

    @MainActor static func testCommsModelOnlyOccupiedCells() {
        let (m, _) = makeComms(desk(ch(0, "Ana", 1) + "," + ch(1) + "," + ch(2, nil, 0, keyed: true) + "," + ch(3)))
        expectEqual(m.cells.map(\.slot), [0, 2], "slots [0,2]")
        expectEqual(m.cells[1].name, "CH 3", "unnamed cell")
    }

    @MainActor static func testCommsModelHoldingKeysWithALease() {
        let (m, rec) = makeComms(desk(ch(0, "Ana", 1)))
        run {
            await m.press(slot: 0); await m.press(slot: 0)
            await m.release(slot: 0); await m.release(slot: 0)
        }
        expectSent(rec, [("comms-key", #"{"leaseMs":1500,"on":true,"slot":0}"#), ("comms-key", #"{"on":false,"slot":0}"#)], "deduped")
    }

    @MainActor static func testCommsModelOnAirFromEngineOnly() {
        let (m, _) = makeComms(twoPeople)
        run { await m.press(slot: 0) }
        expectEqual(m.cells[0].tone, .ready, "pressed alone is not on air")
        expectEqual(m.cells[0].stateText, "ready", "ready text")
        m.apply(.some(commsState(desk(ch(0, "Ana", 1, keyed: true) + "," + ch(1, "Ben", 1)))))
        expectEqual(m.cells[0].tone, .hot, "hot")
        expectEqual(m.cells[0].stateText, "on air", "on air")
        m.apply(.some(commsState(desk(ch(0, "Ana", 0, keyed: true) + "," + ch(1, "Ben", 1)))))
        expectEqual(m.cells[0].tone, .armed, "keyed, nobody hearing: never hot")
        expectEqual(m.cells[0].stateText, "nobody in channel", "nobody text")
    }

    @MainActor static func testCommsModelLatchMode() {
        let (m, rec) = makeComms(twoPeople)
        m.latchMode.toggle()
        run { await m.press(slot: 1); await m.release(slot: 1) }
        m.apply(.some(commsState(desk(ch(0, "Ana", 1) + "," + ch(1, "Ben", 1, keyed: true, latched: true)))))
        expectEqual(m.cells[1].stateText, "on air · latched", "latched text")
        run { await m.press(slot: 1) }
        expectSent(rec, [("comms-latch", #"{"on":true,"slot":1}"#), ("comms-latch", #"{"on":false,"slot":1}"#)], "latch toggles")
    }

    @MainActor static func testCommsModelAllCall() {
        let (m, rec) = makeComms(twoPeople)
        run {
            await m.allPress(); await m.allPress(); await m.allRelease()
            m.latchMode = true
            await m.allPress()
        }
        expectSent(rec, [("comms-key", #"{"all":true,"leaseMs":1500,"on":true}"#),
                         ("comms-key", #"{"all":true,"on":false}"#),
                         ("comms-latch", #"{"all":true,"on":true}"#)], "all call")
        m.apply(.some(commsState(desk(ch(0, "Ana", 1, keyed: true) + "," + ch(1, "Ben", 0, keyed: true)))))
        expect(m.allHot, "all hot")
        expectEqual(m.allText, "ALL CALL · ON AIR", "all text")
    }

    @MainActor static func testCommsModelSameCellsNeverRebuild() {
        let (m, _) = makeComms(twoPeople)
        let version = m.cellStructureVersion
        run { await m.press(slot: 0) }
        m.apply(.some(commsState(desk(ch(0, "Ana", 1, keyed: true) + "," + ch(1, "Ben", 1) + "," + ch(2)))))
        m.apply(.some(commsState(desk(ch(0, "Ana", 0, keyed: true) + "," + ch(1, "Ben", 1) + "," + ch(2)))))
        expectEqual(m.cellStructureVersion, version, "no rebuild")
        expect(m.cells[0].held, "hold survives")
    }

    @MainActor static func testCommsModelStructureChangeKeepsHold() {
        let (m, _) = makeComms(twoPeople)
        run { await m.press(slot: 1) }
        m.apply(.some(commsState(desk(ch(0) + "," + ch(1, "Ben", 1) + "," + ch(2)))))
        expectEqual(m.cells.map(\.slot), [1], "Ben survives alone")
        expect(m.cells[0].held, "with his hold")
    }

    @MainActor static func testCommsModelReleaseAll() {
        let (m, rec) = makeComms(twoPeople)
        run { await m.press(slot: 0); await m.allPress() }
        rec.clear()
        run { await m.releaseAll() }
        expectSent(rec, [("comms-key", #"{"on":false,"slot":0}"#), ("comms-key", #"{"all":true,"on":false}"#)], "release all")
        expect(!m.anyHeld, "nothing held")
    }

    @MainActor static func testCommsModelRenewLeases() {
        let (m, rec) = makeComms(twoPeople)
        run { await m.press(slot: 1) }
        rec.clear()
        run { await m.renewLeases() }
        expectSent(rec, [("comms-key", #"{"leaseMs":1500,"on":true,"slot":1}"#)], "renewed")
        run { await m.release(slot: 1) }
        rec.clear()
        run { await m.renewLeases() }
        expect(rec.sent.isEmpty, "released: not renewed")
    }

    @MainActor static func testCommsModelLatchedNotLeased() {
        let (m, rec) = makeComms(twoPeople)
        m.latchMode = true
        run { await m.press(slot: 0) }
        rec.clear()
        run { await m.renewLeases() }
        expect(rec.sent.isEmpty, "latched channels are not leased")
        expect(!m.anyHeld, "a latch is not a hold")
    }

    @MainActor static func testCommsModelShutdownSendsReleaseAll() {
        let (m, rec) = makeComms(twoPeople)
        run { await m.press(slot: 0) }
        rec.clear()
        run { await m.sendReleaseAll() }
        expectSent(rec, [("comms-release-all", "{}")], "release-all")
        expect(!m.anyHeld, "nothing held")
    }

    @MainActor static func testCommsModelLeavingForgetsHolds() {
        let (m, rec) = makeComms(twoPeople)
        run { await m.press(slot: 0) }
        rec.clear()
        m.applyMeeting(false, label: nil)
        run { await m.drain() }
        expect(!m.anyHeld, "local holds cleared")
        expect(rec.sent.isEmpty, "Windows sends nothing here (CommsViewModel.cs:318)")
        expectEqual(m.pageState, .notInMeeting, "not in a meeting")
        run { await m.renewLeases() }
        expect(rec.sent.isEmpty, "nothing left to renew")
    }

    @MainActor static func testCommsModelNullReportForgetsHolds() {
        let (m, rec) = makeComms(twoPeople)
        run { await m.press(slot: 0) }
        rec.clear()
        m.apply(.some(nil))
        run { await m.drain() }
        expect(!m.anyHeld, "local holds cleared")
        expect(rec.sent.isEmpty, "nothing sent (CommsViewModel.cs:326)")
        expectEqual(m.pageState, .waiting, "waiting")
    }

    @MainActor static func testCommsModelOtherRoom() {
        let (m, rec) = makeComms(desk(ch(0, "Ana", 1, room: "Green room") + "," + ch(1, "Ben", 1)))
        expect(m.cells[0].off, "off")
        expectEqual(m.cells[0].tone, .off, "off tone")
        expectEqual(m.cells[0].stateText, "in Green room", "in room")
        run { await m.press(slot: 0) }
        expect(rec.sent.isEmpty, "cannot be keyed")
        expectEqual(m.cell(forDigit: 1)?.slot, 1, "digits skip unreachable cells")
        expectEqual(m.cells[1].keyHint, "1", "key hint")
    }

    @MainActor static func testCommsModelGroupListening() {
        let (m, _) = makeComms(desk(ch(4, nil, 3, reachOk: 3, reachDark: 2)))
        expectEqual(m.cells[0].stateText, "3 listening · 2 elsewhere", "group text")
        expectEqual(m.cells[0].name, "CH 5", "group name")
    }

    @MainActor static func testCommsModelEditTalent() {
        let roster = #"[{"participantId":201,"name":"Ana","talkback":true,"room":null,"channels":[0]},{"participantId":203,"name":"Wes","talkback":false,"room":null,"channels":[]}]"#
        let (m, rec) = makeComms(desk(ch(0, "Ana", 1) + "," + ch(1), roster: roster))
        m.editMode.toggle()
        expect(m.editVisible, "edit visible")
        expect(!m.gridVisible, "grid hidden")
        expectEqual(m.people.count, 2, "two rows")
        let ana = m.people[0]
        expectEqual(ana.chips.count, 2, "one chip per channel")
        expect(ana.chips[0].on, "on CH 1")
        expectEqual(ana.chips[0].tip, "Take off Ana", "take off tip")
        expect(!ana.chips[1].on, "off CH 2")
        expectEqual(ana.chips[1].tip, "Put on CH 2 (empty)", "put on tip")
        expect(!m.people[1].talkback, "Wes has no talkback")
        expect(m.people[1].chips.isEmpty, "no chips for Wes")
        expect(CommsModel.webClientWhy.contains("web client"), "web client copy")
        run { await m.assign(slot: 1, participantId: 201, on: !ana.chips[1].on) }
        expectSent(rec, [("comms-assign", #"{"on":true,"participantId":201,"slot":1}"#)], "assign payload")
    }

    @MainActor static func testCommsModelPeopleRowsGate() {
        let roster = #"[{"participantId":201,"name":"Ana","talkback":true,"room":null,"channels":[0]}]"#
        let (m, _) = makeComms(desk(ch(0, "Ana", 1), roster: roster))
        let version = m.peopleStructureVersion
        m.apply(.some(commsState(desk(ch(0, "Ana", 1, keyed: true), roster: roster))))
        expectEqual(m.peopleStructureVersion, version, "keyed is not a roster change")
    }

    @MainActor static func testCommsModelSettingsVerbs() {
        let (m, rec) = makeComms(twoPeople)
        run {
            await m.selectMic("Mic B"); await m.selectOutput("Speakers")
            await m.toggleSidetone(); await m.toggleTone(); await m.gainUp(); await m.gainDown()
        }
        expectSent(rec, [("comms-mic", #"{"device":"Mic B"}"#), ("comms-sidetone-out", #"{"device":"Speakers"}"#),
                         ("comms-sidetone", #"{"on":false}"#), ("comms-tone", #"{"on":true}"#),
                         ("comms-gain", #"{"db":1}"#), ("comms-gain", #"{"db":-1}"#)], "settings verbs")
        expectEqual(m.selectedMic, "Mic A", "still the engine's until it reports the switch")
    }

    @MainActor static func testCommsModelApplyNeverSends() {
        let (m, rec) = makeComms(twoPeople)
        let v = m.deviceListVersion
        m.apply(.some(commsState(twoPeople)))
        m.apply(.some(commsState(twoPeople.replacingOccurrences(of: #""peakDbfs":-24"#, with: #""peakDbfs":-12"#))))
        run { await m.drain() }
        expect(rec.sent.isEmpty, "applying state sends nothing")
        expectEqual(m.deviceListVersion, v, "pickers not rebuilt")
        expectEqual(m.mics, ["Mic A", "Mic B"], "mics")
    }

    @MainActor static func testCommsModelRefusalVerbatim() {
        let (m, rec) = makeComms(twoPeople)
        rec.reply = "Comms is not available: Not in a meeting."
        run { await m.press(slot: 0) }
        expectEqual(m.error, "Comms is not available: Not in a meeting.", "refusal verbatim")
        rec.reply = nil
        run { await m.release(slot: 0) }
        expectEqual(m.error, nil, "cleared on success")
    }

    @MainActor static func testCommsModelMeter() {
        let (m, _) = makeComms(twoPeople)
        expectEqual(m.litSegments, 6, "-24 dBFS lights half of 12")
        m.apply(.some(commsState(twoPeople.replacingOccurrences(of: #""peakDbfs":-24"#, with: #""peakDbfs":null"#))))
        expectEqual(m.litSegments, 0, "unmeasured is dark")
        expectEqual(m.micLevelName, "Microphone level not measured", "level name")
    }

    @MainActor static func testCommsModelOpsHistory() {
        let (m, _) = makeComms(twoPeople)
        expectEqual(m.opsLine, "b", "newest line")
        expectEqual(m.opsLines.map(\.text), ["b", "a"], "newest first")
    }

    @MainActor static func testCommsModelPrivateOffByDefault() {
        let (m, rec) = makeComms(twoPeople)
        run { await m.drain() }
        expect(!m.privatePreference, "off by default")
        expect(!m.privateReported, "not reported")
        expect(!rec.verbs.contains("comms-private"), "nothing sent")
    }

    @MainActor static func testCommsModelTogglePrivate() {
        let (m, rec) = makeComms(twoPeople)
        var saved: [Bool] = []
        m.onPrivatePreferenceChanged = { saved.append($0) }
        run { await m.togglePrivate(); await m.togglePrivate() }
        expectSent(rec, [("comms-private", #"{"on":true}"#), ("comms-private", #"{"on":false}"#)], "private verbs")
        expectEqual(saved, [true, false], "asked to persist")
    }

    @MainActor static func testCommsModelPrivateReasserted() {
        var t: Int64 = 0
        let (m, rec) = makeComms(inMeeting: false, clock: { t })
        m.applyMeeting(true, label: nil)
        m.restorePrivatePreference(true)
        run { await m.drain() }
        expect(rec.sent.isEmpty, "restoring sends nothing by itself")
        for step in 0..<10 {  // 0..2250 ms of disagreeing reports
            t = Int64(step * 250)
            m.apply(.some(commsState(desk(ch(0, "Ana", 1)))))
        }
        run { await m.drain() }
        expectEqual(rec.payloads("comms-private"), [#"{"on":true}"#, #"{"on":true}"#], "at t=0 and t=2000 only")
        let agreed = desk(ch(0, "Ana", 1), extra: #","private":true"#)
        m.apply(.some(commsState(agreed)))
        t += 5000
        m.apply(.some(commsState(agreed)))
        run { await m.drain() }
        expectEqual(rec.payloads("comms-private").count, 2, "quiet once they agree")
        expect(m.privateReported, "reported")
        expect(m.railText.hasSuffix("· private comms"), "rail says private")
    }

    @MainActor static func testCommsModelPrivateOutsideMeeting() {
        let (m, rec) = makeComms(inMeeting: false)
        run { await m.togglePrivate() }
        expect(m.privatePreference, "recorded")
        expect(rec.sent.isEmpty, "nothing sent outside a meeting")
    }

    static func testCommsModelMeetingLabel() {
        expectEqual(CommsModel.meetingLabel(for: "https://zoom.us/j/1234567890?pwd=abc"), "Meeting 123 456 7890", "10 digits")
        expectEqual(CommsModel.meetingLabel(for: "https://us02web.zoom.us/j/12345678901"), "Meeting 123 4567 8901", "11 digits")
        expectEqual(CommsModel.meetingLabel(for: "123 456 7890"), "Meeting 123 456 7890", "bare id")
        expectEqual(CommsModel.meetingLabel(for: ""), nil, "empty")
        expectEqual(CommsModel.meetingLabel(for: "https://zoom.us/my/someone"), nil, "personal link")
    }

    @MainActor static func testCommsModelGainClamps() {
        let (m, rec) = makeComms(twoPeople.replacingOccurrences(of: #""gainDb":0"#, with: #""gainDb":20"#))
        run { await m.gainUp() }
        expect(rec.sent.isEmpty, "at 20, gainUp sends nothing")
        run { await m.gainDown() }
        expectSent(rec, [("comms-gain", #"{"db":19}"#)], "down from 20")
    }
}
