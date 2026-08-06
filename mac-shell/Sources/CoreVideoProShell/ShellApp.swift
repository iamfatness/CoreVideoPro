// CoreVideo Pro — macOS shell. Composition mirrors the Windows product
// (ground truth: artifacts/alpha-evidence/soak-*.png + StudioWorkspace.xaml):
// header with pill tabs under Produce/Setup group labels + Capture/Zoom Live
// pills, readiness strip, and the Studio workspace — scene rail (numbered
// rows, PGM amber / PVW green), PROGRAM + PREVIEW monitors (preview LEFT),
// show-inputs strip, "Video in room" participant rail — over the transport
// (Magic Scene green, Take amber, Record red, Stream blue) and the OUTPUTS
// status row with the MASTER meter. Palette from App.xaml.

import AppKit
import IOSurface
import SwiftUI

struct StudioPanel: ViewModifier {
    func body(content: Content) -> some View {
        content
            .padding(10)
            .background(RoundedRectangle(cornerRadius: 8).fill(Studio.panel))
            .overlay(RoundedRectangle(cornerRadius: 8).stroke(Studio.stroke, lineWidth: 1))
    }
}

@main
struct ShellApp: App {
    @StateObject private var model = AppModel()

    init() {
        setvbuf(stdout, nil, _IOLBF, 0)
        // Redaction self-check harness (there is no XCTest target in this SPM
        // package): seeds every secret shape the product handles, exports a REAL
        // support bundle to a temp dir and greps it. Runs before any UI.
        //   COREVIDEO_SHELL_SELFCHECK=1 .build/release/CoreVideoProShell
        if ProcessInfo.processInfo.environment["COREVIDEO_SHELL_SELFCHECK"] != nil {
            DiagnosticsSelfCheck.runAndExit()
        }
        // Headless bundle export for the case the UI is what's broken.
        if let destination =
            ProcessInfo.processInfo.environment["COREVIDEO_SHELL_EXPORT_BUNDLE"] {
            DiagnosticsSelfCheck.exportHeadlessAndExit(destination: destination)
        }
        DesignFonts.register()
        // A bare (non-bundled) executable defaults to an activation policy
        // whose windows cannot become KEY — text fields silently refuse
        // input (the engine-bundle lesson). Force Regular + activate.
        NSApplication.shared.setActivationPolicy(.regular)
        DispatchQueue.main.async {
            NSApplication.shared.activate(ignoringOtherApps: true)
        }
    }

    var body: some Scene {
        WindowGroup("CoreVideo Pro") {
            RootView()
                .environmentObject(model)
                .onAppear { model.start() }
                .modifier(PopoutAutoOpen())
                .onOpenURL { url in model.handleOAuthCallback(url) }
                .frame(minWidth: 1360, minHeight: 860)
                .studioChrome()
        }

        // POP-OUTS — `Window`, not `WindowGroup`. WindowGroup mints a NEW window
        // on every openWindow(id:), and macOS also restores the previous
        // session's windows, so an operator ended up with two multiviewers and
        // no idea which was live. These are single-instance operator panels:
        // re-invoking just brings the existing one forward.
        //
        // A live operator runs multiple displays: the monitor wall goes
        // on a second screen while the console stays on the laptop. The WinUI
        // shell ships MultiviewPopoutWindow / ProgramPreviewPopoutWindow /
        // AudioMixerWindow for exactly this; macOS had a SINGLE WindowGroup, so
        // the multiviewer was trapped in a tab and could not be a monitor wall at
        // all. Each pop-out shares the SAME AppModel instance, so it is a second
        // view of live state, never a second copy of it.
        Window("Multiviewer", id: PopoutWindow.multiview) {
            MultiviewerCanvas(isPopout: true)
                .environmentObject(model)
                .padding(8)
                .frame(minWidth: 640, minHeight: 400)
                .studioChrome()
        }

        Window("Program / Preview", id: PopoutWindow.busMonitor) {
            BusMonitorPopout()
                .environmentObject(model)
                .frame(minWidth: 720, minHeight: 260)
                .studioChrome()
        }

        // Picture control, the macOS twin of ColorGradeEditorWindow. A window
        // (not a tab) because grading is done while WATCHING program.
        Window("Color Grade", id: PopoutWindow.colorGrade) {
            ColorGradePane()
                .environmentObject(model)
                .frame(minWidth: 420, minHeight: 320)
                .studioChrome()
        }

        Window("Audio Mixer", id: PopoutWindow.audioMixer) {
            AudioPane(isPopout: true)
                .environmentObject(model)
                .frame(minWidth: 900, minHeight: 520)
                .studioChrome()
        }
    }
}

// Opens pop-outs at launch from COREVIDEO_OPEN_POPOUTS (comma-separated:
// multiview, buses, mixer). Two uses: restoring a multi-display layout without
// re-clicking every show, and letting a headless screenshot/UI check exercise
// the pop-outs (they otherwise require a click, and the design protocol requires
// screenshot proof for UI changes).
struct PopoutAutoOpen: ViewModifier {
    @Environment(\.openWindow) private var openWindow

    func body(content: Content) -> some View {
        content.onAppear {
            guard let raw = ProcessInfo.processInfo.environment["COREVIDEO_OPEN_POPOUTS"],
                  !raw.isEmpty else { return }
            let wanted = raw.split(separator: ",").map {
                $0.trimmingCharacters(in: .whitespaces).lowercased()
            }
            // Slight delay: the main window must finish presenting before a
            // second WindowGroup opens, or the new window can land behind it.
            DispatchQueue.main.asyncAfter(deadline: .now() + 1.0) {
                for name in wanted {
                    switch name {
                    case "multiview": openWindow(id: PopoutWindow.multiview)
                    case "buses", "busmonitor": openWindow(id: PopoutWindow.busMonitor)
                    case "mixer", "audio": openWindow(id: PopoutWindow.audioMixer)
                    case "grade", "colorgrade": openWindow(id: PopoutWindow.colorGrade)
                    default: break
                    }
                }
            }
        }
    }
}

enum PopoutWindow {
    static let multiview = "popout-multiview"
    static let busMonitor = "popout-bus-monitor"
    static let audioMixer = "popout-audio-mixer"
    static let colorGrade = "popout-color-grade"
}

extension View {
    // Shared studio look. Pop-out windows are operator surfaces on a second
    // display, so they must match the console exactly — a light-mode window on a
    // dark stage is a real problem in a control room.
    func studioChrome() -> some View {
        self.font(.grotesk(12))
            .foregroundStyle(Studio.textPrimary)
            .background(Studio.background)
            .tint(Studio.accent)
            .preferredColorScheme(.dark)
    }
}

// Program + preview as a dedicated bus monitor. The multiview composite already
// carries PGM/PVW cells, but on a second display an operator wants the two buses
// LARGE and nothing else — the core already publishes both surfaces separately
// (programSurfaceId / previewSurfaceId); nothing presented them until now.
struct BusMonitorPopout: View {
    @EnvironmentObject var model: AppModel

    var body: some View {
        HStack(spacing: 8) {
            busPane("PREVIEW", surfaceId: model.previewSurfaceId, tally: Studio.accent)
            busPane("PROGRAM", surfaceId: model.programSurfaceId, tally: Studio.red)
        }
        .padding(8)
    }

    func busPane(_ label: String, surfaceId: UInt32, tally: Color) -> some View {
        VStack(spacing: 4) {
            HStack(spacing: 6) {
                Text(label).font(.grotesk(11, .semibold)).foregroundStyle(tally)
                Spacer()
                Text(model.clockText)
                    .font(.plexMono(10)).foregroundStyle(Studio.secondary)
            }
            ZStack {
                Rectangle().fill(Color.black)
                if surfaceId != 0 {
                    SurfaceView(surfaceId: surfaceId)
                } else {
                    // Never a silently black rectangle — that is exactly how
                    // "program and preview are missing" presented before.
                    Text("waiting for \(label.lowercased()) pixels")
                        .font(.plexMono(10)).foregroundStyle(Studio.textDim)
                }
            }
            .aspectRatio(16.0 / 9.0, contentMode: .fit)
            .overlay(Rectangle().stroke(tally, lineWidth: 2))
        }
    }
}

struct RootView: View {
    @EnvironmentObject var model: AppModel

    var body: some View {
        VStack(spacing: 0) {
            HeaderBar()
            ReadinessStrip()
            Group {
                if model.selectedTab == .studio {
                    StudioWorkspaceView()
                } else {
                    TabPage()
                }
            }
            .frame(maxHeight: .infinity)
            TransportBar()
            OutputsStatusRow()
        }
        .background(Studio.background)
    }
}

// ── Header: brand + grouped pill tabs + live pills ───────────────────────────

struct HeaderBar: View {
    @EnvironmentObject var model: AppModel

    var body: some View {
        HStack(spacing: 10) {
            BrandBadge()
            VStack(alignment: .leading, spacing: 0) {
                Text("CoreVideo Pro").font(.grotesk(14, .semibold))
                Text("Live production").font(.grotesk(11))
                    .foregroundStyle(Studio.secondary)
            }
            GroupLabel("Produce")
            TabPill(tab: .studio)
            GroupLabel("Setup")
            ForEach([StudioTab.zoom, .sources, .scenes, .routing], id: \.self) { TabPill(tab: $0) }
            ForEach([StudioTab.overlays, .audio, .media, .automation], id: \.self) {
                TabPill(tab: $0)
            }
            GroupLabel("Diagnose")
            ForEach([StudioTab.diagnose, .settings], id: \.self) { TabPill(tab: $0) }
            Spacer()
            CapturePill()
            if model.meetingState == "in_meeting" {
                LivePill(label: "Zoom Live")
            }
            ConnectionDot()
        }
        .padding(.horizontal, 12)
        .padding(.vertical, 8)
        .background(Studio.panel)
        .overlay(Rectangle().frame(height: 1).foregroundStyle(Studio.stroke),
                 alignment: .bottom)
    }
}

struct GroupLabel: View {
    let text: String
    init(_ text: String) { self.text = text }

    var body: some View {
        Text(text)
            .font(.grotesk(9, .semibold))
            .foregroundStyle(Studio.secondary.opacity(0.7))
            .padding(.leading, 6)
    }
}

struct TabPill: View {
    @EnvironmentObject var model: AppModel
    let tab: StudioTab

    var active: Bool { model.selectedTab == tab }

    var body: some View {
        // Nav button spec: ghost, 12px SemiBold, padding 12x8, 10px radius.
        Button(tab.rawValue) { model.selectedTab = tab }
            .buttonStyle(.plain)
            .font(.grotesk(12, .semibold))
            .foregroundStyle(active ? Studio.accent : Studio.textPrimary)
            .padding(.horizontal, 12)
            .padding(.vertical, 8)
            .background(RoundedRectangle(cornerRadius: 10)
                .fill(active ? Studio.accent.opacity(0.12) : Studio.surface))
            .overlay(RoundedRectangle(cornerRadius: 10)
                .stroke(active ? Studio.accent.opacity(0.7) : Studio.border, lineWidth: 1))
    }
}

// Routing tab — composition per docs/design-reference/04-routing.png: the
// video crosspoint matrix (rows = sources, columns = ISO 1-8 / MULTIVIEW /
// AUX). ISO columns are exclusive and fold into the recording payload's
// isoSourceIds; MULTIVIEW is show membership; AUX is present-but-inert
// (Windows parity). Audio routing lives on the Audio tab.
struct RoutingPane: View {
    @EnvironmentObject var model: AppModel

    let isoColumns = Array(1...8)

    var body: some View {
        VStack(alignment: .leading, spacing: 10) {
            HStack(spacing: 8) {
                Image(systemName: "arrow.triangle.branch")
                    .font(.system(size: 12))  // design-lint: allow
                    .foregroundStyle(Studio.accent)
                Text("Video Routing").font(.grotesk(15, .semibold))
            }
            Text("Click a cell to send a source to ISO record, multiview, or aux. "
                 + "Audio routing is available on the Audio tab.")
                .font(.grotesk(12)).foregroundStyle(Studio.secondary)
            Grid(horizontalSpacing: 2, verticalSpacing: 2) {
                GridRow {
                    Color.clear.frame(width: 170, height: 16)
                    ForEach(isoColumns, id: \.self) { column in
                        MonoLabel("ISO \(column)").frame(width: 48)
                    }
                    MonoLabel("Multiview").frame(width: 72)
                    MonoLabel("Aux", dim: true).frame(width: 48)
                }
                ForEach(model.routingRows) { row in
                    GridRow {
                        Text(row.label)
                            .font(.grotesk(12))
                            .lineLimit(1).truncationMode(.tail)
                            .frame(width: 170, alignment: .leading)
                            .padding(.vertical, 4)
                            .padding(.leading, 6)
                            .background(Rectangle().fill(Studio.surface))
                        ForEach(isoColumns, id: \.self) { column in
                            CrosspointCell(
                                routed: model.isoColumn(of: row.id) == column,
                                enabled: row.id.hasPrefix("zoom:")
                                    || row.id.hasPrefix("capture:")) {
                                model.toggleIsoCell(column: column, sourceId: row.id)
                            }
                            .frame(width: 48)
                        }
                        CrosspointCell(routed: row.inMultiview,
                                       enabled: row.multiviewToggleable) {
                            model.toggleMultiviewCell(sourceId: row.id)
                        }
                        .frame(width: 72)
                        CrosspointCell(routed: false, enabled: false) {}
                            .frame(width: 48)
                    }
                }
            }
            if model.routingRows.count <= 3 {
                Text("Sources appear here as participants join and captures connect.")
                    .font(.grotesk(12)).foregroundStyle(Studio.textDim)
            }
            Spacer()
        }
        .padding(14)
        .background(RoundedRectangle(cornerRadius: 10).fill(Studio.panel))
        .overlay(RoundedRectangle(cornerRadius: 10).stroke(Studio.border, lineWidth: 1))
    }
}

struct CrosspointCell: View {
    let routed: Bool
    let enabled: Bool
    let action: () -> Void

    var body: some View {
        Button(action: action) {
            Text(routed ? "●" : "·")
                .font(.plexMono(10, .semibold))
                .foregroundStyle(routed ? Studio.accent
                                 : enabled ? Studio.secondary : Studio.textDim)
                .frame(maxWidth: .infinity)
                .frame(height: 24)
                .background(Rectangle().fill(
                    routed ? Studio.accent.opacity(0.16) : Studio.field))
                .overlay(Rectangle().stroke(
                    routed ? Studio.accent.opacity(0.7) : Studio.border, lineWidth: 1))
        }
        .buttonStyle(.plain)
        .disabled(!enabled)
    }
}

struct DisabledPill: View {
    let label: String
    init(_ label: String) { self.label = label }

    var body: some View {
        Text(label)
            .font(.grotesk(12))
            .foregroundStyle(Studio.secondary.opacity(0.4))
            .padding(.horizontal, 12)
            .padding(.vertical, 6)
            .background(RoundedRectangle(cornerRadius: 15).fill(Studio.card.opacity(0.5)))
            .help("Coming to the macOS shell")
    }
}

struct CapturePill: View {
    @EnvironmentObject var model: AppModel

    // Truthful two-state readout: label shows the ENGINE-reported raw-media
    // state; the button toggles operator intent.
    var live: Bool { model.rawMediaActive }
    var wanted: Bool { model.captureEnabled }

    var body: some View {
        Button {
            model.setCapture(enabled: !wanted)
        } label: {
            HStack(spacing: 5) {
                Image(systemName: "power").font(.system(size: 10, weight: .bold))  // design-lint: allow
                Text(live ? "Capture On" : wanted ? "Capture starting…" : "Capture Off")
                    .font(.grotesk(12, .medium))
            }
            .foregroundStyle(live ? Studio.accent : wanted ? Studio.amber : Studio.secondary)
            .padding(.horizontal, 12)
            .padding(.vertical, 6)
            .background(Capsule().fill(Studio.card))
            .overlay(Capsule().stroke(
                live ? Studio.accent.opacity(0.7)
                     : wanted ? Studio.amber.opacity(0.7) : Studio.stroke,
                lineWidth: 1))
        }
        .buttonStyle(.plain)
        .help(live ? "Raw media is live — click to stop capture"
                   : "Start capture (requests Zoom recording permission)")
    }
}

struct LivePill: View {
    let label: String

    var body: some View {
        HStack(spacing: 5) {
            Circle().fill(Studio.accent).frame(width: 6, height: 6)
            Text(label).font(.grotesk(12, .medium))
        }
        .foregroundStyle(Studio.accent)
        .padding(.horizontal, 12)
        .padding(.vertical, 6)
        .background(Capsule().fill(Studio.accent.opacity(0.14)))
        .overlay(Capsule().stroke(Studio.accent.opacity(0.7), lineWidth: 1))
    }
}

struct ConnectionDot: View {
    @EnvironmentObject var model: AppModel

    var color: Color {
        switch model.status {
        case .connected: return Studio.accent
        case .launching: return Studio.amber
        case .exited, .failed: return Studio.red
        }
    }

    var help: String {
        switch model.status {
        case .connected(let renderer): return "media core connected (\(renderer))"
        case .launching: return "media core launching…"
        case .exited(let code): return "media core exited (\(code)) — relaunching"
        case .failed(let why): return why
        }
    }

    var body: some View {
        Circle().fill(color).frame(width: 9, height: 9).help(help)
    }
}

// ── Readiness strip ──────────────────────────────────────────────────────────

struct ReadinessStrip: View {
    @EnvironmentObject var model: AppModel

    var ready: Bool {
        if case .connected = model.status { return true }
        return false
    }

    var message: String {
        guard ready else { return "Media core is starting…" }
        if model.meetingState == "in_meeting" {
            return "In meeting. Assign participants, pick a scene, Take it to Program."
        }
        return "Ready to produce. Join Zoom or connect sources, pick a scene, "
            + "Take it to Program, then start Record or Stream."
    }

    var body: some View {
        HStack(spacing: 8) {
            Image(systemName: ready ? "checkmark.circle.fill" : "hourglass")
                .font(.system(size: 11))  // design-lint: allow
                .foregroundStyle(ready ? Studio.accent : .yellow)
            Text("Show readiness").font(.grotesk(11, .semibold))
                .foregroundStyle(ready ? Studio.accent : Studio.secondary)
            Text(message).font(.grotesk(11)).foregroundStyle(Studio.textPrimary)
            Spacer()
            Text("\(model.liveInputCount) assigned · "
                 + "\(model.roster.filter(\.hasVideo).count) Zoom feeds · "
                 + "\(model.captureDevices.filter { $0.connectionState == "connected" }.count)"
                 + " capture")
                .font(.grotesk(11)).foregroundStyle(Studio.secondary)
        }
        .padding(.horizontal, 12)
        .padding(.vertical, 5)
        .background(Studio.line2)
        .overlay(Rectangle().frame(height: 1).foregroundStyle(Studio.stroke),
                 alignment: .bottom)
    }
}

// ── Studio workspace (the Produce tab) ───────────────────────────────────────

struct StudioWorkspaceView: View {
    @EnvironmentObject var model: AppModel

    var body: some View {
        HStack(alignment: .top, spacing: 8) {
            SceneRail().frame(width: 205)
            MultiviewerCanvas()
            VideoRoomRail().frame(width: 200)
        }
        .padding(8)
    }
}

// The Studio center: ONE multiview canvas (reference 01-studio). The core
// composites PREVIEW + PROGRAM + live input tiles into the multiview texture
// in the pgmPvw layout modes — the shell just presents it.
struct MultiviewerCanvas: View {
    @EnvironmentObject var model: AppModel
    @Environment(\.openWindow) private var openWindow
    // The pop-out hosts this same view; it just hides the pop-out affordances.
    var isPopout: Bool = false

    var body: some View {
        VStack(spacing: 6) {
            HStack(spacing: 8) {
                Text("MULTIVIEWER").font(.grotesk(12, .semibold))
                MonoChip("1080p60")
                Text("\(model.liveInputCount) live inputs")
                    .font(.grotesk(11)).foregroundStyle(Studio.secondary)
                Spacer()
                Text(model.clockText)
                    .font(.plexMono(12, .medium)).foregroundStyle(Studio.textPrimary)
                Menu {
                    Button("P/P top") { model.configureMultiviewer(mode: "pgmPvwTop") }
                    Button("P/P large") { model.configureMultiviewer(mode: "pgmPvwLarge") }
                    Button("P/P side") { model.configureMultiviewer(mode: "pgmPvwSide") }
                    Button("Grid") { model.configureMultiviewer(mode: "grid") }
                } label: {
                    HStack(spacing: 4) {
                        Image(systemName: "rectangle.split.2x2").font(.system(size: 9))  // design-lint: allow
                        Text("Edit layout").font(.grotesk(11, .semibold))
                    }
                }
                .menuStyle(.borderlessButton)
                .fixedSize()
                .foregroundStyle(Studio.textPrimary)
                ActionChip("Scene builder", accent: true) { model.selectedTab = .scenes }
                HStack(spacing: 4) {
                    Text("LT \(model.lowerThirdPhase == "hidden" ? "out" : "in")")
                        .font(.plexMono(10, .semibold))
                        .foregroundStyle(model.lowerThirdPhase == "hidden"
                                         ? Studio.secondary : Studio.accent)
                }
                .padding(.horizontal, 8).padding(.vertical, 4)
                .background(RoundedRectangle(cornerRadius: 8).fill(Studio.surface))
                .overlay(RoundedRectangle(cornerRadius: 8)
                    .stroke(Studio.border, lineWidth: 1))
                ActionChip("Room") { model.selectedTab = .zoom }
                // Send the wall to a second display. Hidden inside the pop-out
                // itself so it cannot spawn copies of its own window.
                ActionChip("Grade") { openWindow(id: PopoutWindow.colorGrade) }
                if !isPopout {
                    ActionChip("Pop out") { openWindow(id: PopoutWindow.multiview) }
                    ActionChip("P/P") { openWindow(id: PopoutWindow.busMonitor) }
                }
            }
            ZStack {
                Rectangle().fill(Color.black)
                if model.multiviewSurfaceId != 0 {
                    SurfaceView(surfaceId: model.multiviewSurfaceId)
                }
                // Tile chrome is the SHELL's job (the WinUI multiview-host
                // pattern): the core composites pixels, we draw the labels and
                // tally borders from tiles[]. Without this a PGM/PVW cell with
                // no source is an invisible black rectangle — which is exactly
                // how "program and preview are missing" presented.
                GeometryReader { geometry in
                    ForEach(model.multiviewTiles) { tile in
                        MultiviewTileChrome(tile: tile)
                            .frame(width: max(1, tile.w * geometry.size.width),
                                   height: max(1, tile.h * geometry.size.height))
                            .offset(x: tile.x * geometry.size.width,
                                    y: tile.y * geometry.size.height)
                            // Source tiles are the bus: click cues that source
                            // solo to PREVIEW (StudioViewModel.PreviewMultiviewTile).
                            .onTapGesture {
                                guard tile.role == "source" else { return }
                                if let slot = model.slotForSourceId(tile.id) {
                                    model.previewMultiviewSlot(slot)
                                }
                            }
                    }
                }
                if model.multiviewTiles.isEmpty {
                    VStack(spacing: 6) {
                        Text("Multiviewer")
                            .font(.grotesk(14, .semibold))
                            .foregroundStyle(Studio.secondary)
                        Text("Join Zoom or connect sources, assign inputs, "
                             + "pick a scene — preview and program composite here.")
                            .font(.grotesk(12)).foregroundStyle(Studio.textDim)
                    }
                }
            }
            .frame(maxWidth: .infinity, maxHeight: .infinity)
            .overlay(Rectangle().stroke(Studio.border, lineWidth: 1))
        }
        .modifier(StudioPanel())
    }
}

// PGM = amber/red tally, PVW = green, sources = neutral (active speaker
// lights green). Label bar sits at the bottom of the cell like the reference.
struct MultiviewTileChrome: View {
    let tile: MultiviewTile

    var accent: Color {
        switch tile.role {
        case "pgm": return Studio.red
        case "pvw": return Studio.accent
        default:
            if tile.tally == "program" { return Studio.red }
            if tile.tally == "preview" { return Studio.accent }
            return tile.activeSpeaker ? Studio.accent : Studio.border
        }
    }

    var title: String {
        switch tile.role {
        case "pgm": return "PROGRAM"
        case "pvw": return "PREVIEW"
        default: return tile.label.isEmpty ? "Source" : tile.label
        }
    }

    var isBus: Bool { tile.role == "pgm" || tile.role == "pvw" }

    var body: some View {
        ZStack(alignment: .bottomLeading) {
            Rectangle().fill(Color.clear)
            HStack(spacing: 4) {
                Text(title)
                    .font(.grotesk(11, isBus ? .semibold : .regular))
                    .foregroundStyle(isBus ? Studio.onAccent : Studio.textPrimary)
                    .lineLimit(1)
                if !isBus, !tile.label.isEmpty, tile.activeSpeaker {
                    Text("LIVE").font(.plexMono(8, .semibold))
                        .foregroundStyle(Studio.onAccent)
                }
                Spacer()
            }
            .padding(.horizontal, 6).padding(.vertical, 3)
            .frame(maxWidth: .infinity, alignment: .leading)
            .background(Rectangle().fill(
                isBus ? accent : Color.black.opacity(0.55)))
        }
        .overlay(Rectangle().stroke(accent, lineWidth: isBus ? 3 : 2))
        .contentShape(Rectangle())
        .allowsHitTesting(!isBus)
    }
}

struct MonoChip: View {
    let text: String
    init(_ text: String) { self.text = text }

    var body: some View {
        Text(text)
            .font(.plexMono(10, .semibold))
            .padding(.horizontal, 6).padding(.vertical, 2)
            .background(RoundedRectangle(cornerRadius: 4).fill(Studio.surface))
            .foregroundStyle(Studio.secondary)
    }
}

struct ActionChip: View {
    let label: String
    var accent = false
    let action: () -> Void

    init(_ label: String, accent: Bool = false, action: @escaping () -> Void) {
        self.label = label
        self.accent = accent
        self.action = action
    }

    var body: some View {
        Button(label, action: action)
            .buttonStyle(.plain)
            .font(.grotesk(11, .semibold))
            .foregroundStyle(accent ? Studio.accent : Studio.textPrimary)
            .padding(.horizontal, 10).padding(.vertical, 4)
            .background(RoundedRectangle(cornerRadius: 8)
                .fill(accent ? Studio.accent.opacity(0.12) : Studio.surface))
            .overlay(RoundedRectangle(cornerRadius: 8)
                .stroke(accent ? Studio.accent.opacity(0.6) : Studio.border, lineWidth: 1))
    }
}

// ── Scene rail ───────────────────────────────────────────────────────────────

struct SceneRail: View {
    @EnvironmentObject var model: AppModel

    var programName: String {
        model.scenes.first { $0.id == model.programSceneId }?.name ?? "—"
    }

    var previewName: String {
        // A solo cue shows the SOURCE, not a scene name — the rail has no row
        // for the reserved solo scenes.
        if model.previewSceneId == AppModel.soloSceneA
            || model.previewSceneId == AppModel.soloSceneB {
            if let slotId = model.soloSlotId,
               let slot = model.slots.first(where: { $0.id == slotId }) {
                return slot.name.isEmpty ? "Input \(slotId)" : slot.name
            }
            return "Solo"
        }
        return model.scenes.first { $0.id == model.previewSceneId }?.name ?? "—"
    }

    var body: some View {
        VStack(alignment: .leading, spacing: 8) {
            Text("Scenes").font(.grotesk(13, .semibold))
            Text("\(model.scenes.count) scenes · 1920x1080 · 60 fps")
                .font(.grotesk(10)).foregroundStyle(Studio.secondary)
            HStack(spacing: 6) {
                SceneChip(label: "PGM \(programName)", color: Studio.amber)
                SceneChip(label: "PVW \(previewName)", color: Studio.accent)
            }
            ForEach(Array(model.railScenes.enumerated()), id: \.element.id) { index, scene in
                SceneRow(index: index + 1, scene: scene)
            }
            Text("Tap a scene to queue on preview · Take swaps PVW and PGM")
                .font(.grotesk(9)).foregroundStyle(Studio.secondary.opacity(0.8))
            Button {
                model.selectedTab = .scenes
            } label: {
                HStack {
                    Image(systemName: "square.grid.2x2").font(.system(size: 10))  // design-lint: allow
                    Text("Open Scene builder").font(.system(size: 11, weight: .medium))  // design-lint: allow
                }
                .foregroundStyle(Studio.accent)
                .frame(maxWidth: .infinity)
                .padding(.vertical, 7)
                .background(RoundedRectangle(cornerRadius: 6)
                    .fill(Studio.accent.opacity(0.12)))
                .overlay(RoundedRectangle(cornerRadius: 6)
                    .stroke(Studio.accent.opacity(0.6), lineWidth: 1))
            }
            .buttonStyle(.plain)
            Spacer()
        }
        .modifier(StudioPanel())
    }
}

struct SceneRow: View {
    @EnvironmentObject var model: AppModel
    let index: Int
    let scene: SceneDef

    var onProgram: Bool { scene.id == model.programSceneId }
    var onPreview: Bool { scene.id == model.previewSceneId }

    var body: some View {
        Button {
            model.selectPreviewScene(scene.id)
        } label: {
            HStack(spacing: 8) {
                Circle()
                    .fill(onProgram ? Studio.amber : Studio.surfaceRaised)
                    .frame(width: 24, height: 24)
                    .overlay(Text("\(index)")
                        .font(.grotesk(11, .bold))
                        .foregroundStyle(onProgram ? .black : .white))
                VStack(alignment: .leading, spacing: 1) {
                    Text(scene.name).font(.grotesk(12, .medium))
                        .foregroundStyle(Studio.textPrimary)
                    Text(scene.layout).font(.grotesk(9))
                        .foregroundStyle(Studio.secondary)
                }
                Spacer()
                if onProgram {
                    SceneChip(label: "PGM", color: Studio.amber)
                } else if onPreview {
                    SceneChip(label: "PVW", color: Studio.accent)
                } else {
                    Text("IDLE").font(.plexMono(8, .bold))
                        .foregroundStyle(Studio.secondary.opacity(0.6))
                }
            }
            .padding(8)
            .background(RoundedRectangle(cornerRadius: 8)
                .fill(onProgram ? Studio.amber.opacity(0.12)
                      : onPreview ? Studio.accent.opacity(0.08) : Studio.card))
            .overlay(RoundedRectangle(cornerRadius: 8)
                .stroke(onProgram ? Studio.amber.opacity(0.7)
                        : onPreview ? Studio.accent.opacity(0.6) : Studio.stroke,
                        lineWidth: 1))
        }
        .buttonStyle(.plain)
    }
}

struct SceneChip: View {
    let label: String
    let color: Color

    var body: some View {
        Text(label)
            .font(.plexMono(9, .bold))
            .lineLimit(1)
            .padding(.horizontal, 6).padding(.vertical, 2)
            .background(RoundedRectangle(cornerRadius: 4).fill(color.opacity(0.16)))
            .overlay(RoundedRectangle(cornerRadius: 4).stroke(color.opacity(0.7),
                                                              lineWidth: 1))
            .foregroundStyle(color)
    }
}

// ── Video in room rail ───────────────────────────────────────────────────────

struct VideoRoomRail: View {
    @EnvironmentObject var model: AppModel

    var body: some View {
        VStack(alignment: .leading, spacing: 8) {
            Text("Video in room (\(model.roster.count))")
                .font(.grotesk(13, .semibold))
            Text(model.meetingState == "in_meeting" ? "Main room" : "Not in a meeting")
                .font(.grotesk(10)).foregroundStyle(Studio.secondary)
            ScrollView {
                VStack(spacing: 6) {
                    ForEach(model.roster) { participant in
                        ParticipantCard(participant: participant)
                    }
                }
            }
            Spacer(minLength: 0)
            Button {
                model.selectedTab = .zoom
            } label: {
                Text(model.meetingState == "in_meeting" ? "Manage meeting" : "Join meeting")
                    .font(.grotesk(11, .medium))
                    .frame(maxWidth: .infinity)
                    .padding(.vertical, 7)
                    .background(RoundedRectangle(cornerRadius: 6).fill(Studio.card))
                    .overlay(RoundedRectangle(cornerRadius: 6).stroke(Studio.stroke,
                                                                      lineWidth: 1))
            }
            .buttonStyle(.plain)
        }
        .modifier(StudioPanel())
    }
}

struct ParticipantCard: View {
    @EnvironmentObject var model: AppModel
    let participant: RosterParticipant

    var assigned: Bool { model.assignedIds.contains(participant.id) }

    var body: some View {
        HStack(spacing: 8) {
            RoundedRectangle(cornerRadius: 6)
                .fill(Studio.surfaceRaised)
                .frame(width: 30, height: 30)
                .overlay(Text(String(participant.name.prefix(1)).lowercased())
                    .font(.grotesk(13, .semibold))
                    .foregroundStyle(Studio.textPrimary))
            VStack(alignment: .leading, spacing: 1) {
                Text(participant.name).font(.grotesk(11, .medium))
                    .lineLimit(1)
                HStack(spacing: 4) {
                    Text("Guest").font(.grotesk(9)).foregroundStyle(Studio.secondary)
                    if participant.talking {
                        Text("Talking").font(.grotesk(9))
                            .foregroundStyle(Studio.accent)
                    }
                    if participant.muted {
                        Image(systemName: "mic.slash.fill").font(.system(size: 8))  // design-lint: allow
                            .foregroundStyle(Studio.secondary)
                    }
                }
            }
            Spacer()
            Button(assigned ? "−" : "+") { model.toggleAssigned(participant) }
                .buttonStyle(.plain)
                .font(.grotesk(13, .bold))
                .foregroundStyle(assigned ? Studio.amber : Studio.accent)
                .frame(width: 22, height: 22)
                .background(RoundedRectangle(cornerRadius: 5).fill(Studio.card))
                .help(assigned ? "Remove from show" : "Assign to show")
        }
        .padding(6)
        .background(RoundedRectangle(cornerRadius: 8)
            .fill(participant.talking ? Studio.accent.opacity(0.08) : Studio.card))
        .overlay(RoundedRectangle(cornerRadius: 8)
            .stroke(participant.talking ? Studio.accent.opacity(0.6)
                    : assigned ? Studio.stroke : Studio.stroke.opacity(0.5),
                    lineWidth: 1))
    }
}

// ── Non-studio tab pages ─────────────────────────────────────────────────────

struct TabPage: View {
    @EnvironmentObject var model: AppModel

    var body: some View {
        ScrollView {
            HStack {
                Spacer(minLength: 0)
                Group {
                    switch model.selectedTab {
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
                .frame(maxWidth: 720)
                Spacer(minLength: 0)
            }
            .padding(12)
        }
    }
}

// ── IOSurface presenter ──────────────────────────────────────────────────────

struct SurfaceView: NSViewRepresentable {
    let surfaceId: UInt32

    func makeNSView(context: Context) -> SurfaceNSView { SurfaceNSView() }

    func updateNSView(_ view: SurfaceNSView, context: Context) {
        view.present(surfaceId: surfaceId)
    }
}

final class SurfaceNSView: NSView {
    private var currentSurfaceId: UInt32 = 0
    private var surface: IOSurfaceRef?
    private var refreshTimer: Timer?

    override init(frame frameRect: NSRect) {
        super.init(frame: frameRect)
        wantsLayer = true
        layer?.backgroundColor = NSColor.black.cgColor
        layer?.contentsGravity = .resizeAspect
        // The surface is the compositor's LIVE render target; re-marking the
        // layer contents at display cadence picks up new frames zero-copy.
        refreshTimer = Timer.scheduledTimer(withTimeInterval: 1.0 / 60.0, repeats: true) {
            [weak self] _ in
            guard let self, let layer = self.layer, let surface = self.surface else { return }
            // Re-assigning the SAME contents object is a Core Animation no-op
            // (the first latch displayed forever — black). Detach + re-attach
            // inside a no-action transaction forces CA to re-texture from the
            // live IOSurface each display tick; still zero-copy.
            CATransaction.begin()
            CATransaction.setDisableActions(true)
            layer.contents = nil
            layer.contents = surface
            CATransaction.commit()
        }
    }

    @available(*, unavailable)
    required init?(coder: NSCoder) { fatalError() }

    deinit { refreshTimer?.invalidate() }

    func present(surfaceId: UInt32) {
        guard surfaceId != 0, surfaceId != currentSurfaceId else { return }
        guard let looked = IOSurfaceLookup(surfaceId) else {
            // A failed lookup means the core exported a non-global surface —
            // the exact silent-black failure this log line exists to catch.
            ShellLog.write("IOSurfaceLookup FAILED for id \(surfaceId)")
            return
        }
        currentSurfaceId = surfaceId
        surface = looked
        layer?.contents = looked
    }
}

// ── Transport bar ────────────────────────────────────────────────────────────

struct TransportBar: View {
    @EnvironmentObject var model: AppModel

    var recording: Bool {
        model.recordingStatus == "recording" || model.recordingStatus == "warning"
    }

    var outputsLabel: String {
        var live: [String] = []
        if recording { live.append("recording") }
        if model.streamingDesired { live.append("streaming") }
        return live.isEmpty ? "Outputs idle" : "Outputs live: " + live.joined(separator: " + ")
    }

    var body: some View {
        HStack(spacing: 10) {
            // Magic Scene = auto-direct with auto-take, the one-tap AI pill.
            Button {
                model.autoTakeEnabled = true
                model.setAutoDirect(enabled: !model.autoDirectEnabled)
            } label: {
                HStack(spacing: 5) {
                    Image(systemName: "sparkles").font(.system(size: 10))  // design-lint: allow
                    VStack(alignment: .leading, spacing: 0) {
                        Text("Magic Scene").font(.grotesk(11, .semibold))
                        Text("AI auto-direct").font(.grotesk(8))
                    }
                }
                .foregroundStyle(model.autoDirectEnabled ? .black : Studio.accent)
                .padding(.horizontal, 10).padding(.vertical, 5)
                .background(RoundedRectangle(cornerRadius: 8)
                    .fill(model.autoDirectEnabled ? Studio.accent
                          : Studio.accent.opacity(0.12)))
                .overlay(RoundedRectangle(cornerRadius: 8)
                    .stroke(Studio.accent.opacity(0.7), lineWidth: 1))
            }
            .buttonStyle(.plain)
            HStack(spacing: 6) {
                VStack(alignment: .leading, spacing: 0) {
                    Text("Set & Forget").font(.grotesk(10, .medium))
                    Text("Automation \(model.autoDirectEnabled ? "On" : "Off")")
                        .font(.grotesk(8)).foregroundStyle(Studio.secondary)
                }
                Toggle("", isOn: Binding(
                    get: { model.autoDirectEnabled },
                    set: { model.setAutoDirect(enabled: $0) }))
                    .toggleStyle(.switch)
                    .controlSize(.mini)
                    .labelsHidden()
            }
            .padding(.horizontal, 8).padding(.vertical, 4)
            .background(RoundedRectangle(cornerRadius: 8).fill(Studio.card))
            HStack(spacing: 4) {
                Text("LT")
                    .font(.grotesk(10, .bold))
                Text(model.lowerThirdPhase == "hidden" ? "Ready" : model.lowerThirdPhase)
                    .font(.grotesk(9))
                    .foregroundStyle(model.lowerThirdPhase == "hidden"
                                     ? Studio.secondary : Studio.accent)
            }
            .padding(.horizontal, 8).padding(.vertical, 6)
            .background(RoundedRectangle(cornerRadius: 8).fill(Studio.card))
            // Preview / DSK chips (the reference transport). Preview mirrors
            // whether a scene is armed; DSK reflects the lower-third key.
            TransportChip(title: "Preview",
                          detail: model.previewSceneId.isEmpty ? "Off" : "Armed",
                          lit: !model.previewSceneId.isEmpty)
            TransportChip(title: "DSK",
                          detail: model.lowerThirdPhase == "hidden" ? "Off" : "In",
                          lit: model.lowerThirdPhase != "hidden")
            Spacer()
            Text(outputsLabel).font(.grotesk(10)).foregroundStyle(Studio.secondary)
            // Take — amber, the director's commit button.
            Button {
                model.take()
            } label: {
                HStack(spacing: 4) {
                    Text("Take").font(.grotesk(12, .bold))
                    Text("Cut").font(.grotesk(9))
                        .foregroundStyle(Studio.onAccent.opacity(0.7))
                }
                .foregroundStyle(Studio.onAccent)
                .padding(.horizontal, 14).padding(.vertical, 7)
                .background(RoundedRectangle(cornerRadius: 8).fill(
                    model.previewSceneId.isEmpty
                        || model.previewSceneId == model.programSceneId
                        ? Studio.amber.opacity(0.35) : Studio.amber))
            }
            .buttonStyle(.plain)
            .disabled(model.previewSceneId.isEmpty
                      || model.previewSceneId == model.programSceneId)
            // Record — red outline with format subtitle.
            Button {
                model.toggleRecording()
            } label: {
                HStack(spacing: 5) {
                    Image(systemName: recording ? "stop.fill" : "record.circle")
                        .font(.system(size: 10))  // design-lint: allow
                    VStack(alignment: .leading, spacing: 0) {
                        Text(recording ? "Stop Rec" : "Record")
                            .font(.grotesk(11, .semibold))
                        Text(model.isoRecordingEnabled
                             ? "MP4 + \(model.resolvedIsoSourceIds().count) ISOs" : "MP4")
                            .font(.grotesk(8)).foregroundStyle(Studio.secondary)
                    }
                }
                .foregroundStyle(recording ? .white : Studio.red)
                .padding(.horizontal, 10).padding(.vertical, 5)
                .background(RoundedRectangle(cornerRadius: 8)
                    .fill(recording ? Studio.red : Studio.red.opacity(0.10)))
                .overlay(RoundedRectangle(cornerRadius: 8)
                    .stroke(Studio.red.opacity(0.8), lineWidth: 1))
            }
            .buttonStyle(.plain)
            Toggle("ISOs", isOn: $model.isoRecordingEnabled)
                .toggleStyle(.checkbox)
                .font(.grotesk(10))
                .disabled(recording)
            StreamControl()
            VirtualCamChip()
        }
        .padding(.horizontal, 12)
        .padding(.vertical, 7)
        .background(Studio.panel)
        .overlay(Rectangle().frame(height: 1).foregroundStyle(Studio.stroke),
                 alignment: .top)
    }
}

// ── Stream control (blue, transport) ─────────────────────────────────────────

struct TransportChip: View {
    let title: String
    let detail: String
    let lit: Bool

    var body: some View {
        VStack(alignment: .leading, spacing: 0) {
            Text(title).font(.grotesk(11, .semibold))
            Text(detail).font(.grotesk(9))
                .foregroundStyle(lit ? Studio.accent : Studio.secondary)
        }
        .padding(.horizontal, 8).padding(.vertical, 5)
        .background(RoundedRectangle(cornerRadius: 8).fill(Studio.card))
        .overlay(RoundedRectangle(cornerRadius: 8)
            .stroke(lit ? Studio.accent.opacity(0.5) : Studio.border, lineWidth: 1))
    }
}

// Virtual camera: the CoreMediaIO system extension needs a Developer ID
// (an owner-blocked item), so the control states that plainly instead of
// pretending to be available.
struct VirtualCamChip: View {
    var body: some View {
        VStack(alignment: .leading, spacing: 0) {
            Text("Virtual Cam").font(.grotesk(11, .semibold))
            Text("Needs Developer ID").font(.grotesk(9))
                .foregroundStyle(Studio.textDim)
        }
        .padding(.horizontal, 8).padding(.vertical, 5)
        .background(RoundedRectangle(cornerRadius: 8).fill(Studio.card))
        .overlay(RoundedRectangle(cornerRadius: 8).stroke(Studio.border, lineWidth: 1))
        .opacity(0.6)
        .help("The CoreMediaIO camera extension requires a signed, notarized "
              + "build — blocked on the Apple Developer ID.")
    }
}

struct StreamControl: View {
    @EnvironmentObject var model: AppModel
    @State private var showSettings = false

    var live: Bool { model.streamingDesired }

    var body: some View {
        HStack(spacing: 4) {
            Button {
                if live {
                    model.toggleStreaming()
                } else if model.streamKey.isEmpty {
                    showSettings = true
                } else {
                    model.toggleStreaming()
                }
            } label: {
                HStack(spacing: 5) {
                    Image(systemName: "dot.radiowaves.left.and.right")
                        .font(.system(size: 10))  // design-lint: allow
                    VStack(alignment: .leading, spacing: 0) {
                        Text(live ? "Stop Stream" : "Stream")
                            .font(.grotesk(11, .semibold))
                        Text(live ? (model.streamDetail.isEmpty ? "starting…"
                                     : model.streamDetail)
                             : "RTMP 6 Mbps")
                            .font(.grotesk(8)).foregroundStyle(
                                live ? Studio.blue : Studio.secondary)
                            .lineLimit(1)
                    }
                }
                .foregroundStyle(live ? .white : Studio.blue)
                .padding(.horizontal, 10).padding(.vertical, 5)
                .background(RoundedRectangle(cornerRadius: 8)
                    .fill(live ? Studio.blue : Studio.blue.opacity(0.10)))
                .overlay(RoundedRectangle(cornerRadius: 8)
                    .stroke(Studio.blue.opacity(0.8), lineWidth: 1))
            }
            .buttonStyle(.plain)
            Button {
                showSettings = true
            } label: {
                Image(systemName: "gearshape").font(.system(size: 10))  // design-lint: allow
            }
            .buttonStyle(.plain)
            .foregroundStyle(Studio.secondary)
        }
        .popover(isPresented: $showSettings) {
            VStack(alignment: .leading, spacing: 8) {
                Text("Stream settings").font(.grotesk(14, .semibold))
                TextField("RTMP URL", text: $model.streamUrl)
                    .textFieldStyle(StudioFieldStyle())
                SecureField("Stream key (stored in Keychain)", text: $model.streamKey)
                    .textFieldStyle(StudioFieldStyle())
                HStack {
                    Spacer()
                    Button(live ? "Stop streaming" : "Start streaming") {
                        model.toggleStreaming()
                        showSettings = false
                    }
                    .disabled(!live && (model.streamKey.isEmpty || model.streamUrl.isEmpty))
                }
                Text("1080p30 H.264 (VideoToolbox via ffmpeg) · 6 Mbps CBR · AAC 160 kbps")
                    .font(.grotesk(10)).foregroundStyle(Studio.secondary)
            }
            .padding(14)
            .frame(width: 380)
        }
    }
}

// ── OUTPUTS status row ───────────────────────────────────────────────────────

struct OutputsStatusRow: View {
    @EnvironmentObject var model: AppModel

    var recording: Bool {
        model.recordingStatus == "recording" || model.recordingStatus == "warning"
    }

    var lufsLabel: String {
        model.shortTermLufs <= -119 ? "—" : String(format: "%.1f LUFS", model.shortTermLufs)
    }

    var peakLabel: String {
        model.truePeakDbfs <= -119 ? "" : String(format: "Peak %.1f dBFS", model.truePeakDbfs)
    }

    var body: some View {
        HStack(spacing: 14) {
            HStack(spacing: 4) {
                Circle().fill(Studio.accent).frame(width: 6, height: 6)
                Text("OUTPUTS").font(.plexMono(9, .bold))
                    .foregroundStyle(Studio.accent)
            }
            OutputColumn(title: "PROGRAM", value: "1080p60",
                         detail: model.programSceneId.isEmpty ? "idle" : "live")
            OutputColumn(title: "STREAM",
                         value: model.streamingDesired ? "6 Mbps" : "—",
                         detail: model.streamingDesired
                             ? (model.streamStatus.isEmpty ? "starting" : model.streamStatus)
                             : "Idle")
            OutputColumn(title: "RECORD",
                         value: recording ? "MP4" : "—",
                         detail: recording ? "recording" : "Idle")
            if recording, let started = model.recordingStartedAt {
                TimelineView(.periodic(from: started, by: 1)) { context in
                    OutputColumn(
                        title: "LIVE",
                        value: elapsedString(from: started, to: context.date),
                        detail: "")
                }
            }
            Spacer()
            Toggle(isOn: Binding(
                get: { model.monitorEnabled },
                set: { model.setMonitor(enabled: $0, volume: model.monitorVolume) })) {
                Text("Monitor").font(.grotesk(11, .medium))
            }
            .toggleStyle(.button)
            .tint(Studio.blue)
            VStack(alignment: .leading, spacing: 1) {
                HStack(spacing: 6) {
                    Text("MASTER")
                        .font(.plexMono(9, .bold))
                        .foregroundStyle(Studio.secondary)
                    Text(lufsLabel).font(.plexMono(11, .semibold))
                }
                MeterBar(level: model.masterLevel).frame(width: 170)
            }
            if !peakLabel.isEmpty {
                Text(peakLabel).font(.plexMono(9))
                    .foregroundStyle(Studio.secondary)
            }
            Text(model.clockText).font(.plexMono(10))
                .foregroundStyle(Studio.secondary)
        }
        .padding(.horizontal, 12)
        .padding(.vertical, 6)
        .background(Studio.background)
    }

    private func elapsedString(from start: Date, to now: Date) -> String {
        let seconds = max(0, Int(now.timeIntervalSince(start)))
        return String(format: "%02d:%02d", seconds / 60, seconds % 60)
    }
}

struct OutputColumn: View {
    let title: String
    let value: String
    let detail: String

    var body: some View {
        VStack(alignment: .leading, spacing: 0) {
            Text(title).font(.plexMono(8, .bold))
                .foregroundStyle(Studio.secondary)
            Text(value).font(.grotesk(10, .semibold))
            if !detail.isEmpty {
                Text(detail).font(.grotesk(8)).foregroundStyle(Studio.secondary)
            }
        }
    }
}

// ── Panes (tab pages) ────────────────────────────────────────────────────────

// Zoom tab — composition per docs/design-reference/02-zoom.png: the
// "Zoom connection" card (URL field · role name · Webinar), status lines,
// Recent meetings, the Zoom-account sign-in block, Join Zoom / Open in Zoom
// app, then the bordered "Zoom status" card. Status text is engine-reported
// truth, never optimism.
struct ZoomPane: View {
    @EnvironmentObject var model: AppModel

    var inMeeting: Bool { model.meetingState == "in_meeting" }

    var statusHeadline: String {
        switch model.meetingState {
        case "in_meeting": return "In meeting"
        case "joining": return "Joining…"
        case "error": return "Join failed"
        default: return "Ready"
        }
    }

    var statusDetail: String {
        if inMeeting {
            return model.rawMediaActive
                ? "Connected — raw media live"
                : "Connected — capture is off"
        }
        return model.meetingState == "error" ? "See Diagnose for the engine warning"
                                             : "Not connected"
    }

    var body: some View {
        VStack(alignment: .leading, spacing: 12) {
            VStack(alignment: .leading, spacing: 10) {
                Text("Zoom connection").font(.grotesk(14, .semibold))
                HStack(spacing: 8) {
                    TextField("https://zoom.us/j/123456789", text: $model.joinMeetingId)
                        .textFieldStyle(StudioFieldStyle())
                        .frame(width: 220)
                    TextField("CoreVideo Producer", text: $model.displayName)
                        .textFieldStyle(StudioFieldStyle())
                        .frame(width: 170)
                    Toggle("Webinar", isOn: $model.webinar)
                        .toggleStyle(.checkbox)
                        .font(.grotesk(12))
                }
                TextField("Passcode (from the link when present)",
                          text: $model.joinPasscode)
                    .textFieldStyle(StudioFieldStyle())
                    .frame(width: 220)
                Text(statusHeadline)
                    .font(.grotesk(12, .medium))
                    .foregroundStyle(inMeeting || model.meetingState == "joining"
                                     ? Studio.accent
                                     : model.meetingState == "error" ? Studio.red
                                                                     : Studio.accent)
                Text(statusDetail).font(.grotesk(12)).foregroundStyle(Studio.secondary)
                Text("Recent meetings").font(.grotesk(13, .semibold))
                Menu {
                    if model.recentMeetings.isEmpty {
                        Button("No recent meetings") {}.disabled(true)
                    }
                    ForEach(model.recentMeetings, id: \.self) { recent in
                        Button(recent) { model.joinMeetingId = recent }
                    }
                } label: {
                    Text(model.recentMeetings.first ?? "Select a recent meeting")
                        .font(.grotesk(12))
                        .lineLimit(1)
                }
                .menuStyle(.borderlessButton)
                .frame(width: 220)
                .padding(.horizontal, 10).padding(.vertical, 6)
                .background(RoundedRectangle(cornerRadius: 8).fill(Studio.field))
                .overlay(RoundedRectangle(cornerRadius: 8)
                    .stroke(Studio.border, lineWidth: 1))
                Text("Zoom account").font(.grotesk(13, .semibold))
                Text("Sign in once, then join meetings directly from CoreVideo.")
                    .font(.grotesk(12)).foregroundStyle(Studio.secondary)
                if model.zoomSignedIn {
                    HStack(spacing: 8) {
                        Text("Signed in").font(.grotesk(12, .medium))
                            .foregroundStyle(Studio.accent)
                        Button("Sign out") { model.signOutZoom() }
                            .buttonStyle(GhostButtonStyle())
                    }
                } else {
                    Button("Sign in with Zoom") { model.signInWithZoom() }
                        .buttonStyle(AccentButtonStyle())
                    if !model.zoomOAuthStatus.isEmpty {
                        Text(model.zoomOAuthStatus).font(.grotesk(11))
                            .foregroundStyle(Studio.secondary)
                    }
                }
                HStack(spacing: 8) {
                    if inMeeting {
                        Button("Leave meeting") { model.leaveZoom() }
                            .buttonStyle(GhostButtonStyle(tint: Studio.red))
                    } else {
                        Button("Join Zoom") { model.joinZoom() }
                            .buttonStyle(AccentButtonStyle())
                            .disabled(model.joinMeetingId.isEmpty)
                    }
                    Button("Open in Zoom app") { model.openInZoomApp() }
                        .buttonStyle(GhostButtonStyle())
                        .disabled(model.joinMeetingId.isEmpty)
                }
                if !model.zoomSignedIn {
                    Text("Sign in with Zoom to join as your account — "
                         + "guest joins wait in the waiting room.")
                        .font(.grotesk(12)).foregroundStyle(Studio.secondary)
                }
            }
            .padding(14)
            .background(RoundedRectangle(cornerRadius: 10).fill(Studio.panel))
            .overlay(RoundedRectangle(cornerRadius: 10).stroke(Studio.border, lineWidth: 1))

            VStack(alignment: .leading, spacing: 8) {
                Text("Zoom status").font(.grotesk(14, .semibold))
                Text(inMeeting ? "In the meeting — \(model.roster.count) in room"
                               : "Ready to join Zoom")
                    .font(.grotesk(12, .medium))
                    .foregroundStyle(inMeeting ? Studio.accent : Studio.textPrimary)
                    .padding(.horizontal, 12).padding(.vertical, 8)
                    .frame(maxWidth: .infinity, alignment: .leading)
                    .background(RoundedRectangle(cornerRadius: 8).fill(Studio.field))
                    .overlay(RoundedRectangle(cornerRadius: 8)
                        .stroke(Studio.accent.opacity(0.6), lineWidth: 1))
                Text(inMeeting
                     ? "Assign participants from the Studio room rail; "
                     + "press Capture to start raw media."
                     : "Enter a meeting link or ID above when you are ready to connect.")
                    .font(.grotesk(12)).foregroundStyle(Studio.secondary)
            }
            .padding(14)
            .background(RoundedRectangle(cornerRadius: 10).fill(Studio.panel))
            .overlay(RoundedRectangle(cornerRadius: 10).stroke(Studio.border, lineWidth: 1))
        }
        .frame(maxWidth: 520, alignment: .leading)
    }
}

// Sources tab — the inputs-1–10 patch bay (03-sources.png + routing-ux-spec
// tab 1): "Show inputs (multiview)" slot table with in-show checkbox, source
// menu, editable name, ISO checkbox and Unassign, over the Zoom-feed-health
// and capture-devices reference cards.
struct SourcesPane: View {
    @EnvironmentObject var model: AppModel

    var inShowCount: Int { model.slots.filter(\.inShow).count }
    var assignedCount: Int { model.slots.filter { $0.kind != "unassigned" }.count }

    var body: some View {
        VStack(alignment: .leading, spacing: 12) {
            VStack(alignment: .leading, spacing: 8) {
                Text("Show inputs (multiview)").font(.grotesk(15, .semibold))
                Text("Assign up to 10 sources below. Toggle In show to put them "
                     + "on the Studio multiview strip.")
                    .font(.grotesk(12)).foregroundStyle(Studio.secondary)
                Text("\(inShowCount) in show · \(assignedCount) assigned")
                    .font(.grotesk(12, .medium)).foregroundStyle(Studio.accent)
                HStack(spacing: 8) {
                    StepCard(number: "1", title: "Assign inputs",
                             detail: "Bind Zoom guests, media, or cameras to stable slots.",
                             active: true)
                    StepCard(number: "2", title: "Route outputs",
                             detail: "Use Routing for ISO video and program audio.",
                             active: false)
                    StepCard(number: "3", title: "Compose scenes",
                             detail: "Add Inputs to the 16:9 canvas and Take to Program.",
                             active: false)
                }
                ForEach(model.slots) { slot in
                    SlotRow(slot: slot)
                }
            }
            .padding(14)
            .background(RoundedRectangle(cornerRadius: 10).fill(Studio.panel))
            .overlay(RoundedRectangle(cornerRadius: 10).stroke(Studio.border, lineWidth: 1))

            HStack(alignment: .top, spacing: 12) {
                VStack(alignment: .leading, spacing: 6) {
                    Text("Zoom feed health").font(.grotesk(14, .semibold))
                    if model.roster.isEmpty {
                        Text("No Zoom feeds — join a meeting")
                            .font(.grotesk(12)).foregroundStyle(Studio.secondary)
                    }
                    ForEach(model.roster) { participant in
                        HStack(spacing: 6) {
                            Circle()
                                .fill(participant.hasVideo ? Studio.accent
                                                           : Studio.secondary.opacity(0.4))
                                .frame(width: 7, height: 7)
                            Text(participant.name).font(.grotesk(12)).lineLimit(1)
                            Text(participant.hasVideo ? "video" : "no video")
                                .font(.plexMono(10))
                                .foregroundStyle(Studio.secondary)
                        }
                    }
                }
                .padding(14)
                .frame(maxWidth: .infinity, alignment: .leading)
                .background(RoundedRectangle(cornerRadius: 10).fill(Studio.panel))
                .overlay(RoundedRectangle(cornerRadius: 10)
                    .stroke(Studio.border, lineWidth: 1))

                VStack(alignment: .leading, spacing: 6) {
                    Text("Capture devices").font(.grotesk(14, .semibold))
                    Text("\(model.captureDevices.filter { $0.connectionState == "connected" }.count)"
                         + " connected · \(model.captureDevices.count) detected")
                        .font(.grotesk(12)).foregroundStyle(Studio.secondary)
                    ForEach(model.captureDevices.prefix(8)) { device in
                        HStack(spacing: 6) {
                            Circle()
                                .fill(device.signalPresent ? Studio.accent
                                      : device.connectionState == "error"
                                          ? Studio.red : Studio.secondary.opacity(0.4))
                                .frame(width: 7, height: 7)
                            Text(device.name).font(.grotesk(12)).lineLimit(1)
                            Spacer()
                            Button(device.connectionState == "connected"
                                   ? "Disconnect" : "Connect") {
                                model.connectCaptureDevice(device)
                            }
                            .buttonStyle(.plain)
                            .font(.grotesk(11, .semibold))
                            .foregroundStyle(Studio.accent)
                        }
                        // A device that connects but never delivers a frame
                        // (idle virtual camera, camera held by another app,
                        // permission not granted) says so here — silence was
                        // indistinguishable from a working source.
                        if !device.warning.isEmpty {
                            Text(device.warning)
                                .font(.grotesk(10))
                                .foregroundStyle(device.connectionState == "error"
                                                 ? Studio.red : Studio.amber)
                                .fixedSize(horizontal: false, vertical: true)
                        } else if device.connectionState == "connected",
                                  !device.signalPresent {
                            Text("Connected — waiting for the first frame")
                                .font(.grotesk(10)).foregroundStyle(Studio.amber)
                        }
                    }
                    if model.captureDevices.count > 8 {
                        Text("+ \(model.captureDevices.count - 8) more")
                            .font(.grotesk(11)).foregroundStyle(Studio.textDim)
                    }
                }
                .padding(14)
                .frame(maxWidth: .infinity, alignment: .leading)
                .background(RoundedRectangle(cornerRadius: 10).fill(Studio.panel))
                .overlay(RoundedRectangle(cornerRadius: 10)
                    .stroke(Studio.border, lineWidth: 1))
            }
        }
        .frame(maxWidth: 900)
    }
}

struct StepCard: View {
    let number: String
    let title: String
    let detail: String
    let active: Bool

    var body: some View {
        VStack(alignment: .leading, spacing: 2) {
            HStack(spacing: 5) {
                Text(number).font(.grotesk(12, .semibold))
                Text(title).font(.grotesk(12, .semibold))
            }
            Text(detail).font(.grotesk(10)).foregroundStyle(Studio.secondary)
        }
        .padding(10)
        .frame(maxWidth: .infinity, alignment: .leading)
        .background(RoundedRectangle(cornerRadius: 8)
            .fill(active ? Studio.accent.opacity(0.08) : Studio.surface))
        .overlay(RoundedRectangle(cornerRadius: 8)
            .stroke(active ? Studio.accent.opacity(0.6) : Studio.border, lineWidth: 1))
    }
}

struct SlotRow: View {
    @EnvironmentObject var model: AppModel
    let slot: ShowInputSlot

    var assigned: Bool { slot.kind != "unassigned" }

    var sourceLabel: String {
        guard assigned else { return "Select a source" }
        if slot.offline { return "Source offline — choose or reconnect" }
        return (slot.kind == "zoom" ? "Zoom · " : "Camera · ") + slot.name
    }

    var body: some View {
        HStack(spacing: 8) {
            Text("\(slot.id)")
                .font(.plexMono(11, .medium))
                .foregroundStyle(Studio.secondary)
                .frame(width: 22, alignment: .trailing)
            Toggle("", isOn: Binding(
                get: { slot.inShow },
                set: { _ in model.toggleSlotInShow(slot.id) }))
                .toggleStyle(.checkbox)
                .labelsHidden()
                .disabled(!assigned)
            Menu {
                Section("Zoom participants") {
                    ForEach(model.roster) { participant in
                        Button(participant.name) {
                            model.assignSlot(slot.id, kind: "zoom",
                                             sourceId: participant.id,
                                             name: participant.name)
                        }
                    }
                }
                Section("Capture devices") {
                    ForEach(model.captureDevices) { device in
                        Button(device.name) {
                            model.assignSlot(slot.id, kind: "capture",
                                             sourceId: device.id, name: device.name)
                            if device.connectionState != "connected" {
                                model.connectCaptureDevice(device)
                            }
                        }
                    }
                }
            } label: {
                HStack {
                    Text(sourceLabel)
                        .font(.grotesk(12))
                        .foregroundStyle(assigned ? Studio.textPrimary : Studio.secondary)
                        .lineLimit(1)
                    if slot.offline {
                        Text("OFFLINE")
                            .font(.plexMono(9, .semibold))
                            .foregroundStyle(Studio.amber)
                    }
                    Spacer()
                }
            }
            .menuStyle(.borderlessButton)
            .frame(width: 300)
            .padding(.horizontal, 10).padding(.vertical, 6)
            .background(RoundedRectangle(cornerRadius: 8).fill(Studio.field))
            .overlay(RoundedRectangle(cornerRadius: 8)
                .stroke(slot.offline ? Studio.amber.opacity(0.6) : Studio.border,
                        lineWidth: 1))
            if assigned, slot.kind == "capture",
               let device = model.captureDevices.first(where: { $0.id == slot.sourceId }),
               device.connectionState == "connected", !device.signalPresent {
                Text("no signal").font(.plexMono(9, .semibold))
                    .foregroundStyle(Studio.amber)
            }
            if assigned {
                TextField("Name", text: Binding(
                    get: { slot.name },
                    set: { value in
                        if let index = model.slots.firstIndex(where: { $0.id == slot.id }) {
                            model.slots[index].name = value
                        }
                    }))
                    .textFieldStyle(StudioFieldStyle())
                    .frame(width: 150)
                Toggle("ISO", isOn: Binding(
                    get: { slot.iso },
                    set: { _ in model.toggleSlotIso(slot.id) }))
                    .toggleStyle(.checkbox)
                    .font(.grotesk(11))
                Button("Unassign") { model.unassignSlot(slot.id) }
                    .buttonStyle(GhostButtonStyle())
            }
            Spacer()
        }
    }
}

struct OverlaysPane: View {
    @EnvironmentObject var model: AppModel

    var onAir: Bool {
        model.lowerThirdPhase == "on-air" || model.lowerThirdPhase == "building-in"
    }

    var body: some View {
        VStack(alignment: .leading, spacing: 8) {
            Text("Overlays").font(.grotesk(14, .semibold))
            Text("LOWER THIRD")
                .font(.plexMono(10, .bold))
                .foregroundStyle(Studio.secondary)
            TextField("Name", text: $model.lowerThirdName)
                .textFieldStyle(StudioFieldStyle())
            TextField("Title", text: $model.lowerThirdTitle)
                .textFieldStyle(StudioFieldStyle())
            Picker("Position", selection: $model.lowerThirdPosition) {
                Text("Lower left").tag("lower-left")
                Text("Lower right").tag("lower-right")
                Text("Center").tag("lower-center")
            }
            .pickerStyle(.segmented)
            HStack {
                Button(onAir ? "Hide" : "Show") {
                    onAir ? model.hideLowerThird() : model.showLowerThird()
                }
                .disabled(model.lowerThirdName.isEmpty && !onAir)
                .tint(onAir ? .red : Studio.accent)
                Text(model.lowerThirdPhase)
                    .font(.plexMono(10))
                    .foregroundStyle(onAir ? Studio.accent : Studio.secondary)
                Spacer()
                Text("\(model.overlaysOnAir) on air")
                    .font(.plexMono(10))
                    .foregroundStyle(Studio.secondary)
            }
            Divider()
            Text("LOGO BUG")
                .font(.plexMono(10, .bold))
                .foregroundStyle(Studio.secondary)
            if let bug = model.logoBug {
                HStack {
                    Text(bug.name).font(.grotesk(12))
                    Spacer()
                    Button("Remove") { model.toggleLogoBug(bug) }.font(.grotesk(11))
                }
            } else {
                Text("Pick a still on the Media tab and tap “Bug” to key it top-right.")
                    .font(.grotesk(11)).foregroundStyle(Studio.secondary)
            }
        }
        .modifier(StudioPanel())
    }
}

struct MediaPane: View {
    @EnvironmentObject var model: AppModel

    var body: some View {
        VStack(alignment: .leading, spacing: 8) {
            HStack {
                Text("Media").font(.grotesk(14, .semibold))
                Spacer()
                Button("Import…") {
                    let panel = NSOpenPanel()
                    panel.allowsMultipleSelection = true
                    panel.canChooseDirectories = false
                    if panel.runModal() == .OK {
                        model.importMedia(urls: panel.urls)
                    }
                }
                Button("Refresh") { model.refreshMediaBin() }
            }
            Text(MediaBin.root)
                .font(.plexMono(8))
                .foregroundStyle(Studio.secondary.opacity(0.6))
                .lineLimit(1).truncationMode(.head)
            if model.mediaAssets.isEmpty {
                Text("Bin is empty — import stills, stingers or audio beds.")
                    .font(.grotesk(11)).foregroundStyle(Studio.secondary)
            }
            ForEach(model.mediaAssets) { asset in
                HStack {
                    VStack(alignment: .leading, spacing: 1) {
                        Text(asset.name).font(.grotesk(12)).lineLimit(1)
                        Text("\(asset.kind)"
                             + (asset.naturalWidth > 0
                                ? " · \(asset.naturalWidth)×\(asset.naturalHeight)"
                                : ""))
                            .font(.plexMono(9))
                            .foregroundStyle(Studio.secondary)
                    }
                    Spacer()
                    if asset.isStillImage {
                        Button(model.logoBug?.id == asset.id ? "Unbug" : "Bug") {
                            model.toggleLogoBug(asset)
                        }
                        .font(.grotesk(11))
                    }
                }
                .padding(.vertical, 2)
            }
        }
        .modifier(StudioPanel())
    }
}

struct AutomationPane: View {
    @EnvironmentObject var model: AppModel

    var body: some View {
        VStack(alignment: .leading, spacing: 8) {
            Text("Automation").font(.grotesk(14, .semibold))
            Toggle("Set & Forget (auto-direct)", isOn: Binding(
                get: { model.autoDirectEnabled },
                set: { model.setAutoDirect(enabled: $0) }))
            Toggle("Auto-take (off = queue on preview)", isOn: $model.autoTakeEnabled)
                .disabled(!model.autoDirectEnabled)
            HStack {
                Text("Confidence ≥ \(Int(model.autoConfidenceThreshold))")
                    .font(.grotesk(11))
                Slider(value: $model.autoConfidenceThreshold, in: 0...100, step: 5)
            }
            HStack {
                Text(String(format: "Hold %.0fs", model.autoHoldSeconds)).font(.grotesk(11))
                Slider(value: $model.autoHoldSeconds, in: 0...30, step: 1)
            }
            Text(model.autoStatus)
                .font(.plexMono(11))
                .foregroundStyle(model.autoDirectEnabled ? Studio.accent : Studio.secondary)
            Divider()
            Text("SCENE INTELLIGENCE")
                .font(.plexMono(10, .bold))
                .foregroundStyle(Studio.secondary)
            if model.autoSceneId.isEmpty {
                Text("Director idle — recommendations appear once the meeting has participants.")
                    .font(.grotesk(11)).foregroundStyle(Studio.secondary)
            } else {
                HStack {
                    Text(model.autoSceneId)
                        .font(.grotesk(13, .semibold))
                    Text("\(model.autoConfidence)%")
                        .font(.plexMono(11))
                        .foregroundStyle(model.autoConfidence >= Int(model.autoConfidenceThreshold)
                                         ? Studio.accent : Studio.secondary)
                    Spacer()
                    Text(model.autoRuleId)
                        .font(.plexMono(9))
                        .foregroundStyle(Studio.secondary)
                }
                Text(model.autoRationale)
                    .font(.grotesk(11)).foregroundStyle(Studio.secondary)
            }
            // AutomationPage.xaml parity: Magic Scene, dynamic lower thirds,
            // reset defaults, the scene-intelligence readouts, and the honest
            // disabled block. Lives in AutomationExtras.swift.
            AutomationExtrasSection()
        }
        .modifier(StudioPanel())
    }
}

// DiagnosePane now lives in Diagnostics.swift (the full DIAGNOSE surface plus
// the redacted support-bundle export).

// ── Meters ───────────────────────────────────────────────────────────────────

struct MeterBar: View {
    let level: Int  // 0..100

    var body: some View {
        GeometryReader { geometry in
            ZStack(alignment: .leading) {
                RoundedRectangle(cornerRadius: 2).fill(.black.opacity(0.5))
                RoundedRectangle(cornerRadius: 2)
                    .fill(level > 90 ? Color.red : level > 70 ? .yellow : Studio.accent)
                    .frame(width: geometry.size.width * CGFloat(min(100, max(0, level))) / 100.0)
            }
        }
        .frame(height: 10)
        .animation(.linear(duration: 0.08), value: level)
    }
}

struct VerticalMeter: View {
    let level: Int  // 0..100

    var body: some View {
        GeometryReader { geometry in
            ZStack(alignment: .bottom) {
                RoundedRectangle(cornerRadius: 2).fill(.black.opacity(0.5))
                RoundedRectangle(cornerRadius: 2)
                    .fill(level > 90 ? Color.red : level > 70 ? .yellow : Studio.accent)
                    .frame(height: geometry.size.height
                        * CGFloat(min(100, max(0, level))) / 100.0)
            }
        }
        .animation(.linear(duration: 0.08), value: level)
    }
}
