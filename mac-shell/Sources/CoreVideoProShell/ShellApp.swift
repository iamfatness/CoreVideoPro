// CoreVideo Pro — macOS shell. The workspace mirrors the WinUI product's
// geometry (Views/StudioWorkspace.xaml): header with the nav tab rail +
// CAPTURE/ZOOM/SHOW chips and clock, the PGM/PVW director row, the
// MULTIVIEWER row, the transport bar (Take/Cut/Fade + OUTPUTS + MASTER
// meter), and the right-rail tab panes. Palette from App.xaml: #0A0B0C
// chrome, #101315 panels, #22C86E studio green, #8B949B secondary.

import AppKit
import IOSurface
import SwiftUI

enum Studio {
    static let background = Color(red: 0x0A / 255.0, green: 0x0B / 255.0, blue: 0x0C / 255.0)
    static let panel = Color(red: 0x10 / 255.0, green: 0x13 / 255.0, blue: 0x15 / 255.0)
    static let accent = Color(red: 0x22 / 255.0, green: 0xC8 / 255.0, blue: 0x6E / 255.0)
    static let secondary = Color(red: 0x8B / 255.0, green: 0x94 / 255.0, blue: 0x9B / 255.0)
    static let stroke = Color.white.opacity(0x17 / 255.0)
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
        setvbuf(stdout, nil, _IOLBF, 0)  // line-buffer logs when piped
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
                .frame(minWidth: 1280, minHeight: 800)
                .background(Studio.background)
                .tint(Studio.accent)
                .preferredColorScheme(.dark)
        }
    }
}

struct RootView: View {
    @EnvironmentObject var model: AppModel

    var body: some View {
        VStack(spacing: 8) {
            HeaderBar()
            HStack(alignment: .top, spacing: 8) {
                SceneRail()
                    .frame(width: 200)
                VStack(spacing: 8) {
                    DirectorRow()
                    MultiviewRow()
                    TransportBar()
                }
                RightRail()
                    .frame(width: 360)
            }
            .padding([.horizontal, .bottom], 8)
        }
        .background(Studio.background)
    }
}

// ── Header: brand + nav tab rail + status chips + clock ──────────────────────

// The WinUI nav order. Tabs without a mac pane yet render disabled — same
// rail, honest about what's ported.
private let navRail: [(title: String, tab: StudioTab?)] = [
    ("Zoom", .zoom), ("Sources", .sources), ("Scenes", .scenes), ("Routing", nil),
    ("Overlays", .overlays), ("Audio", .audio), ("Media", .media), ("Automation", nil),
    ("Diagnose", .diagnose),
]

struct HeaderBar: View {
    @EnvironmentObject var model: AppModel

    var body: some View {
        HStack(spacing: 14) {
            VStack(alignment: .leading, spacing: 0) {
                Text("CoreVideo Pro").font(.system(size: 15, weight: .semibold))
                Text("Live production").font(.caption2).foregroundStyle(Studio.secondary)
            }
            Divider().frame(height: 26)
            ForEach(navRail, id: \.title) { entry in
                if let tab = entry.tab {
                    Button(entry.title) { model.selectedTab = tab }
                        .buttonStyle(.plain)
                        .font(.system(size: 12,
                                      weight: model.selectedTab == tab ? .semibold : .regular))
                        .foregroundStyle(model.selectedTab == tab ? Studio.accent
                                                                  : Studio.secondary)
                } else {
                    Text(entry.title).font(.system(size: 12))
                        .foregroundStyle(Studio.secondary.opacity(0.35))
                        .help("Coming to the macOS shell")
                }
            }
            Spacer()
            StatusChip(label: "CAPTURE", active: model.rawMediaActive)
            StatusChip(label: "ZOOM", active: model.meetingState == "in_meeting")
            StatusChip(label: "SHOW", active: model.recordingStatus == "recording")
            Text(model.clockText)
                .font(.system(.caption, design: .monospaced))
                .foregroundStyle(Studio.secondary)
            ConnectionDot()
        }
        .padding(.horizontal, 12)
        .padding(.vertical, 8)
        .background(Studio.panel)
        .overlay(Rectangle().frame(height: 1).foregroundStyle(Studio.stroke),
                 alignment: .bottom)
    }
}

struct StatusChip: View {
    let label: String
    let active: Bool

    var body: some View {
        Text(label)
            .font(.system(size: 10, weight: .bold, design: .monospaced))
            .padding(.horizontal, 8)
            .padding(.vertical, 3)
            .background(RoundedRectangle(cornerRadius: 4)
                .fill(active ? Studio.accent.opacity(0.22) : Color.white.opacity(0.05)))
            .foregroundStyle(active ? Studio.accent : Studio.secondary)
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
        Circle().fill(color).frame(width: 10, height: 10).help(help)
    }
}

// ── Scene rail (mirrors the WinUI Studio-tab left rail) ──────────────────────

struct SceneRail: View {
    @EnvironmentObject var model: AppModel

    var body: some View {
        VStack(alignment: .leading, spacing: 8) {
            Text("Scenes").font(.headline)
            ForEach(model.scenes) { scene in
                Button {
                    model.selectPreviewScene(scene.id)
                } label: {
                    HStack {
                        VStack(alignment: .leading, spacing: 1) {
                            Text(scene.name).font(.system(size: 12, weight: .medium))
                            Text(scene.layout)
                                .font(.system(size: 9, design: .monospaced))
                                .foregroundStyle(Studio.secondary)
                        }
                        Spacer()
                        if scene.id == model.programSceneId {
                            SceneChip(label: "PGM", color: Color(red: 0.94, green: 0.66, blue: 0.36))
                        } else if scene.id == model.previewSceneId {
                            SceneChip(label: "PVW", color: Studio.accent)
                        }
                    }
                    .padding(8)
                    .background(RoundedRectangle(cornerRadius: 6)
                        .fill(scene.id == model.previewSceneId
                              ? Studio.accent.opacity(0.10) : Color.white.opacity(0.03)))
                    .overlay(RoundedRectangle(cornerRadius: 6).stroke(
                        scene.id == model.programSceneId
                            ? Color(red: 0.94, green: 0.66, blue: 0.36).opacity(0.6)
                            : scene.id == model.previewSceneId
                                ? Studio.accent.opacity(0.5) : Studio.stroke,
                        lineWidth: 1))
                }
                .buttonStyle(.plain)
            }
            Text("Tap arms PVW — Take puts it on air")
                .font(.system(size: 9))
                .foregroundStyle(Studio.secondary.opacity(0.7))
            Spacer()
        }
        .modifier(StudioPanel())
    }
}

struct SceneChip: View {
    let label: String
    let color: Color

    var body: some View {
        Text(label)
            .font(.system(size: 9, weight: .bold, design: .monospaced))
            .padding(.horizontal, 5).padding(.vertical, 2)
            .background(RoundedRectangle(cornerRadius: 3).fill(color.opacity(0.22)))
            .foregroundStyle(color)
    }
}

// ── Director row: PGM + PVW monitors ─────────────────────────────────────────

struct DirectorRow: View {
    @EnvironmentObject var model: AppModel

    var body: some View {
        HStack(spacing: 8) {
            MonitorTile(title: "PGM", tally: .red,
                        surfaceId: model.programSurfaceId, emptyHint: "Program output")
            MonitorTile(title: "PVW", tally: Studio.accent,
                        surfaceId: model.previewSurfaceId, emptyHint: "No preview scene")
        }
    }
}

struct MonitorTile: View {
    let title: String
    let tally: Color
    let surfaceId: UInt32
    let emptyHint: String

    var body: some View {
        VStack(spacing: 4) {
            HStack {
                Text(title)
                    .font(.system(size: 11, weight: .bold, design: .monospaced))
                    .foregroundStyle(tally)
                Spacer()
            }
            ZStack {
                Rectangle().fill(Color.black)
                if surfaceId == 0 {
                    Text(emptyHint).font(.caption).foregroundStyle(Studio.secondary)
                } else {
                    SurfaceView(surfaceId: surfaceId)
                }
            }
            .aspectRatio(16.0 / 9.0, contentMode: .fit)
            .overlay(Rectangle().stroke(
                surfaceId == 0 ? Studio.stroke : tally.opacity(0.6), lineWidth: 1))
        }
        .modifier(StudioPanel())
        .frame(maxWidth: .infinity)
    }
}

// ── Multiview row ────────────────────────────────────────────────────────────

struct MultiviewRow: View {
    @EnvironmentObject var model: AppModel

    var body: some View {
        VStack(spacing: 4) {
            HStack {
                Text("MULTIVIEWER")
                    .font(.system(size: 11, weight: .bold, design: .monospaced))
                    .foregroundStyle(Studio.secondary)
                Spacer()
                Text("\(model.assignedIds.count) assigned")
                    .font(.caption2).foregroundStyle(Studio.secondary.opacity(0.6))
            }
            ZStack {
                Rectangle().fill(Color.black)
                if model.multiviewSurfaceId == 0 {
                    Text("Assign participants or connect sources to populate the multiviewer")
                        .font(.caption).foregroundStyle(Studio.secondary)
                } else {
                    SurfaceView(surfaceId: model.multiviewSurfaceId)
                }
            }
            .frame(minHeight: 160, maxHeight: 220)
            .overlay(Rectangle().stroke(Studio.stroke, lineWidth: 1))
        }
        .modifier(StudioPanel())
    }
}

// ── IOSurface presenter (PGM / PVW / multiview share it) ─────────────────────

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

    var body: some View {
        HStack(spacing: 10) {
            // Take/Cut both hard-cut (the core has no transition engine —
            // the Windows fade/dip/wipe labels are cosmetic too).
            Button("Take") { model.take() }
                .fontWeight(.semibold)
                .disabled(model.previewSceneId.isEmpty
                          || model.previewSceneId == model.programSceneId)
            Button("Cut") { model.take() }
                .disabled(model.previewSceneId.isEmpty
                          || model.previewSceneId == model.programSceneId)
            Divider().frame(height: 22)
            Text("OUTPUTS")
                .font(.system(size: 10, weight: .bold, design: .monospaced))
                .foregroundStyle(Studio.secondary)
            Button(recording ? "STOP" : "RECORD") { model.toggleRecording() }
                .tint(.red)
                .fontWeight(.semibold)
            if recording {
                StatusChip(label: "LIVE", active: true)
            }
            Text(model.recordingStatus)
                .font(.system(.caption, design: .monospaced))
                .foregroundStyle(recording ? .red : Studio.secondary)
            if !model.recordingArtifactPath.isEmpty {
                Text((model.recordingArtifactPath as NSString).lastPathComponent)
                    .font(.caption2).foregroundStyle(Studio.secondary)
                    .lineLimit(1).truncationMode(.head)
            }
            Spacer()
            Button("Stop Capture") { model.stopCapture() }
                .disabled(!model.rawMediaActive)
            Text("MASTER")
                .font(.system(size: 10, weight: .bold, design: .monospaced))
                .foregroundStyle(Studio.secondary)
            MeterBar(level: model.masterLevel).frame(width: 140)
        }
        .modifier(StudioPanel())
    }
}

// ── Right rail: tab panes ────────────────────────────────────────────────────

struct RightRail: View {
    @EnvironmentObject var model: AppModel

    var body: some View {
        VStack(spacing: 8) {
            switch model.selectedTab {
            case .zoom: ZoomPane()
            case .sources: SourcesPane()
            case .scenes: ScenesPane()
            case .overlays: OverlaysPane()
            case .audio: AudioPane()
            case .media: MediaPane()
            case .diagnose: DiagnosePane()
            }
            Spacer(minLength: 0)
        }
    }
}

struct ZoomPane: View {
    @EnvironmentObject var model: AppModel

    var body: some View {
        VStack(alignment: .leading, spacing: 8) {
            Text("Zoom").font(.headline)
            TextField("Meeting ID or URL", text: $model.joinMeetingId)
                .textFieldStyle(.roundedBorder)
            HStack {
                TextField("Passcode", text: $model.joinPasscode)
                    .textFieldStyle(.roundedBorder)
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
            ScrollView {
                VStack(spacing: 4) {
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
                            Button(model.assignedIds.contains(participant.id)
                                   ? "Unassign" : "Assign") {
                                model.toggleAssigned(participant)
                            }
                            .font(.caption)
                        }
                        .padding(.vertical, 2)
                    }
                }
            }
            .frame(maxHeight: 260)
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
            ScrollView {
                VStack(spacing: 6) {
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
            }
            .frame(maxHeight: 420)
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
                    .foregroundStyle(Color(red: 0.94, green: 0.66, blue: 0.36))
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
                    .fill(strip.muted ? Color(red: 0.78, green: 0.21, blue: 0.18)
                                      : Color.white.opacity(0.06)))
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
            Divider()
            Text("WARNINGS")
                .font(.system(size: 10, weight: .bold, design: .monospaced))
                .foregroundStyle(Studio.secondary)
            if model.warnings.isEmpty {
                Text("None.").font(.caption).foregroundStyle(Studio.secondary)
            }
            ScrollView {
                VStack(alignment: .leading, spacing: 3) {
                    ForEach(Array(model.warnings.enumerated()), id: \.offset) { _, warning in
                        Text(warning)
                            .font(.system(size: 10, design: .monospaced))
                            .foregroundStyle(.orange)
                            .textSelection(.enabled)
                    }
                }
            }
            .frame(maxHeight: 360)
        }
        .frame(maxWidth: .infinity, alignment: .leading)
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
                .textFieldStyle(.roundedBorder)
            TextField("Title", text: $model.lowerThirdTitle)
                .textFieldStyle(.roundedBorder)
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
            ScrollView {
                VStack(spacing: 4) {
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
            }
            .frame(maxHeight: 380)
        }
        .modifier(StudioPanel())
    }
}

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
