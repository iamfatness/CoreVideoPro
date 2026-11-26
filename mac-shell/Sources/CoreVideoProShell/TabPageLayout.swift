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
        // Graphics: Windows' OverlaysPage fills the page (playlist + a 300 px inspector).
        case .sources, .routing, .diagnose, .overlays: return .infinity
        // Comms fills the window, like Windows (Border margin 12, padding 20, no cap).
        case .comms: return .infinity
        // The Audio tab too (OD-3): SHOW fills the window like the Windows
        // console; SETUP caps its own right column at 820 (Task 5).
        case .audio: return .infinity
        // Forms and cards: a readable measure.
        case .studio, .zoom, .scenes, .media, .automation, .settings: return 720
        }
    }

    /// A page that owns its own scrolling and fills the window's height, so TabPage gives it
    /// no outer ScrollView. Graphics: Windows hosts it in a fixed-height Border, its playlist
    /// and inspector scroll on their own (StudioWorkspace.xaml:725-728). Comms: it pins its
    /// strip and ops strip like Windows' five-row grid (CommsPage.xaml:51-58), and inside an
    /// outer scroll they would scroll away. Audio (OD-3, Windows AudioPage.xaml:31-36): SHOW
    /// is a fixed viewport; its ROUTING and SETUP surfaces scroll inside AudioPane.
    /// Every other tab scrolls as one page.
    static func fillsHeight(_ tab: StudioTab) -> Bool { tab == .overlays || tab == .comms || tab == .audio }

    /// Tiles workspace (Windows SourcesPage Scene Builder `228 | * | 292`): while a
    /// Tiles wall is being edited the Scenes tab takes the window's width.
    static func maxWidth(for tab: StudioTab, tilesEditing: Bool) -> CGFloat {
        tab == .scenes && tilesEditing ? .infinity : maxWidth(for: tab)
    }

    /// Whether the tab scrolls as one page. The Tiles workspace does not: only the
    /// inspector dock's tab content scrolls (each Windows Pivot item is its own ScrollViewer).
    static func scrolls(_ tab: StudioTab, tilesEditing: Bool) -> Bool {
        !(tab == .scenes && tilesEditing) && !fillsHeight(tab)
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
                case .comms: CommsPage(model: model.comms, onLeave: { model.leaveZoom() })
                case .media: MediaPane()
                case .automation: AutomationPane()
                case .settings: SettingsPane()
                case .diagnose: DiagnosePane()
                }
            }
            .frame(maxWidth: TabPageLayout.maxWidth(for: tab, tilesEditing: model.tilesEditTargetId != nil),
                   maxHeight: tab == .comms ? .infinity : nil)
            Spacer(minLength: 0)
        }
        // The Audio tab is a fixed viewport (no page ScrollView): it fills the height.
        .frame(maxHeight: tab == .audio ? .infinity : nil, alignment: .top)
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
            // Slots 6 and 7: capture slots with a paired mic, so the Audio tab's
            // extra strips carry slot names (AppModel.audioSourceLabel) like real ones.
            CaptureDeviceRow(id: "cam-podium", name: "Podium camera", kind: "video", vendor: "avfoundation",
                             connectionState: "connected", signalPresent: true, warning: ""),
            CaptureDeviceRow(id: "cam-stage", name: "Stage camera", kind: "video", vendor: "avfoundation",
                             connectionState: "connected", signalPresent: true, warning: ""),
        ]
        ShowInputWriteScope.run("snapshot-seed") { seedSlots(model) }
        seedMedia(model)
        seedComms(model)
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
        // COREVIDEO_SHELL_TAB_SNAPSHOT_TELEMETRY=1: the Zoom tab's telemetry card with its preview
        // open (an engine-off payload; no service, so the status reads "key missing").
        if ProcessInfo.processInfo.environment["COREVIDEO_SHELL_TAB_SNAPSHOT_TELEMETRY"] == "1" {
            model.telemetryPreviewJSON = TelemetryPayloadBuilder.serializePreview(TelemetryPayloadBuilder.build(
                name: TelemetryEventNames.sessionEnd, version: "1.2.3", sessionLengthSeconds: 3600, snapshot: nil,
                crashCountSinceLastSend: 0,
                machine: TelemetryMachineClass(label: "mac-arm64-cpu12-ram36gb", cpuCores: 12, ramBand: "32-64GB", gpuTier: "apple-m3-pro")))
        }
        // The inspector open on slot 3 (beside the table at 1920, below it at 1360).
        // COREVIDEO_SHELL_TAB_SNAPSHOT_INSPECTOR=0 renders the table with it closed.
        if ProcessInfo.processInfo.environment["COREVIDEO_SHELL_TAB_SNAPSHOT_INSPECTOR"] != "0" {
            model.sourcesWorkspace.inspect(3)
        }
        // COREVIDEO_SHELL_TAB_SNAPSHOT_TILES=1: a CoreVideo Tiles wall on Preview, so the
        // Scenes tab renders the Tiles workspace (mac Tiles Task 15). =style|members picks
        // the inspector tab.
        if let tiles = ProcessInfo.processInfo.environment["COREVIDEO_SHELL_TAB_SNAPSHOT_TILES"], !tiles.isEmpty {
            model.newTilesScene()
            model.tilesInspectorTab = TilesInspectorTab(rawValue: tiles.capitalized) ?? .layout
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
        // A7 Task 4: eight more channels, so the strip bank must scroll under the
        // pinned master rail; an ordered chain on 101, and 101 selected.
        let more: [(id: String, gain: Double, pan: Double, level: Int)] = [
            ("zoom-mix", 0, 0, 40), ("media", -6, 0, 0), ("local-machine-audio", 3, -0.25, 55),
            ("capture:cam-1", -2.5, 0.5, 62), ("capture:screen:0", 0, 0, 0), ("capture:cam-podium", 1.5, -1, 30),
            ("capture:cam-stage", -12, 0.75, 20), ("capture:cam-gone", 0, 0, 0),
        ]
        // One channel routed only to a custom bus, so its chip shows that bus
        // (primaryBusLabel), not the MASTER fallback.
        model.audioSends["capture:cam-stage"] = [bus.id: 0]
        for extra in more {
            model.strips.append(AudioStrip(id: extra.id, outputLevel: extra.level, manualGainDb: extra.gain,
                                           muted: extra.id == "media", solo: false, pan: extra.pan, status: "live"))
        }
        model.editInserts("101") { inserts in
            for name in ["Built-in EQ", "Compressor", "Limiter"] { inserts.add(name) }
            inserts.eqBandsDb[1] = -3; inserts.eqBandsDb[5] = 4
        }
        model.selectAudioStrip("101")
        // A7 Task 5: SETUP opens on bus:master carrying Compressor + Limiter.
        // The SHOW strip selection stays on 101.
        model.busInserts["master"] = ["Compressor", "Limiter"]
        model.selectProcessingTarget(AudioProcessingTargetPolicy.defaultTargetId)
    }

    private static func seedSlots(_ model: AppModel) {
        model.assignSlot(1, kind: "zoom", sourceId: "101", name: "Jordan Wallace")
        model.assignSlot(2, kind: "zoom", sourceId: "102", name: "Guest with a long display name")
        model.assignSlot(3, kind: "capture", sourceId: "cam-1", name: "Wide camera (Blackmagic)")
        model.slots[2].audioDeviceId = "mic"; model.slots[2].audioDeviceName = "USB Audio Interface"
        model.assignSlot(4, kind: "capture", sourceId: "screen:0", name: "Display 1")
        model.assignSlot(5, kind: "capture", sourceId: "cam-gone", name: "Old desk camera")
        model.assignSlot(6, kind: "capture", sourceId: "cam-podium", name: "Podium camera")
        model.slots[5].audioDeviceId = "usb-mic"; model.slots[5].audioDeviceName = "USB mic"
        model.assignSlot(7, kind: "capture", sourceId: "cam-stage", name: "Stage camera")
        model.slots[6].audioDeviceId = "line-in"; model.slots[6].audioDeviceName = "Line in"
        model.toggleSlotIso(1)
    }

    /// The Comms tab (gate on only): COREVIDEO_SHELL_COMMS_SEED names a fact in
    /// contracts/comms/zoom-comms-state.fixtures.json, or not-in-meeting /
    /// waiting; COREVIDEO_SHELL_COMMS_SEED_UI=edit|settings opens a drawer.
    static func seedComms(_ model: AppModel) {
        let env = ProcessInfo.processInfo.environment
        guard CommsGate.enabled, let seed = env["COREVIDEO_SHELL_COMMS_SEED"] else { return }
        model.joinMeetingId = "https://zoom.us/j/1234567890"   // the rail's meeting label
        let comms = model.comms
        switch seed {
        case "not-in-meeting":
            comms.applyMeeting(false, label: nil)
            return
        case "waiting":
            comms.applyMeeting(true, label: "Meeting 123 456 7890")
            comms.apply(.some(nil))
            return
        default:
            comms.applyMeeting(true, label: "Meeting 123 456 7890")
            // The checkout this binary was built from (the fixture is a repo file).
            let url = URL(fileURLWithPath: #filePath).deletingLastPathComponent().deletingLastPathComponent()
                .deletingLastPathComponent().deletingLastPathComponent()
                .appendingPathComponent("contracts/comms/zoom-comms-state.fixtures.json")
            guard let data = try? Data(contentsOf: url),
                  let doc = try? JSONSerialization.jsonObject(with: data) as? JSONObject,
                  let line = (doc["facts"] as? [JSONObject])?.first(where: { $0["name"] as? String == seed })?["line"] as? String,
                  let event = try? JSONSerialization.jsonObject(with: Data(line.utf8)) as? JSONObject,
                  let fact = ZoomCommsProtocol.parseFact(event) else { return }
            comms.apply(fact)
        }
        switch env["COREVIDEO_SHELL_COMMS_SEED_UI"] {
        case "edit": comms.editMode = true
        case "settings": comms.settingsOpen = true
        case "ops": comms.opsOpen = true
        default: break
        }
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
    /// `height` nil = the page's fitting height (a scrolling page drawn whole);
    /// a value renders at a real window height, which a fixed-viewport page
    /// (the Audio SHOW surface) needs: its fitting height is its minimum.
    static func render(_ model: AppModel, tab: StudioTab, width: CGFloat, height fixedHeight: CGFloat? = nil,
                       to url: URL) -> CGSize? {
        let host = NSHostingView(rootView: TabPageContent(tab: tab).environmentObject(model)
            .frame(width: width, height: fixedHeight, alignment: .top)
            .background(Studio.background)
            .environment(\.colorScheme, .dark))
        host.frame = NSRect(x: 0, y: 0, width: width, height: fixedHeight ?? 100)
        host.layoutSubtreeIfNeeded()
        // A page that fills the window has no fitting height of its own: give it a window's
        // unless the harness asked for one (Graphics and Audio a fixed 760; Comms 16:9-ish,
        // so its pinned rows sit where a window puts them).
        let height = fixedHeight ?? (tab == .overlays || tab == .audio ? 760
            : tab == .comms ? (width * 768 / 1360).rounded()
            : max(100, host.fittingSize.height))
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

    /// COREVIDEO_SHELL_TAB_SNAPSHOT_WIDTHS (comma-separated points, e.g.
    /// "900,1360,1920"; 900 is the Audio pop-out minimum); default 1360 and 1920.
    static var snapshotWidths: [Double] {
        let parsed = ProcessInfo.processInfo.environment["COREVIDEO_SHELL_TAB_SNAPSHOT_WIDTHS"]?
            .split(separator: ",").compactMap { Double($0.trimmingCharacters(in: .whitespaces)) }
            .filter { $0 >= 100 } ?? []
        return parsed.isEmpty ? [1360, 1920] : parsed
    }

    /// COREVIDEO_SHELL_TAB_SNAPSHOT_HEIGHT: render at this window height
    /// instead of the page's fitting height (the Audio SHOW viewport).
    static var snapshotHeight: CGFloat? {
        ProcessInfo.processInfo.environment["COREVIDEO_SHELL_TAB_SNAPSHOT_HEIGHT"]
            .flatMap(Double.init).flatMap { $0 >= 100 ? CGFloat($0) : nil }
    }

    static func runAndExit(dir: String) -> Never {
        try? FileManager.default.createDirectory(atPath: dir, withIntermediateDirectories: true)
        let model = AppModel()
        seed(model)
        let wanted = ProcessInfo.processInfo.environment["COREVIDEO_SHELL_TAB_LIST"]?
            .split(separator: ",").compactMap { StudioTab(rawValue: String($0)) }
            .filter { $0 != .comms || CommsGate.enabled }
        for tab in wanted ?? StudioTab.navigation where tab != .studio {
            for width in snapshotWidths {
                let url = URL(fileURLWithPath: dir)
                    .appendingPathComponent("tab-\(tab.rawValue.lowercased())-\(Int(width)).png")
                let size = render(model, tab: tab, width: CGFloat(width), height: snapshotHeight, to: url)
                print("tab snapshot \(url.path) size=\(size.map { "\($0.width)x\($0.height)" } ?? "FAILED")")
            }
        }
        GradeSnapshotHarness.render(model: model, dir: dir)   // COREVIDEO_SHELL_GRADE only
        exit(0)
    }
}
