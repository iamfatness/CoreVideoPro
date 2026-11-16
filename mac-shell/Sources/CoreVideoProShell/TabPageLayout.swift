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
        model.assignSlot(1, kind: "zoom", sourceId: "101", name: "Jordan Wallace")
        model.assignSlot(2, kind: "zoom", sourceId: "102", name: "Guest with a long display name")
        model.assignSlot(3, kind: "capture", sourceId: "cam-1", name: "Wide camera (Blackmagic)")
        model.slots[2].audioDeviceId = "mic"; model.slots[2].audioDeviceName = "USB Audio Interface"
        model.toggleSlotIso(1)
        // The Media tab: one asset per kind, the clip selected. Synthetic rows, not
        // files: a snapshot run must never read or write the operator's bin.
        func asset(_ id: String, _ file: String, _ kind: String, _ ms: Int?, _ w: Int = 0, _ h: Int = 0) -> MediaAsset {
            MediaAsset(id: id, name: (file as NSString).deletingPathExtension, kind: kind,
                       relativePath: kind + "/" + file, filePath: "/snapshot/" + kind + "/" + file,
                       naturalWidth: w, naturalHeight: h, durationMs: ms)
        }
        model.mediaAssets = [
            asset("snap-sting", "Show open.mp4", "stinger", 12_400),
            asset("snap-logo", "Station logo.png", "lower-third", nil, 1920, 1080),
            asset("snap-bed", "Walk-on bed.wav", "audio-bed", 93_000),
            asset("snap-slate", "slate-break.png", "slate", nil, 1920, 1080),
        ]
        model.selectedAssetId = "snap-sting"
        model.mediaStatus = "Show open ready to cue or audition"
        // COREVIDEO_SHELL_MEDIA_SNAPSHOT=empty|cued|program: the Media tab's other states.
        switch ProcessInfo.processInfo.environment["COREVIDEO_SHELL_MEDIA_SNAPSHOT"] {
        case "empty":
            model.mediaAssets = []; model.selectedAssetId = ""; model.mediaStatus = ""
        case "cued", "program":
            model.applySnapshotForTesting(
                ["profile": ["capabilityStates": ["media-clip-playback": ["state": "available"]]]])
            if model.scenes.count >= 2 {
                model.programSceneId = model.scenes[0].id
                model.previewSceneId = model.scenes[1].id
                model.cueSelectedInPreview()
                if ProcessInfo.processInfo.environment["COREVIDEO_SHELL_MEDIA_SNAPSHOT"] == "program" {
                    swap(&model.programSceneId, &model.previewSceneId)
                    model.applySnapshotForTesting(["mediaSources": [[
                        "sourceId": "media:snap-sting", "mediaAssetId": "snap-sting", "state": "live",
                        "onProgram": true, "onPreview": false, "positionMs": 3200.0, "durationMs": 12400.0]]])
                }
            }
        default: break
        }
        model.roster = [
            RosterParticipant(id: "101", name: "Jordan Wallace", hasVideo: true,
                              muted: false, talking: true, assigned: true),
            RosterParticipant(id: "102", name: "Guest with a long display name", hasVideo: true,
                              muted: false, talking: false, assigned: true),
        ]
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
