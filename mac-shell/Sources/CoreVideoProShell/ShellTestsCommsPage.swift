// The Comms page, plan 2026-11-22 Task 9: TalkKey press/release, the page
// owning its scrolling (strip and ops pinned), the grid width, and the
// tone-to-token mapping. Layout is measured on a real offscreen NSHostingView.

import AppKit
import SwiftUI

extension ShellTests {
    static var commsPageCases: [(String, () -> Void)] {
        [
            ("comms/talkkey-press-release-once", testTalkKeyPressReleaseOnce),
            ("comms/talkkey-releases-when-removed-from-window", testTalkKeyReleasesWhenRemoved),
            ("comms/talkkey-releases-on-window-resign-key", testTalkKeyReleasesOnResignKey),
            ("comms/talkkey-disabled-ignores", testTalkKeyDisabledIgnores),
            ("comms/talkkey-right-click-ignored", testTalkKeyRightClickIgnored),
            ("comms/page-owns-its-scrolling", testCommsPageOwnsItsScrolling),
            ("comms/grid-fits-six-200pt-columns", testCommsGridFitsSixColumns),
            ("comms/tone-colours-use-studio-tokens", testCommsToneColours),
        ]
    }

    // ── TalkKey ──────────────────────────────────────────────────────────

    final class KeyCounts { var presses = 0; var releases = 0 }

    @MainActor static func makeTalkKey(enabled: Bool = true) -> (TalkKeyView, KeyCounts, NSWindow) {
        let counts = KeyCounts()
        let view = TalkKeyView(frame: NSRect(x: 0, y: 0, width: 100, height: 40))
        view.enabled = enabled
        view.onPress = { counts.presses += 1 }
        view.onRelease = { counts.releases += 1 }
        let window = NSWindow(contentRect: NSRect(x: -10_000, y: -10_000, width: 100, height: 40),
                              styleMask: [.borderless], backing: .buffered, defer: false)
        window.contentView = NSView(frame: NSRect(x: 0, y: 0, width: 100, height: 40))
        window.contentView!.addSubview(view)
        return (view, counts, window)
    }

    @MainActor static func mouse(_ type: NSEvent.EventType, in window: NSWindow) -> NSEvent {
        NSEvent.mouseEvent(with: type, location: NSPoint(x: 10, y: 10), modifierFlags: [], timestamp: 0,
                           windowNumber: window.windowNumber, context: nil, eventNumber: 0, clickCount: 1,
                           pressure: 1)!
    }

    @MainActor static func testTalkKeyPressReleaseOnce() {
        let (v, c, w) = makeTalkKey()
        v.mouseDown(with: mouse(.leftMouseDown, in: w)); v.mouseDown(with: mouse(.leftMouseDown, in: w))
        expectEqual(c.presses, 1, "press acts on mouse-down, once")
        expectEqual(c.releases, 0, "nothing released yet")
        v.mouseUp(with: mouse(.leftMouseUp, in: w)); v.mouseUp(with: mouse(.leftMouseUp, in: w))
        expectEqual(c.releases, 1, "release once")
    }

    @MainActor static func testTalkKeyReleasesWhenRemoved() {
        let (v, c, w) = makeTalkKey()
        v.mouseDown(with: mouse(.leftMouseDown, in: w))
        v.removeFromSuperview()
        expectEqual(c.releases, 1, "leaving the window releases")
    }

    @MainActor static func testTalkKeyReleasesOnResignKey() {
        let (v, c, w) = makeTalkKey()
        v.mouseDown(with: mouse(.leftMouseDown, in: w))
        NotificationCenter.default.post(name: NSWindow.didResignKeyNotification, object: w)
        expectEqual(c.releases, 1, "the window resigning key releases")
        NotificationCenter.default.post(name: NSWindow.didResignKeyNotification, object: NSWindow())
        expectEqual(c.releases, 1, "another window's resign does nothing")
    }

    @MainActor static func testTalkKeyDisabledIgnores() {
        let (v, c, w) = makeTalkKey(enabled: false)
        v.mouseDown(with: mouse(.leftMouseDown, in: w))
        v.mouseUp(with: mouse(.leftMouseUp, in: w))
        expectEqual(c.presses, 0, "disabled: no press")
        expectEqual(c.releases, 0, "disabled: no release")
        expect(!v.acceptsFirstResponder, "never takes keyboard focus")
    }

    @MainActor static func testTalkKeyRightClickIgnored() {
        let (v, c, w) = makeTalkKey()
        v.rightMouseDown(with: mouse(.rightMouseDown, in: w))
        expectEqual(c.presses, 0, "left button only")
    }

    // ── layout ───────────────────────────────────────────────────────────

    final class CommsFrames { var ops: CGRect = .zero; var desk: CGRect = .zero; var cells: [Int: CGRect] = [:] }

    /// Renders TabPage itself (its scroll decision included) at width x height.
    @MainActor static func layoutComms(_ desk: String, tab: StudioTab = .comms, width: CGFloat, height: CGFloat)
        -> (CommsFrames, AppModel) {
        let model = AppModel()
        model.comms.applyMeeting(true, label: "Meeting 123 456 7890")
        model.comms.apply(.some(commsState(desk)))
        model.selectedTab = tab
        let frames = CommsFrames()
        let root = TabPage().environmentObject(model)
            .onPreferenceChange(CommsOpsFrameKey.self) { frames.ops = $0 }
            .onPreferenceChange(CommsDeskFrameKey.self) { frames.desk = $0 }
            .onPreferenceChange(CommsCellFramesKey.self) { frames.cells = $0 }
            .frame(width: width, height: height)
        let host = NSHostingView(rootView: root)
        let window = NSWindow(contentRect: NSRect(x: -10_000, y: -10_000, width: width, height: height),
                              styleMask: [.borderless], backing: .buffered, defer: false)
        window.contentView = host
        host.frame = NSRect(x: 0, y: 0, width: width, height: height)
        host.layoutSubtreeIfNeeded()
        host.display()
        spin(until: { !frames.cells.isEmpty && frames.ops != .zero }, limit: 2)
        return (frames, model)
    }

    static func manyCells(_ n: Int) -> String {
        desk((0..<n).map { ch($0, "Guest \($0 + 1)", 1) }.joined(separator: ","))
    }

    @MainActor static func testCommsPageOwnsItsScrolling() {
        expect(TabPageLayout.fillsHeight(.comms), "Comms owns its scrolling")
        // .audio fills the window too (OD-3, the Audio SHOW fixed viewport), so the example is .settings.
        expect(!TabPageLayout.fillsHeight(.settings), "other tabs keep the outer ScrollView")
        let height: CGFloat = 768
        let (f, _) = layoutComms(manyCells(30), width: 1360, height: height)
        expect(f.ops != .zero, "ops strip measured")
        // The comms coordinate space is the page inside TabPage's 12 pt gutter.
        let pageBottom = height - 2 * TabPageLayout.gutter
        expect(abs(f.ops.maxY - pageBottom) <= 12,
               "ops strip pinned at the bottom with 30 cells (maxY \(f.ops.maxY), page \(pageBottom))")
        expect(f.desk.maxY <= f.ops.minY + 0.5, "the desk ends above the strips")
        expect(f.desk.height < 30 * 72, "the desk scrolls rather than growing to fit 30 cells")
    }

    @MainActor static func testCommsGridFitsSixColumns() {
        let (f, _) = layoutComms(manyCells(12), width: 1360, height: 768)
        let firstRow = f.cells.values.filter { abs($0.minY - (f.cells[0]?.minY ?? -1)) < 1 }
        expectEqual(firstRow.count, 6, "six columns at a 1360 pt window (Windows CommsPageProbe)")
        expect(firstRow.allSatisfy { $0.width >= 190 }, "each column at least 190 pt")
    }

    static func testCommsToneColours() {
        typealias T = CommsColors.Triple
        expectEqual(CommsColors.cell(.hot), T(background: Studio.red, border: Studio.red, text: Studio.background), "hot")
        expectEqual(CommsColors.cell(.armed), T(background: Studio.surface, border: Studio.amber, text: Studio.amber), "armed")
        expectEqual(CommsColors.cell(.ready), T(background: Studio.surface, border: Studio.border, text: Studio.textPrimary), "ready")
        expectEqual(CommsColors.cell(.idle), T(background: Studio.surface, border: Studio.border, text: Studio.secondary), "idle")
        expectEqual(CommsColors.cell(.off), T(background: Studio.field, border: Studio.line2, text: Studio.textDim), "off")
        expectEqual(CommsColors.meter(.hot), Studio.red, "meter hot")
        expectEqual(CommsColors.meter(.armed), Studio.amber, "meter armed")
        expectEqual(CommsColors.meter(.ready), Studio.accent, "meter otherwise")
        expectEqual(CommsColors.lamp(true), Studio.accent, "lamp on (StudioLiveBrush)")
        expectEqual(CommsColors.lamp(false), Studio.textDim, "lamp off")
        expect(CommsColors.cell(.armed).background != Studio.red, "armed is never the on-air red")
    }
}
