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
                case .sources: SourcesTable()
                case .scenes: ScenesPane()
                case .routing: RoutingPane()
                case .overlays: GraphicsTabPane(graphics: model.graphics)
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
    /// The mixed roster from spec §6, deterministic (no clock reads):
    /// slot 1 Zoom 101 live + talking 1920×1080@30 with its ISO armed; slot 2 Zoom 102
    /// stalled (bus age 2400 ms); slot 3 camera with a mic, live 1280×720, on Program;
    /// slot 4 a screen; slot 5 a camera that is gone (offline).
    static func seed(_ model: AppModel) {
        model.sourceFactsClockForTesting = { 0 }
        model.captureDevices = [
            CaptureDeviceRow(id: "cam-1", name: "Wide camera (Blackmagic)", kind: "video", vendor: "blackmagic",
                             connectionState: "connected", signalPresent: true, warning: ""),
            CaptureDeviceRow(id: "screen:0", name: "Display 1", kind: "video", vendor: "ScreenCaptureKit",
                             connectionState: "detected", signalPresent: false, warning: ""),
        ]
        ShowInputWriteScope.run("snapshot-seed") { seedSlots(model) }
        seedMedia(model)
        model.applyZoomForTesting([
            "meetingState": "in_meeting", "rosterEpoch": "1:1:seed", "rosterRevision": NSNumber(value: 1),
            "participants": [
                ["userId": "101", "displayName": "Jordan Wallace", "videoOn": true, "talking": true,
                 "sourceGeneration": NSNumber(value: 1)],
                ["userId": "102", "displayName": "Guest with a long display name", "videoOn": true,
                 "sourceGeneration": NSNumber(value: 1)],
            ] as [JSONObject]])
        for pid in ["101", "102"] {
            model.handleEventForTesting(["type": "zoom-source-format", "format": [
                "participantId": pid, "rosterEpoch": "1:1:seed", "sourceGeneration": NSNumber(value: 1),
                "width": 1920, "height": 1080, "fps": 30, "frameId": 1, "frameAtMs": 100.0] as JSONObject])
        }
        model.applySnapshotForTesting(["sources": [
            ["sourceId": "101", "kind": "zoom", "width": 1920, "height": 1080, "framesIngested": 300,
             "sinceLastNewFrameMs": 12.0],
            ["sourceId": "102", "kind": "zoom", "width": 1920, "height": 1080, "framesIngested": 90,
             "sinceLastNewFrameMs": 2400.0],
            ["sourceId": "capture:cam-1", "kind": "capture", "width": 1280, "height": 720,
             "framesIngested": 600, "sinceLastNewFrameMs": 10.0],
        ] as [JSONObject]])
        // Program and the ISO writer straight into the facts, so the seed does not
        // drive the recording or program read models of the other tabs.
        model.sourceFacts.mutate {
            $0.applyTally(program: ["capture:cam-1"], preview: [])
            $0.applyWriters(["zoom:101": "armed"])
        }
        seedAudio(model)
        // The inspector open on slot 3 (beside the table at 1920, below it at 1360).
        // COREVIDEO_SHELL_TAB_SNAPSHOT_INSPECTOR=0 renders the table with it closed.
        if ProcessInfo.processInfo.environment["COREVIDEO_SHELL_TAB_SNAPSHOT_INSPECTOR"] != "0" {
            model.sourcesWorkspace.inspect(3)
        }
    }

    /// Audio tab (Phase 2 A4/A6): two Zoom strips (102 muted in Zoom, 101 with
    /// measured gain reduction), default routes, a custom bus sending to master
    /// and LISTEN on aux-1. Measurements are fixture values, never live levels.
    private static func seedAudio(_ model: AppModel) {
        let echo: [JSONObject] = [
            ["participantId": "101", "outputLevel": 70, "inputPeakDbfs": -8.0, "gainReductionDb": 4.5, "status": "live"],
            ["participantId": "102", "outputLevel": 0, "inputPeakDbfs": -14.0, "status": "live"],
        ]
        model.strips = AudioConsolePolicy.mergeStrips(echo: echo, prior: [], expected: ["101", "102"], zoomMuted: ["102"])
        for source in ["101", "102"] {
            for bus in AudioConsolePolicy.defaultSendBuses { model.audioSends[source, default: [:]][bus] = 0 }
        }
        let bus = model.addCustomBus()
        model.audioSends["101", default: [:]][bus.id] = -3
        model.setBusSend(bus.id, target: "master", enabled: true)
        model.setListening("aux-1", on: true)
    }

    private static func seedSlots(_ model: AppModel) {
        model.assignSlot(1, kind: "zoom", sourceId: "101", name: "Jordan Wallace")
        model.assignSlot(2, kind: "zoom", sourceId: "102", name: "Guest with a long display name")
        model.assignSlot(3, kind: "capture", sourceId: "cam-1", name: "Wide camera (Blackmagic)")
        model.slots[2].audioDeviceId = "mic"; model.slots[2].audioDeviceName = "USB Audio Interface"
        model.assignSlot(4, kind: "capture", sourceId: "screen:0", name: "Display 1")
        model.assignSlot(5, kind: "capture", sourceId: "cam-gone", name: "Old desk camera")
        model.toggleSlotIso(1)
    }

    /// The Media tab: synthetic assets (never the operator's bin) and the
    /// COREVIDEO_SHELL_MEDIA_SNAPSHOT states.
    private static func seedMedia(_ model: AppModel) {
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
        GradeSnapshotHarness.render(model: model, dir: dir)   // COREVIDEO_SHELL_GRADE only
        exit(0)
    }
}
