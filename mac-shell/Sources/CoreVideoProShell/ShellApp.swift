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

// Color tokens — exact hexes from docs/design-handoff-macos.md (dark only,
// opaque, status colors load-bearing and never adjusted).
enum Studio {
    static let background = Color(hex: 0x0A0B0C)
    static let panel = Color(hex: 0x101315)
    static let surface = Color(hex: 0x16191B)
    static let surfaceRaised = Color(hex: 0x1B1F22)
    static let field = Color(hex: 0x0E1112)
    static let border = Color.white.opacity(0.09)
    static let line2 = Color.white.opacity(0.05)
    static let textPrimary = Color(hex: 0xE9EDEF)
    static let secondary = Color(hex: 0x8B949B)
    static let textDim = Color(hex: 0x5C656B)
    static let accent = Color(hex: 0x22C86E)
    static let onAccent = Color(hex: 0x06170D)
    static let amber = Color(hex: 0xE8A41F)
    static let red = Color(hex: 0xE5433F)
    // The one documented inconsistency: sliders/toggles render the stock
    // accent on Windows too — match the reference, don't unify on green.
    static let blue = Color(red: 0.29, green: 0.62, blue: 0.85)
    // Back-compat aliases for pre-handoff call sites.
    static let stroke = border
    static let card = surface.opacity(0.6)
}

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
                .frame(minWidth: 1360, minHeight: 860)
                .font(.grotesk(12))
                .foregroundStyle(Studio.textPrimary)
                .background(Studio.background)
                .tint(Studio.accent)
                .preferredColorScheme(.dark)
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
            ForEach([StudioTab.zoom, .sources, .scenes], id: \.self) { TabPill(tab: $0) }
            DisabledPill("Routing")
            ForEach([StudioTab.overlays, .audio, .media, .automation, .diagnose],
                    id: \.self) { TabPill(tab: $0) }
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
            .font(.system(size: 9, weight: .semibold))
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

struct DisabledPill: View {
    let label: String
    init(_ label: String) { self.label = label }

    var body: some View {
        Text(label)
            .font(.system(size: 12))
            .foregroundStyle(Studio.secondary.opacity(0.4))
            .padding(.horizontal, 12)
            .padding(.vertical, 6)
            .background(RoundedRectangle(cornerRadius: 15).fill(Studio.card.opacity(0.5)))
            .help("Coming to the macOS shell")
    }
}

struct CapturePill: View {
    @EnvironmentObject var model: AppModel

    var on: Bool { model.rawMediaActive }

    var body: some View {
        HStack(spacing: 5) {
            Image(systemName: "power").font(.system(size: 10, weight: .bold))
            Text(on ? "Capture On" : "Capture Off").font(.system(size: 12, weight: .medium))
        }
        .foregroundStyle(on ? Studio.accent : Studio.secondary)
        .padding(.horizontal, 12)
        .padding(.vertical, 6)
        .background(Capsule().fill(Studio.card))
        .overlay(Capsule().stroke(on ? Studio.accent.opacity(0.7) : Studio.stroke,
                                  lineWidth: 1))
    }
}

struct LivePill: View {
    let label: String

    var body: some View {
        HStack(spacing: 5) {
            Circle().fill(Studio.accent).frame(width: 6, height: 6)
            Text(label).font(.system(size: 12, weight: .medium))
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
        case .launching: return .yellow
        case .exited, .failed: return .red
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
                .font(.system(size: 11))
                .foregroundStyle(ready ? Studio.accent : .yellow)
            Text("Show readiness").font(.system(size: 11, weight: .semibold))
                .foregroundStyle(ready ? Studio.accent : Studio.secondary)
            Text(message).font(.system(size: 11)).foregroundStyle(.white.opacity(0.85))
            Spacer()
            Text("\(model.assignedIds.count) assigned · "
                 + "\(model.roster.filter(\.hasVideo).count) Zoom feeds · "
                 + "\(model.captureDevices.filter { $0.connectionState == "connected" }.count)"
                 + " capture")
                .font(.system(size: 11)).foregroundStyle(Studio.secondary)
        }
        .padding(.horizontal, 12)
        .padding(.vertical, 5)
        .background(Color.white.opacity(0.02))
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

    var body: some View {
        VStack(spacing: 6) {
            HStack(spacing: 8) {
                Text("MULTIVIEWER").font(.grotesk(12, .semibold))
                MonoChip("1080p60")
                Text("\(model.assignedIds.count) live inputs")
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
                        Image(systemName: "rectangle.split.2x2").font(.system(size: 9))
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
            }
            ZStack {
                Rectangle().fill(Color.black)
                if model.multiviewSurfaceId == 0 {
                    VStack(spacing: 6) {
                        Text("Multiviewer")
                            .font(.grotesk(14, .semibold))
                            .foregroundStyle(Studio.secondary)
                        Text("Join Zoom or connect sources, assign inputs, "
                             + "pick a scene — preview and program composite here.")
                            .font(.grotesk(12)).foregroundStyle(Studio.textDim)
                    }
                } else {
                    SurfaceView(surfaceId: model.multiviewSurfaceId)
                }
            }
            .frame(maxWidth: .infinity, maxHeight: .infinity)
            .overlay(Rectangle().stroke(Studio.border, lineWidth: 1))
        }
        .modifier(StudioPanel())
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
        model.scenes.first { $0.id == model.previewSceneId }?.name ?? "—"
    }

    var body: some View {
        VStack(alignment: .leading, spacing: 8) {
            Text("Scenes").font(.system(size: 13, weight: .semibold))
            Text("\(model.scenes.count) scenes · 1920x1080 · 60 fps")
                .font(.system(size: 10)).foregroundStyle(Studio.secondary)
            HStack(spacing: 6) {
                SceneChip(label: "PGM \(programName)", color: Studio.amber)
                SceneChip(label: "PVW \(previewName)", color: Studio.accent)
            }
            ForEach(Array(model.scenes.enumerated()), id: \.element.id) { index, scene in
                SceneRow(index: index + 1, scene: scene)
            }
            Text("Tap a scene to queue on preview · Take swaps PVW and PGM")
                .font(.system(size: 9)).foregroundStyle(Studio.secondary.opacity(0.8))
            Button {
                model.selectedTab = .scenes
            } label: {
                HStack {
                    Image(systemName: "square.grid.2x2").font(.system(size: 10))
                    Text("Open Scene builder").font(.system(size: 11, weight: .medium))
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
                    .fill(onProgram ? Studio.amber : Color.white.opacity(0.08))
                    .frame(width: 24, height: 24)
                    .overlay(Text("\(index)")
                        .font(.system(size: 11, weight: .bold))
                        .foregroundStyle(onProgram ? .black : .white))
                VStack(alignment: .leading, spacing: 1) {
                    Text(scene.name).font(.system(size: 12, weight: .medium))
                        .foregroundStyle(.white)
                    Text(scene.layout).font(.system(size: 9))
                        .foregroundStyle(Studio.secondary)
                }
                Spacer()
                if onProgram {
                    SceneChip(label: "PGM", color: Studio.amber)
                } else if onPreview {
                    SceneChip(label: "PVW", color: Studio.accent)
                } else {
                    Text("IDLE").font(.system(size: 8, weight: .bold, design: .monospaced))
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
            .font(.system(size: 9, weight: .bold, design: .monospaced))
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
                .font(.system(size: 13, weight: .semibold))
            Text(model.meetingState == "in_meeting" ? "Main room" : "Not in a meeting")
                .font(.system(size: 10)).foregroundStyle(Studio.secondary)
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
                    .font(.system(size: 11, weight: .medium))
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
                .fill(Color.white.opacity(0.08))
                .frame(width: 30, height: 30)
                .overlay(Text(String(participant.name.prefix(1)).lowercased())
                    .font(.system(size: 13, weight: .semibold))
                    .foregroundStyle(.white))
            VStack(alignment: .leading, spacing: 1) {
                Text(participant.name).font(.system(size: 11, weight: .medium))
                    .lineLimit(1)
                HStack(spacing: 4) {
                    Text("Guest").font(.system(size: 9)).foregroundStyle(Studio.secondary)
                    if participant.talking {
                        Text("Talking").font(.system(size: 9))
                            .foregroundStyle(Studio.accent)
                    }
                    if participant.muted {
                        Image(systemName: "mic.slash.fill").font(.system(size: 8))
                            .foregroundStyle(Studio.secondary)
                    }
                }
            }
            Spacer()
            Button(assigned ? "−" : "+") { model.toggleAssigned(participant) }
                .buttonStyle(.plain)
                .font(.system(size: 13, weight: .bold))
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
                    case .overlays: OverlaysPane()
                    case .audio: AudioPane()
                    case .media: MediaPane()
                    case .automation: AutomationPane()
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
            guard let self, let layer = self.layer, self.surface != nil else { return }
            layer.setNeedsDisplay()
            layer.contents = layer.contents  // re-latch the surface contents
        }
    }

    @available(*, unavailable)
    required init?(coder: NSCoder) { fatalError() }

    deinit { refreshTimer?.invalidate() }

    func present(surfaceId: UInt32) {
        guard surfaceId != 0, surfaceId != currentSurfaceId else { return }
        guard let looked = IOSurfaceLookup(surfaceId) else { return }
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
                    Image(systemName: "sparkles").font(.system(size: 10))
                    VStack(alignment: .leading, spacing: 0) {
                        Text("Magic Scene").font(.system(size: 11, weight: .semibold))
                        Text("AI auto-direct").font(.system(size: 8))
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
                    Text("Set & Forget").font(.system(size: 10, weight: .medium))
                    Text("Automation \(model.autoDirectEnabled ? "On" : "Off")")
                        .font(.system(size: 8)).foregroundStyle(Studio.secondary)
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
                    .font(.system(size: 10, weight: .bold))
                Text(model.lowerThirdPhase == "hidden" ? "Ready" : model.lowerThirdPhase)
                    .font(.system(size: 9))
                    .foregroundStyle(model.lowerThirdPhase == "hidden"
                                     ? Studio.secondary : Studio.accent)
            }
            .padding(.horizontal, 8).padding(.vertical, 6)
            .background(RoundedRectangle(cornerRadius: 8).fill(Studio.card))
            Spacer()
            Text(outputsLabel).font(.system(size: 10)).foregroundStyle(Studio.secondary)
            // Take — amber, the director's commit button.
            Button {
                model.take()
            } label: {
                HStack(spacing: 4) {
                    Text("Take").font(.system(size: 12, weight: .bold))
                    Text("Cut").font(.system(size: 9))
                        .foregroundStyle(.black.opacity(0.6))
                }
                .foregroundStyle(.black)
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
                        .font(.system(size: 10))
                    VStack(alignment: .leading, spacing: 0) {
                        Text(recording ? "Stop Rec" : "Record")
                            .font(.system(size: 11, weight: .semibold))
                        Text(model.isoRecordingEnabled
                             ? "MP4 + \(model.resolvedIsoSourceIds().count) ISOs" : "MP4")
                            .font(.system(size: 8)).foregroundStyle(Studio.secondary)
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
                .font(.system(size: 10))
                .disabled(recording)
            StreamControl()
        }
        .padding(.horizontal, 12)
        .padding(.vertical, 7)
        .background(Studio.panel)
        .overlay(Rectangle().frame(height: 1).foregroundStyle(Studio.stroke),
                 alignment: .top)
    }
}

// ── Stream control (blue, transport) ─────────────────────────────────────────

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
                        .font(.system(size: 10))
                    VStack(alignment: .leading, spacing: 0) {
                        Text(live ? "Stop Stream" : "Stream")
                            .font(.system(size: 11, weight: .semibold))
                        Text(live ? (model.streamDetail.isEmpty ? "starting…"
                                     : model.streamDetail)
                             : "RTMP 6 Mbps")
                            .font(.system(size: 8)).foregroundStyle(
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
                Image(systemName: "gearshape").font(.system(size: 10))
            }
            .buttonStyle(.plain)
            .foregroundStyle(Studio.secondary)
        }
        .popover(isPresented: $showSettings) {
            VStack(alignment: .leading, spacing: 8) {
                Text("Stream settings").font(.headline)
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
                    .font(.caption2).foregroundStyle(Studio.secondary)
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
                Text("OUTPUTS").font(.system(size: 9, weight: .bold, design: .monospaced))
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
                Text("Monitor").font(.system(size: 11, weight: .medium))
            }
            .toggleStyle(.button)
            .tint(Studio.blue)
            VStack(alignment: .leading, spacing: 1) {
                HStack(spacing: 6) {
                    Text("MASTER")
                        .font(.system(size: 9, weight: .bold, design: .monospaced))
                        .foregroundStyle(Studio.secondary)
                    Text(lufsLabel).font(.system(size: 11, weight: .semibold,
                                                 design: .monospaced))
                }
                MeterBar(level: model.masterLevel).frame(width: 170)
            }
            if !peakLabel.isEmpty {
                Text(peakLabel).font(.system(size: 9, design: .monospaced))
                    .foregroundStyle(Studio.secondary)
            }
            Text(model.clockText).font(.system(size: 10, design: .monospaced))
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
            Text(title).font(.system(size: 8, weight: .bold, design: .monospaced))
                .foregroundStyle(Studio.secondary)
            Text(value).font(.system(size: 10, weight: .semibold))
            if !detail.isEmpty {
                Text(detail).font(.system(size: 8)).foregroundStyle(Studio.secondary)
            }
        }
    }
}

// ── Panes (tab pages) ────────────────────────────────────────────────────────

struct ZoomPane: View {
    @EnvironmentObject var model: AppModel

    var body: some View {
        VStack(alignment: .leading, spacing: 8) {
            Text("Zoom").font(.headline)
            TextField("Meeting ID or URL", text: $model.joinMeetingId)
                .textFieldStyle(StudioFieldStyle())
            HStack {
                TextField("Passcode", text: $model.joinPasscode)
                    .textFieldStyle(StudioFieldStyle())
                Button("Join") { model.joinZoom() }
                    .disabled(model.joinMeetingId.isEmpty)
                Button("Leave") { model.leaveZoom() }
            }
            HStack {
                Text("meeting: \(model.meetingState)")
                    .font(.system(.caption2, design: .monospaced))
                    .foregroundStyle(Studio.secondary)
                Spacer()
                Text(model.rawMediaActive ? "raw media LIVE" : "raw media off")
                    .font(.system(.caption2, design: .monospaced))
                    .foregroundStyle(model.rawMediaActive ? Studio.accent : Studio.secondary)
            }
            Divider()
            Text("PARTICIPANTS")
                .font(.system(size: 10, weight: .bold, design: .monospaced))
                .foregroundStyle(Studio.secondary)
            if model.roster.isEmpty {
                Text("No participants yet.").font(.caption).foregroundStyle(Studio.secondary)
            }
            ForEach(model.roster) { participant in
                HStack {
                    Circle()
                        .fill(participant.talking ? Studio.accent
                                                  : Studio.secondary.opacity(0.4))
                        .frame(width: 8, height: 8)
                    Text(participant.name).font(.system(size: 12)).lineLimit(1)
                    if participant.hasVideo {
                        Image(systemName: "video.fill").font(.system(size: 9))
                            .foregroundStyle(Studio.secondary)
                    }
                    if participant.muted {
                        Image(systemName: "mic.slash.fill").font(.system(size: 9))
                            .foregroundStyle(Studio.secondary)
                    }
                    Spacer()
                    if model.isoRecordingEnabled,
                       model.assignedIds.contains(participant.id) {
                        Toggle("ISO", isOn: Binding(
                            get: {
                                model.isoSelectedSourceIds
                                    .contains("zoom:" + participant.id)
                            },
                            set: { _ in
                                model.toggleIsoSource("zoom:" + participant.id)
                            }))
                            .toggleStyle(.checkbox)
                            .font(.system(size: 10))
                    }
                    Button(model.assignedIds.contains(participant.id)
                           ? "Unassign" : "Assign") {
                        model.toggleAssigned(participant)
                    }
                    .font(.caption)
                }
                .padding(.vertical, 2)
            }
        }
        .modifier(StudioPanel())
    }
}

struct SourcesPane: View {
    @EnvironmentObject var model: AppModel

    var body: some View {
        VStack(alignment: .leading, spacing: 8) {
            Text("Sources").font(.headline)
            Text("Cameras, screens and windows (AVFoundation / ScreenCaptureKit)")
                .font(.caption2).foregroundStyle(Studio.secondary)
            if model.captureDevices.isEmpty {
                Text("No capture sources detected yet.")
                    .font(.caption).foregroundStyle(Studio.secondary)
            }
            ForEach(model.captureDevices) { device in
                VStack(alignment: .leading, spacing: 2) {
                    HStack {
                        Circle()
                            .fill(device.signalPresent ? Studio.accent
                                  : device.connectionState == "error"
                                      ? Color.red : Studio.secondary.opacity(0.4))
                            .frame(width: 8, height: 8)
                        Text(device.name).font(.system(size: 12)).lineLimit(1)
                        Spacer()
                        Button(device.connectionState == "connected"
                               ? "Disconnect" : "Connect") {
                            model.connectCaptureDevice(device)
                        }
                        .font(.caption)
                    }
                    Text("\(device.kind) · \(device.vendor) · \(device.connectionState)")
                        .font(.system(size: 9, design: .monospaced))
                        .foregroundStyle(Studio.secondary)
                    if !device.warning.isEmpty {
                        Text(device.warning).font(.system(size: 9))
                            .foregroundStyle(.orange)
                    }
                }
                .padding(.vertical, 2)
            }
        }
        .modifier(StudioPanel())
    }
}

struct ScenesPane: View {
    @EnvironmentObject var model: AppModel

    var body: some View {
        VStack(alignment: .leading, spacing: 8) {
            Text("Scenes").font(.headline)
            Text("Scene slots fill from assigned participants in roster order; "
                 + "empty slots follow the active speaker.")
                .font(.caption2).foregroundStyle(Studio.secondary)
            let assigned = model.roster.filter { model.assignedIds.contains($0.id) }
            Text("ASSIGNED (\(assigned.count))")
                .font(.system(size: 10, weight: .bold, design: .monospaced))
                .foregroundStyle(Studio.secondary)
            if assigned.isEmpty {
                Text("Assign participants on the Zoom tab to fill scene slots.")
                    .font(.caption).foregroundStyle(Studio.secondary)
            }
            ForEach(Array(assigned.enumerated()), id: \.element.id) { index, participant in
                HStack {
                    Text("slot \(index + 1)")
                        .font(.system(size: 10, design: .monospaced))
                        .foregroundStyle(Studio.secondary)
                    Text(participant.name).font(.system(size: 12)).lineLimit(1)
                    Spacer()
                }
            }
            Divider()
            HStack {
                Text("PGM: \(model.programSceneId.isEmpty ? "—" : model.programSceneId)")
                    .font(.system(.caption2, design: .monospaced))
                    .foregroundStyle(Studio.amber)
                Text("PVW: \(model.previewSceneId.isEmpty ? "—" : model.previewSceneId)")
                    .font(.system(.caption2, design: .monospaced))
                    .foregroundStyle(Studio.accent)
                Spacer()
                Button("Take") { model.take() }
                    .disabled(model.previewSceneId.isEmpty
                              || model.previewSceneId == model.programSceneId)
            }
        }
        .modifier(StudioPanel())
    }
}

struct OverlaysPane: View {
    @EnvironmentObject var model: AppModel

    var onAir: Bool {
        model.lowerThirdPhase == "on-air" || model.lowerThirdPhase == "building-in"
    }

    var body: some View {
        VStack(alignment: .leading, spacing: 8) {
            Text("Overlays").font(.headline)
            Text("LOWER THIRD")
                .font(.system(size: 10, weight: .bold, design: .monospaced))
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
                    .font(.system(.caption2, design: .monospaced))
                    .foregroundStyle(onAir ? Studio.accent : Studio.secondary)
                Spacer()
                Text("\(model.overlaysOnAir) on air")
                    .font(.system(.caption2, design: .monospaced))
                    .foregroundStyle(Studio.secondary)
            }
            Divider()
            Text("LOGO BUG")
                .font(.system(size: 10, weight: .bold, design: .monospaced))
                .foregroundStyle(Studio.secondary)
            if let bug = model.logoBug {
                HStack {
                    Text(bug.name).font(.system(size: 12))
                    Spacer()
                    Button("Remove") { model.toggleLogoBug(bug) }.font(.caption)
                }
            } else {
                Text("Pick a still on the Media tab and tap “Bug” to key it top-right.")
                    .font(.caption).foregroundStyle(Studio.secondary)
            }
        }
        .modifier(StudioPanel())
    }
}

struct AudioPane: View {
    @EnvironmentObject var model: AppModel

    var body: some View {
        VStack(alignment: .leading, spacing: 8) {
            Text("Audio").font(.headline)
            if model.monitorFeedbackRisk {
                Text("⚠ Monitor output matches the loopback device — feedback risk")
                    .font(.caption).foregroundStyle(.orange)
            }
            if model.strips.isEmpty {
                Text("No mix channels yet — audio strips appear once sources carry PCM.")
                    .font(.caption).foregroundStyle(Studio.secondary)
            }
            ScrollView(.horizontal) {
                HStack(alignment: .top, spacing: 8) {
                    ForEach(model.strips) { strip in
                        ChannelStrip(strip: strip)
                    }
                    MasterRail()
                }
            }
            Divider()
            Toggle("Monitor (system default output)", isOn: Binding(
                get: { model.monitorEnabled },
                set: { model.setMonitor(enabled: $0, volume: model.monitorVolume) }))
            HStack {
                Text("Vol").font(.caption)
                Slider(value: Binding(
                    get: { model.monitorVolume },
                    set: { model.setMonitor(enabled: model.monitorEnabled, volume: $0) }),
                    in: 0...1)
                if !model.monitorStatus.isEmpty {
                    Text(model.monitorStatus)
                        .font(.system(size: 9, design: .monospaced))
                        .foregroundStyle(Studio.secondary)
                }
            }
            .disabled(!model.monitorEnabled)
        }
        .modifier(StudioPanel())
    }
}

struct ChannelStrip: View {
    @EnvironmentObject var model: AppModel
    let strip: AudioStrip

    var body: some View {
        VStack(spacing: 6) {
            Text(strip.id == "zoom-mix" ? "ZOOM MIX" : strip.id)
                .font(.system(size: 9, weight: .semibold, design: .monospaced))
                .lineLimit(1).truncationMode(.middle)
                .frame(width: 84)
            HStack(spacing: 4) {
                Button("M") {
                    model.editStrip(strip.id) { $0.muted.toggle() }
                }
                .buttonStyle(.plain)
                .font(.system(size: 10, weight: .bold))
                .frame(width: 22, height: 18)
                .background(RoundedRectangle(cornerRadius: 3)
                    .fill(strip.muted ? Studio.red : Color.white.opacity(0.06)))
                Button("S") {
                    model.editStrip(strip.id) { $0.solo.toggle() }
                }
                .buttonStyle(.plain)
                .font(.system(size: 10, weight: .bold))
                .frame(width: 22, height: 18)
                .background(RoundedRectangle(cornerRadius: 3)
                    .fill(strip.solo ? Color(red: 0.78, green: 0.60, blue: 0.18)
                                     : Color.white.opacity(0.06)))
            }
            HStack(spacing: 4) {
                // Vertical fader (-24…+24 dB) beside the live meter.
                Slider(value: Binding(
                    get: { strip.manualGainDb },
                    set: { value in model.editStrip(strip.id) { $0.manualGainDb = value } }),
                    in: -24...24)
                    .frame(width: 110)
                    .rotationEffect(.degrees(-90))
                    .frame(width: 28, height: 110)
                VerticalMeter(level: strip.muted ? 0 : strip.outputLevel)
                    .frame(width: 10, height: 110)
            }
            Text(String(format: "%+.1f dB", strip.manualGainDb))
                .font(.system(size: 9, design: .monospaced))
                .foregroundStyle(Studio.secondary)
            Text(strip.status)
                .font(.system(size: 8, design: .monospaced))
                .foregroundStyle(Studio.secondary.opacity(0.7))
        }
        .padding(6)
        .background(RoundedRectangle(cornerRadius: 6).fill(Color.white.opacity(0.03)))
    }
}

struct MasterRail: View {
    @EnvironmentObject var model: AppModel

    var lufsLabel: String {
        model.shortTermLufs <= -119 ? "—" : String(format: "%.1f", model.shortTermLufs)
    }

    var truePeakLabel: String {
        model.truePeakDbfs <= -119 ? "—" : String(format: "TP %.1f dBFS", model.truePeakDbfs)
    }

    var body: some View {
        VStack(spacing: 6) {
            Text("MASTER")
                .font(.system(size: 9, weight: .bold, design: .monospaced))
                .foregroundStyle(Studio.secondary)
            Text(lufsLabel)
                .font(.system(size: 20, weight: .semibold, design: .monospaced))
            Text("LUFS · \(truePeakLabel)")
                .font(.system(size: 8, design: .monospaced))
                .foregroundStyle(Studio.secondary)
            VerticalMeter(level: model.masterLevel)
                .frame(width: 14, height: 96)
            HStack(spacing: 4) {
                Circle()
                    .fill(model.limiterActive ? Color.red : Studio.secondary.opacity(0.3))
                    .frame(width: 7, height: 7)
                Text("LIM").font(.system(size: 8, design: .monospaced))
                    .foregroundStyle(Studio.secondary)
            }
        }
        .padding(6)
        .background(RoundedRectangle(cornerRadius: 6).fill(Color.white.opacity(0.05)))
    }
}

struct MediaPane: View {
    @EnvironmentObject var model: AppModel

    var body: some View {
        VStack(alignment: .leading, spacing: 8) {
            HStack {
                Text("Media").font(.headline)
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
                .font(.system(size: 8, design: .monospaced))
                .foregroundStyle(Studio.secondary.opacity(0.6))
                .lineLimit(1).truncationMode(.head)
            if model.mediaAssets.isEmpty {
                Text("Bin is empty — import stills, stingers or audio beds.")
                    .font(.caption).foregroundStyle(Studio.secondary)
            }
            ForEach(model.mediaAssets) { asset in
                HStack {
                    VStack(alignment: .leading, spacing: 1) {
                        Text(asset.name).font(.system(size: 12)).lineLimit(1)
                        Text("\(asset.kind)"
                             + (asset.naturalWidth > 0
                                ? " · \(asset.naturalWidth)×\(asset.naturalHeight)"
                                : ""))
                            .font(.system(size: 9, design: .monospaced))
                            .foregroundStyle(Studio.secondary)
                    }
                    Spacer()
                    if asset.isStillImage {
                        Button(model.logoBug?.id == asset.id ? "Unbug" : "Bug") {
                            model.toggleLogoBug(asset)
                        }
                        .font(.caption)
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
            Text("Automation").font(.headline)
            Toggle("Set & Forget (auto-direct)", isOn: Binding(
                get: { model.autoDirectEnabled },
                set: { model.setAutoDirect(enabled: $0) }))
            Toggle("Auto-take (off = queue on preview)", isOn: $model.autoTakeEnabled)
                .disabled(!model.autoDirectEnabled)
            HStack {
                Text("Confidence ≥ \(Int(model.autoConfidenceThreshold))")
                    .font(.caption)
                Slider(value: $model.autoConfidenceThreshold, in: 0...100, step: 5)
            }
            HStack {
                Text(String(format: "Hold %.0fs", model.autoHoldSeconds)).font(.caption)
                Slider(value: $model.autoHoldSeconds, in: 0...30, step: 1)
            }
            Text(model.autoStatus)
                .font(.system(.caption, design: .monospaced))
                .foregroundStyle(model.autoDirectEnabled ? Studio.accent : Studio.secondary)
            Divider()
            Text("SCENE INTELLIGENCE")
                .font(.system(size: 10, weight: .bold, design: .monospaced))
                .foregroundStyle(Studio.secondary)
            if model.autoSceneId.isEmpty {
                Text("Director idle — recommendations appear once the meeting has participants.")
                    .font(.caption).foregroundStyle(Studio.secondary)
            } else {
                HStack {
                    Text(model.autoSceneId)
                        .font(.system(size: 13, weight: .semibold))
                    Text("\(model.autoConfidence)%")
                        .font(.system(.caption, design: .monospaced))
                        .foregroundStyle(model.autoConfidence >= Int(model.autoConfidenceThreshold)
                                         ? Studio.accent : Studio.secondary)
                    Spacer()
                    Text(model.autoRuleId)
                        .font(.system(size: 9, design: .monospaced))
                        .foregroundStyle(Studio.secondary)
                }
                Text(model.autoRationale)
                    .font(.caption).foregroundStyle(Studio.secondary)
            }
        }
        .modifier(StudioPanel())
    }
}

struct DiagnosePane: View {
    @EnvironmentObject var model: AppModel

    var body: some View {
        VStack(alignment: .leading, spacing: 6) {
            Text("Diagnose").font(.headline)
            HStack {
                ConnectionDot()
                Text(model.statusDetail.isEmpty ? "core healthy" : model.statusDetail)
                    .font(.system(.caption2, design: .monospaced))
                    .foregroundStyle(Studio.secondary)
                    .lineLimit(2)
            }
            if !model.recordingArtifactPath.isEmpty {
                Text("last artifact: \(model.recordingArtifactPath)")
                    .font(.system(size: 9, design: .monospaced))
                    .foregroundStyle(Studio.secondary)
                    .lineLimit(1).truncationMode(.head)
                    .textSelection(.enabled)
            }
            Divider()
            Text("WARNINGS")
                .font(.system(size: 10, weight: .bold, design: .monospaced))
                .foregroundStyle(Studio.secondary)
            if model.warnings.isEmpty {
                Text("None.").font(.caption).foregroundStyle(Studio.secondary)
            }
            ForEach(Array(model.warnings.enumerated()), id: \.offset) { _, warning in
                Text(warning)
                    .font(.system(size: 10, design: .monospaced))
                    .foregroundStyle(.orange)
                    .textSelection(.enabled)
            }
        }
        .frame(maxWidth: .infinity, alignment: .leading)
        .modifier(StudioPanel())
    }
}

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
