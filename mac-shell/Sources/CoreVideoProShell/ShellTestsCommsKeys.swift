// Comms release triggers and panel keys, plan 2026-11-22 Task 10.

import AppKit

extension ShellTests {
    static var commsKeysCases: [(String, () -> Void)] {
        [
            ("comms/keys-digit-press-release", testCommsKeysDigitPressRelease),
            ("comms/keys-ignored-with-modifier-text-field-or-other-tab", testCommsKeysIgnored),
            ("comms/keys-up-without-recorded-down-does-nothing", testCommsKeysUpWithoutDown),
            ("comms/keys-space-is-all-call-and-keypad-digits-count", testCommsKeysSpaceAndKeypad),
            ("comms/space-on-comms-is-never-take", testCommsSpaceIsNeverTake),
            ("comms/release-on-tab-change", testCommsReleaseOnTabChange),
            ("comms/release-on-app-resign-active", testCommsReleaseOnResignActive),
            ("comms/release-on-main-window-resign-key", testCommsReleaseOnMainWindowResignKey),
            ("comms/quit-sends-release-all-before-core-stop", testCommsQuitReleaseFirst),
            ("comms/confirm-quit-sends-release-all-before-output-stops", testCommsConfirmQuitReleaseFirst),
            ("comms/quit-release-is-bounded-when-the-core-hangs", testCommsQuitReleaseBounded),
            ("comms/keys-drive-the-model-end-to-end", testCommsKeysDriveTheModel),
            ("comms/trigger-releases-a-cell-that-left-while-held", testCommsTriggerReleasesOrphan),
        ]
    }

    static func cev(_ kind: CommsKeyEvent.Kind, down: Bool, modifiers: TransportKeyEvent.Modifiers = [],
                    isRepeat: Bool = false, isCommsTab: Bool = true, isTextEntryFocus: Bool = false) -> CommsKeyEvent {
        CommsKeyEvent(kind: kind, down: down, modifiers: modifiers, isRepeat: isRepeat, isCommsTab: isCommsTab,
                      deskVisible: true, isTextEntryFocus: isTextEntryFocus)
    }

    static func testCommsKeysDigitPressRelease() {
        var k = CommsKeys()
        expectEqual(k.decide(cev(.digit(1), down: true), cellForDigit: { _ in 0 }), .press(slot: 0), "press")
        expectEqual(k.decide(cev(.digit(1), down: true, isRepeat: true), cellForDigit: { _ in 0 }), .swallow,
                    "no re-toggle on repeat")
        expectEqual(k.decide(cev(.digit(1), down: false), cellForDigit: { _ in 0 }), .release(slot: 0), "release")
        expectEqual(k.decide(cev(.digit(2), down: true), cellForDigit: { _ in nil }), .swallow, "no such cell: consumed")
        expectEqual(k.decide(cev(.digit(2), down: false), cellForDigit: { _ in nil }), .swallow, "and its up too")
    }

    static func testCommsKeysIgnored() {
        var k = CommsKeys()
        expectEqual(k.decide(cev(.space, down: true, modifiers: .command), cellForDigit: { _ in 0 }), .ignore, "modifier")
        expectEqual(k.decide(cev(.space, down: true, isTextEntryFocus: true), cellForDigit: { _ in 0 }), .ignore, "text field")
        expectEqual(k.decide(cev(.space, down: true, isCommsTab: false), cellForDigit: { _ in 0 }), .ignore, "other tab")
        var hidden = cev(.digit(1), down: true); hidden.deskVisible = false
        expectEqual(k.decide(hidden, cellForDigit: { _ in 0 }), .ignore, "desk hidden")
        expectEqual(k.decide(cev(.other, down: true), cellForDigit: { _ in 0 }), .ignore, "other keys")
    }

    static func testCommsKeysUpWithoutDown() {
        var k = CommsKeys()
        expectEqual(k.decide(cev(.space, down: false), cellForDigit: { _ in 0 }), .ignore, "space up")
        expectEqual(k.decide(cev(.digit(3), down: false), cellForDigit: { _ in 0 }), .ignore, "digit up")
    }

    static func testCommsKeysSpaceAndKeypad() {
        var k = CommsKeys()
        expectEqual(k.decide(cev(.space, down: true), cellForDigit: { _ in 0 }), .allPress, "space = ALL CALL")
        expectEqual(k.decide(cev(.space, down: true, isRepeat: true), cellForDigit: { _ in 0 }), .swallow, "repeat")
        expectEqual(k.decide(cev(.space, down: false), cellForDigit: { _ in 0 }), .allRelease, "release")
        expectEqual(CommsKeys.kind(keyCode: 18), .digit(1), "main-row 1")
        expectEqual(CommsKeys.kind(keyCode: 25), .digit(9), "main-row 9")
        expectEqual(CommsKeys.kind(keyCode: 83), .digit(1), "keypad 1")
        expectEqual(CommsKeys.kind(keyCode: 92), .digit(9), "keypad 9")
        expectEqual(CommsKeys.kind(keyCode: 29), .other, "0 is not a talk key")
        expectEqual(CommsKeys.kind(keyCode: 49), .space, "space")
        // The slot a digit pressed is the slot it releases, even if the cells moved.
        var m = CommsKeys()
        _ = m.decide(cev(.digit(1), down: true), cellForDigit: { _ in 4 })
        expectEqual(m.decide(cev(.digit(1), down: false), cellForDigit: { _ in 7 }), .release(slot: 4), "pressed slot")
    }

    @MainActor static func testCommsSpaceIsNeverTake() {
        let m = AppModel()
        m.selectedTab = .comms
        m.syncTransportHotkeys(); m.syncCommsKeys()
        expect(!m.transportHotkeys.isInstalled, "Take monitor off on Comms")
        expect(m.commsKeyMonitor.isInstalled, "Comms keys on")
        m.selectedTab = .studio
        m.syncTransportHotkeys(); m.syncCommsKeys()
        expect(!m.commsKeyMonitor.isInstalled, "Comms keys off elsewhere")
        m.transportHotkeys.remove()
    }

    @MainActor static func heldOnComms() -> (AppModel, CommsCoreRecorder) {
        let (m, core) = commsModelWithCore()
        m.applyZoomForTesting(["meetingState": "in_meeting"])
        m.comms.apply(.some(commsState(desk(ch(0, "Ana", 1)))))
        m.selectedTab = .comms
        run { await m.comms.press(slot: 0) }
        core.clear()
        return (m, core)
    }

    @MainActor static func settle(_ m: AppModel) {
        spin(until: { !m.comms.anyHeld }, limit: 1)
        run { await m.comms.drain() }
    }

    @MainActor static func testCommsReleaseOnTabChange() {
        let (m, core) = heldOnComms()
        m.selectedTab = .audio
        settle(m)
        expectEqual(core.sent.map { "\($0.verb) \($0.payload)" }, [#"comms-key {"on":false,"slot":0}"#], "released, nothing else")
        core.clear()
        m.selectedTab = .media
        settle(m)
        expect(core.sent.isEmpty, "nothing held: nothing sent")
    }

    @MainActor static func testCommsReleaseOnResignActive() {
        let (m, core) = heldOnComms()
        m.installCommsReleaseObservers()
        NotificationCenter.default.post(name: NSApplication.didResignActiveNotification, object: NSApp)
        settle(m)
        expectEqual(core.sent.map { "\($0.verb) \($0.payload)" }, [#"comms-key {"on":false,"slot":0}"#], "released")
    }

    @MainActor static func testCommsReleaseOnMainWindowResignKey() {
        let (m, core) = heldOnComms()
        m.installCommsReleaseObservers()
        let other = NSWindow(contentRect: .zero, styleMask: [.borderless], backing: .buffered, defer: true)
        NotificationCenter.default.post(name: NSWindow.didResignKeyNotification, object: other)
        settle(m)
        expect(core.sent.isEmpty && m.comms.anyHeld, "another window resigning key does nothing")
        let main = NSWindow(contentRect: .zero, styleMask: [.borderless], backing: .buffered, defer: true)
        MainWindowTracker.track(main)
        NotificationCenter.default.post(name: NSWindow.didResignKeyNotification, object: main)
        settle(m)
        expectEqual(core.sent.map { "\($0.verb) \($0.payload)" }, [#"comms-key {"on":false,"slot":0}"#], "main window: released")
    }

    @MainActor static func testCommsQuitReleaseFirst() {
        let (m, core) = heldOnComms()
        var order: [String] = []
        m.quitReplyForTesting = { order.append("reply:\($0)") }
        m.quitCoreGoneForTesting = false
        let wrapped = m.quitRequestForTesting
        m.quitRequestForTesting = { body, t in order.append(body["type"] as? String ?? "?"); return try await wrapped!(body, t) }
        expect(m.handleQuitRequest() == .terminateLater, "in a meeting, quit waits for the release")
        spin(until: { order.contains { $0.hasPrefix("reply") } }, limit: 2)
        expectEqual(order, ["comms-release-all", "reply:true"], "release-all, then the app may terminate")
        expectEqual(core.sent.map(\.payload), ["{}"], "one release-all line")
        // Out of a meeting, an idle quit is still immediate.
        let idle = AppModel()
        idle.quitReplyForTesting = { _ in }
        idle.quitRequestForTesting = { _, _ in [:] }
        expect(idle.handleQuitRequest() == .terminateNow, "idle quit unchanged")
    }

    @MainActor static func testCommsConfirmQuitReleaseFirst() {
        let (m, _) = heldOnComms()
        var order: [String] = []
        m.quitReplyForTesting = { order.append("reply:\($0)") }
        m.quitCoreGoneForTesting = false
        m.quitFinishTimeout = 0.3
        m.quitPollInterval = 0.01
        m.quitCoreGrace = 0.05
        let wrapped = m.quitRequestForTesting
        m.quitRequestForTesting = { body, t in
            order.append((body["type"] as? String) ?? "?")
            return try await wrapped!(body, t)
        }
        m.recordingStatus = "recording"
        m.recordingDesired = true
        _ = m.handleQuitRequest()
        m.confirmQuit()
        spin(until: { order.contains { $0.hasPrefix("reply") } }, limit: 5)
        expectEqual(order.first, "comms-release-all", "release-all is the first thing the quit sends")
        expect(order.contains { $0.hasPrefix("reply") }, "the quit completes")
    }

    @MainActor static func testCommsQuitReleaseBounded() {
        let (m, _) = heldOnComms()
        var replies: [Bool] = []
        m.quitReplyForTesting = { replies.append($0) }
        m.quitCoreGoneForTesting = false
        m.quitRequestForTesting = { _, _ in try? await Task.sleep(nanoseconds: 30_000_000_000); return [:] }
        let t0 = Date()
        _ = m.handleQuitRequest()
        spin(until: { !replies.isEmpty }, limit: 3)
        expectEqual(replies, [true], "a hung core cannot hold the quit")
        expect(Date().timeIntervalSince(t0) < 1.5, "bounded near 350 ms")
    }

    /// handleCommsKey -> CommsModel -> the core, with the exact payloads.
    @MainActor static func testCommsKeysDriveTheModel() {
        let (m, core) = commsModelWithCore()
        m.applyZoomForTesting(["meetingState": "in_meeting"])
        m.comms.apply(.some(commsState(desk(ch(0, "Ana", 1) + "," + ch(1, "Ben", 1)))))
        m.selectedTab = .comms
        func key(_ kind: CommsKeyEvent.Kind, down: Bool, isRepeat: Bool = false) -> Bool {
            m.handleCommsKey(CommsKeyEvent(kind: kind, down: down, isRepeat: isRepeat, isCommsTab: m.selectedTab == .comms,
                                           deskVisible: m.comms.deskVisible))
        }
        expect(key(.digit(2), down: true), "digit consumed")
        expect(key(.digit(2), down: true, isRepeat: true), "repeat consumed (never reaches a focused button)")
        expect(key(.space, down: true), "space consumed")
        expect(key(.space, down: false), "space up consumed")
        expect(key(.digit(2), down: false), "digit up consumed")
        expect(!key(.other, down: true), "other keys pass through")
        run { await m.comms.drain() }
        spin(until: { core.sent.count >= 4 }, limit: 1)
        run { await m.comms.drain() }
        expectEqual(core.sent.map { "\($0.verb) \($0.payload)" }, [
            #"comms-key {"leaseMs":1500,"on":true,"slot":1}"#,
            #"comms-key {"all":true,"leaseMs":1500,"on":true}"#,
            #"comms-key {"all":true,"on":false}"#,
            #"comms-key {"on":false,"slot":1}"#,
        ], "digit 2 keys Ben; Space is ALL CALL; each up releases")
        core.clear()
        // A key down, then the desk hides (report dropped), then the key comes up.
        _ = key(.digit(1), down: true)
        spin(until: { !core.sent.isEmpty }, limit: 1)
        m.comms.apply(.some(nil))
        expect(!key(.digit(3), down: true), "desk hidden: new presses pass through")
        expect(key(.digit(1), down: false), "the recorded down still gets its up")
        run { await m.comms.drain() }
        expect(!m.comms.anyHeld, "nothing left held")
    }

    /// A held cell leaves the desk, then a release trigger fires before key-up:
    /// the release still goes out (not left to the lease).
    @MainActor static func testCommsTriggerReleasesOrphan() {
        let (m, core) = heldOnComms()
        m.comms.apply(.some(commsState(desk(ch(0) + "," + ch(1, "Ben", 1)))))   // Ana's cell leaves while held
        expect(m.comms.anyHeld, "the orphaned hold still counts")
        m.selectedTab = .audio
        settle(m)
        expectEqual(core.sent.map { "\($0.verb) \($0.payload)" }, [#"comms-key {"on":false,"slot":0}"#], "released")
    }
}
