// TabPageLayout — how wide each non-Studio tab's page may grow (Tier 0.10).
//
// TabPage used to cap EVERY tab at 720 pt (sources audit row 2). The Sources
// slot rows are built from fixed-width controls totalling ~1000 pt, so at any
// window size they were compressed or clipped, and the Routing grid (~724 pt)
// lost its last column. Windows stretches Sources and Routing to the window
// (SourcesInputsPage / RoutingPage). Forms keep a readable measure.

import AppKit
import SwiftUI

enum TabPageLayout {
    /// Page padding on each side (TabPage's .padding(12)).
    static let gutter: CGFloat = 12

    static func maxWidth(for tab: StudioTab) -> CGFloat {
        switch tab {
        // Tables and grids: use the window, like Windows.
        case .sources, .routing, .diagnose: return .infinity
        // The mixer asks for 1000 itself (AudioConsole.swift).
        case .audio: return 1000
        // Forms and cards: a readable measure.
        case .studio, .zoom, .scenes, .overlays, .media, .automation, .settings: return 720
        }
    }

    /// The width a tab's page actually gets in a window this wide.
    static func pageWidth(for tab: StudioTab, window: CGFloat) -> CGFloat {
        min(maxWidth(for: tab), window - 2 * gutter)
    }
}

/// The tab page without its ScrollView, so the offscreen snapshot harness and
/// the window draw exactly the same layout.
struct TabPageContent: View {
    @EnvironmentObject var model: AppModel
    let tab: StudioTab

    var body: some View {
        HStack {
            Spacer(minLength: 0)
            Group {
                switch tab {
                case .studio: EmptyView()
                case .zoom: ZoomPane()
                case .sources: SourcesPane()
                case .scenes: ScenesPane()
                case .routing: RoutingPane()
                case .overlays: OverlaysPane()
                case .audio: AudioPane()
                case .media: MediaPane()
                case .automation: AutomationPane()
                case .settings: SettingsPane()
                case .diagnose: DiagnosePane()
                }
            }
            .frame(maxWidth: TabPageLayout.maxWidth(for: tab))
            Spacer(minLength: 0)
        }
        .padding(TabPageLayout.gutter)
    }
}

/// COREVIDEO_SHELL_TAB_SNAPSHOT=<dir>: renders tab pages OFFSCREEN with
/// ImageRenderer (no window, no core) at the minimum window width and a wide
/// one, with a populated Sources table, then exits. COREVIDEO_SHELL_TAB_LIST
/// (comma-separated rawValues) narrows the set; default is every tab.
@MainActor
enum TabSnapshotHarness {
    static func seed(_ model: AppModel) {
        ShowInputWriteScope.run("snapshot-seed") { seedSlots(model) }
        // A revisioned roster through the real merge, then one delivered format
        // fact: row 1 reads LIVE 1920×1080 30, row 2 CONNECTING (no frame yet).
        model.applyZoomForTesting([
            "meetingState": "in_meeting", "rosterEpoch": "1:1:seed", "rosterRevision": NSNumber(value: 1),
            "participants": [
                ["userId": "101", "displayName": "Jordan Wallace", "videoOn": true, "talking": false,
                 "sourceGeneration": NSNumber(value: 1)],
                ["userId": "102", "displayName": "Guest with a long display name", "videoOn": true,
                 "sourceGeneration": NSNumber(value: 1)],
            ] as [JSONObject]])
        model.handleEventForTesting(["type": "zoom-source-format", "format": [
            "participantId": "101", "rosterEpoch": "1:1:seed", "sourceGeneration": NSNumber(value: 1),
            "width": 1920, "height": 1080, "fps": 30, "frameId": 1, "frameAtMs": 100.0] as JSONObject])
    }

    private static func seedSlots(_ model: AppModel) {
        model.assignSlot(1, kind: "zoom", sourceId: "101", name: "Jordan Wallace")
        model.assignSlot(2, kind: "zoom", sourceId: "102", name: "Guest with a long display name")
        model.assignSlot(3, kind: "capture", sourceId: "cam-1", name: "Wide camera (Blackmagic)")
        model.slots[2].audioDeviceId = "mic"; model.slots[2].audioDeviceName = "USB Audio Interface"
        model.toggleSlotIso(1)
    }

    /// NSHostingView in an offscreen, never-ordered-front window so AppKit-
    /// backed controls (menus, checkboxes, fields) draw for real — ImageRenderer
    /// draws those as placeholders.
    static func render(_ model: AppModel, tab: StudioTab, width: CGFloat, to url: URL) -> CGSize? {
        let host = NSHostingView(rootView: TabPageContent(tab: tab).environmentObject(model)
            .frame(width: width, alignment: .top)
            .background(Studio.background)
            .environment(\.colorScheme, .dark))
        host.frame = NSRect(x: 0, y: 0, width: width, height: 100)
        host.layoutSubtreeIfNeeded()
        let height = max(100, host.fittingSize.height)
        let window = NSWindow(contentRect: NSRect(x: -10_000, y: -10_000, width: width, height: height),
                              styleMask: [.borderless], backing: .buffered, defer: false)
        window.appearance = NSAppearance(named: .darkAqua)
        window.contentView = host
        host.frame = NSRect(x: 0, y: 0, width: width, height: height)
        host.layoutSubtreeIfNeeded()
        host.display()
        guard let bitmap = host.bitmapImageRepForCachingDisplay(in: host.bounds) else { return nil }
        host.cacheDisplay(in: host.bounds, to: bitmap)
        guard let png = bitmap.representation(using: .png, properties: [:]) else { return nil }
        try? png.write(to: url)
        return host.bounds.size
    }

    static func runAndExit(dir: String) -> Never {
        try? FileManager.default.createDirectory(atPath: dir, withIntermediateDirectories: true)
        let model = AppModel()
        seed(model)
        let wanted = ProcessInfo.processInfo.environment["COREVIDEO_SHELL_TAB_LIST"]?
            .split(separator: ",").compactMap { StudioTab(rawValue: String($0)) }
        for tab in wanted ?? StudioTab.navigation where tab != .studio {
            for width in [1360.0, 1920.0] {
                let url = URL(fileURLWithPath: dir)
                    .appendingPathComponent("tab-\(tab.rawValue.lowercased())-\(Int(width)).png")
                let size = render(model, tab: tab, width: CGFloat(width), to: url)
                print("tab snapshot \(url.path) size=\(size.map { "\($0.width)x\($0.height)" } ?? "FAILED")")
            }
        }
        exit(0)
    }
}
