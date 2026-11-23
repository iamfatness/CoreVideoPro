// Comms wiring, plan 2026-11-22 Task 8: the gate, the tab, the Private pref,
// the 500 ms lease tick, the meeting state, and the core-restart rule.

import Foundation

extension ShellTests {
    static var commsWiringCases: [(String, () -> Void)] {
        [
            ("comms/nav-hidden-until-gate", testCommsNavHiddenUntilGate),
            ("comms/tab-name-resolves", testCommsTabNameResolves),
            ("comms/prefs-absent-means-off", testCommsPrefsAbsentMeansOff),
            ("comms/private-pref-restored-and-saved", testCommsPrivatePrefRestoredAndSaved),
            ("comms/lease-tick-cadence", testCommsLeaseTickCadence),
            ("comms/meeting-state-feeds-the-model", testCommsMeetingStateFeedsTheModel),
            ("comms/core-restart-reasserts-private-and-drops-holds", testCommsCoreRestart),
            ("comms/refusal-reaches-the-model-verbatim", testCommsRefusalThroughTheCore),
            ("comms/minimal-page-states", testCommsMinimalPageStates),
        ]
    }

    /// An AppModel whose core requests are recorded (type + sorted payload).
    final class CommsCoreRecorder {
        var sent: [(verb: String, payload: String)] = []
        var refuse: String?
        var verbs: [String] { sent.map(\.verb) }
        func clear() { sent.removeAll() }
    }

    @MainActor
    static func commsModelWithCore() -> (AppModel, CommsCoreRecorder) {
        let m = AppModel()
        let core = CommsCoreRecorder()
        m.quitRequestForTesting = { body, _ in
            let verb = body["type"] as? String ?? ""
            if verb.hasPrefix("comms-") {
                core.sent.append((verb, sortedJSON(body["payload"] as? JSONObject ?? [:])))
                if let refuse = core.refuse { throw BridgeError.remote(refuse) }
            }
            return ["ok": true]
        }
        return (m, core)
    }

    static func testCommsNavHiddenUntilGate() {
        expect(!StudioTab.navigation(commsEnabled: false).contains(.comms), "gated off")
        expectEqual(StudioTab.navigation(commsEnabled: true).map(\.navLabel),
                    ["Studio", "Zoom", "Sources", "Scenes", "Routing", "Graphics", "Audio", "Comms", "Media", "Health", "Settings"],
                    "Windows order")
    }

    static func testCommsTabNameResolves() {
        expectEqual(StudioTab(name: "comms"), .comms, "by identifier")
        expectEqual(StudioTab(name: "Comms"), .comms, "by label")
    }

    static func testCommsPrefsAbsentMeansOff() {
        guard let p = try? JSONDecoder().decode(ShellPrefs.self, from: Data("{}".utf8)) else {
            expect(false, "empty prefs decode"); return
        }
        expect(p.commsPrivate != true, "absent = off")
        var q = p
        q.commsPrivate = true
        let back = (try? JSONEncoder().encode(q)).flatMap { try? JSONDecoder().decode(ShellPrefs.self, from: $0) }
        expectEqual(back?.commsPrivate, true, "round trip")
    }

    @MainActor static func testCommsPrivatePrefRestoredAndSaved() {
        let (m, _) = commsModelWithCore()
        var prefs = ShellPrefs()
        prefs.commsPrivate = true
        m.applyRestoredPrefs(prefs)
        expect(m.comms.privatePreference, "restored")
        run { await m.comms.togglePrivate() }
        expect(m.currentPrefs().commsPrivate != true, "saved off")
        run { await m.comms.togglePrivate() }
        expectEqual(m.currentPrefs().commsPrivate, true, "saved on")
    }

    @MainActor static func testCommsLeaseTickCadence() {
        let (m, core) = commsModelWithCore()
        m.applyZoomForTesting(["meetingState": "in_meeting"])
        m.comms.apply(.some(commsState(desk(ch(0, "Ana", 1)))))
        run { await m.comms.press(slot: 0) }
        core.clear()
        run {
            for ms in stride(from: 0, through: 1500, by: 500) { await m.commsLeaseTick(nowMs: Int64(ms)) }
        }
        expectEqual(core.verbs.filter { $0 == "comms-key" }.count, 4, "renew at 0,500,1000,1500")
        run { await m.comms.release(slot: 0) }
        core.clear()
        run { await m.commsLeaseTick(nowMs: 2000) }
        expect(core.sent.isEmpty, "nothing held, nothing sent")
    }

    @MainActor static func testCommsMeetingStateFeedsTheModel() {
        let (m, _) = commsModelWithCore()
        m.joinMeetingId = "https://zoom.us/j/1234567890?pwd=abc"
        m.applyZoomForTesting(["meetingState": "in_meeting"])
        expectEqual(m.comms.pageState, .waiting, "in meeting, no report")
        expectEqual(m.comms.railText, "Meeting 123 456 7890", "label from the join text")
        m.applyZoomForTesting(["meetingState": "idle"])
        expectEqual(m.comms.pageState, .notInMeeting, "left")
    }

    @MainActor static func testCommsCoreRestart() {
        let (m, core) = commsModelWithCore()
        var prefs = ShellPrefs()
        prefs.commsPrivate = true
        m.applyRestoredPrefs(prefs)
        m.applyZoomForTesting(["meetingState": "in_meeting"])
        let agreeing = desk(ch(0, "Ana", 1), extra: #","private":true"#)
        m.comms.apply(.some(commsState(agreeing)))
        run { await m.comms.press(slot: 0) }
        core.clear()
        m.handleBridgeStatus(.exited(code: 9))
        m.handleBridgeStatus(.connected(renderer: "metal"))
        m.stopTimersForTesting()
        expect(!m.comms.anyHeld, "holds dropped on the core generation change")
        expectEqual(m.comms.pageState, .waiting, "the old report is gone")
        m.comms.apply(.some(nil))
        m.comms.apply(.some(commsState(desk(ch(0, "Ana", 1)))))  // the new engine reports private:false
        run { await m.comms.drain() }
        expectEqual(core.sent.filter { $0.verb == "comms-private" }.map(\.payload), [#"{"on":true}"#], "re-asserted once")
    }

    @MainActor static func testCommsRefusalThroughTheCore() {
        let (m, core) = commsModelWithCore()
        m.applyZoomForTesting(["meetingState": "in_meeting"])
        m.comms.apply(.some(commsState(desk(ch(0, "Ana", 1)))))
        core.refuse = "Comms is not available: Not in a meeting."
        run { await m.comms.press(slot: 0) }
        expectEqual(m.comms.error, "Comms is not available: Not in a meeting.", "the core's message verbatim")
        expectEqual(core.sent.first?.payload, #"{"leaseMs":1500,"on":true,"slot":0}"#, "payload rides the payload wrapper")
    }

    @MainActor static func testCommsMinimalPageStates() {
        expectEqual(CommsStateBlock.notInMeetingTitle, "Not in a meeting", "title")
        expectEqual(CommsStateBlock.notInMeetingBody,
                    "Join from the Zoom page. Talkback comes up once this account is in the meeting as host or co-host.", "body")
        expectEqual(CommsStateBlock.waitingText, "Waiting for the Zoom engine's talkback report…", "waiting")
        expectEqual(CommsStateBlock.unavailableTitle, "Talkback is not up", "unavailable")
        expectEqual(CommsStateBlock.hostHint,
                    "Ask the meeting host to make this account host or co-host. Video keeps working meanwhile.", "host hint")
    }
}
