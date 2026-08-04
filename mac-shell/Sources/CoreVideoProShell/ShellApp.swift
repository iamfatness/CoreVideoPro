// CoreVideo Pro — macOS shell (Phase 4 M1). Entry point + all M1 views.
// The load-bearing component is ProgramMonitorView: a CALayer whose contents
// is the compositor's live IOSurface, resolved by global ID from the
// snapshot — zero-copy presentation of the Metal-composited program.

import AppKit
import IOSurface
import SwiftUI

// The WinUI studio palette (App.xaml/StudioWorkspace.xaml): near-black app
// chrome, graphite panels, studio-green accent, muted secondary text. Same
// vibe, macOS-native controls.
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
                .frame(minWidth: 1080, minHeight: 640)
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
            StatusBar()
            HSplitView {
                VStack(spacing: 8) {
                    ProgramMonitorView(surfaceId: model.programSurfaceId,
                                       frameNumber: model.programFrameNumber)
                        .frame(minHeight: 320)
                        .layoutPriority(1)
                    TransportPane()
                }
                .padding(8)
                .frame(minWidth: 560)
                VStack(spacing: 12) {
                    ZoomPane()
                    AudioPane()
                    WarningsPane()
                }
                .padding(8)
                .frame(minWidth: 340, maxWidth: 460)
            }
        }
    }
}

struct StatusBar: View {
    @EnvironmentObject var model: AppModel

    var statusText: String {
        switch model.status {
        case .launching: return "core: launching…"
        case .connected(let renderer): return "core: connected (\(renderer))"
        case .exited(let code): return "core: exited (\(code)) — relaunching"
        case .failed(let why): return "core: \(why)"
        }
    }

    var body: some View {
        HStack {
            Circle()
                .fill(statusColor)
                .frame(width: 10, height: 10)
            Text(statusText).font(.system(.caption, design: .monospaced))
            if !model.statusDetail.isEmpty {
                Text(model.statusDetail).font(.caption2).foregroundStyle(.secondary)
            }
            Spacer()
            Text("meeting: \(model.meetingState)")
                .font(.system(.caption, design: .monospaced))
            Text(model.rawMediaActive ? "raw media: LIVE" : "raw media: off")
                .font(.system(.caption, design: .monospaced))
                .foregroundStyle(model.rawMediaActive ? Studio.accent : Studio.secondary)
        }
        .padding(.horizontal, 10)
        .padding(.vertical, 6)
        .background(Studio.panel)
        .overlay(Rectangle().frame(height: 1).foregroundStyle(Studio.stroke), alignment: .bottom)
    }

    var statusColor: Color {
        switch model.status {
        case .connected: return Studio.accent
        case .launching: return .yellow
        case .exited, .failed: return .red
        }
    }
}

// ── Program monitor (IOSurface presenter) ────────────────────────────────────

struct ProgramMonitorView: NSViewRepresentable {
    let surfaceId: UInt32
    let frameNumber: Int64

    func makeNSView(context: Context) -> ProgramMonitorNSView {
        ProgramMonitorNSView()
    }

    func updateNSView(_ view: ProgramMonitorNSView, context: Context) {
        view.present(surfaceId: surfaceId)
    }
}

final class ProgramMonitorNSView: NSView {
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

    deinit {
        refreshTimer?.invalidate()
    }

    func present(surfaceId: UInt32) {
        guard surfaceId != 0, surfaceId != currentSurfaceId else { return }
        guard let looked = IOSurfaceLookup(surfaceId) else { return }
        currentSurfaceId = surfaceId
        surface = looked
        layer?.contents = looked
    }
}

// ── Panes ────────────────────────────────────────────────────────────────────

struct TransportPane: View {
    @EnvironmentObject var model: AppModel

    var recording: Bool {
        model.recordingStatus == "recording" || model.recordingStatus == "warning"
    }

    var body: some View {
        HStack(spacing: 12) {
            Button(recording ? "Stop Recording" : "Record") { model.toggleRecording() }
                .tint(recording ? .red : nil)
            Text("recording: \(model.recordingStatus)")
                .font(.system(.caption, design: .monospaced))
                .foregroundStyle(recording ? .red : .secondary)
            if !model.recordingArtifactPath.isEmpty {
                Text(model.recordingArtifactPath)
                    .font(.caption2).foregroundStyle(.secondary)
                    .lineLimit(1).truncationMode(.head)
            }
            Spacer()
            Button("Stop Capture") { model.stopCapture() }
                .disabled(!model.rawMediaActive)
        }
        .modifier(StudioPanel())
    }
}

struct ZoomPane: View {
    @EnvironmentObject var model: AppModel

    var body: some View {
        VStack(alignment: .leading, spacing: 8) {
            Text("Zoom").font(.headline)
            HStack {
                TextField("Meeting ID or URL", text: $model.joinMeetingId)
                    .textFieldStyle(.roundedBorder)
                TextField("Passcode", text: $model.joinPasscode)
                    .textFieldStyle(.roundedBorder)
                    .frame(width: 110)
            }
            HStack {
                Button("Join") { model.joinZoom() }
                    .disabled(model.joinMeetingId.isEmpty)
                Button("Leave") { model.leaveZoom() }
                Spacer()
            }
            Divider()
            Text("Roster — assign to program").font(.subheadline)
            if model.roster.isEmpty {
                Text("No participants yet.").font(.caption).foregroundStyle(Studio.secondary)
            }
            ScrollView {
                VStack(spacing: 4) {
                    ForEach(model.roster) { participant in
                        HStack {
                            Circle()
                                .fill(participant.talking ? Studio.accent : Studio.secondary.opacity(0.4))
                                .frame(width: 8, height: 8)
                            Text(participant.name).lineLimit(1)
                            if participant.hasVideo {
                                Image(systemName: "video.fill").font(.caption2)
                            }
                            if participant.muted {
                                Image(systemName: "mic.slash.fill").font(.caption2)
                                    .foregroundStyle(.secondary)
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
            .frame(maxHeight: 180)
        }
        .modifier(StudioPanel())
    }
}

struct AudioPane: View {
    @EnvironmentObject var model: AppModel

    var body: some View {
        VStack(alignment: .leading, spacing: 8) {
            Text("Audio").font(.headline)
            HStack {
                Text("Master")
                MeterBar(level: model.masterLevel)
            }
            Toggle("Monitor (system default output)", isOn: Binding(
                get: { model.monitorEnabled },
                set: { model.setMonitor(enabled: $0, volume: model.monitorVolume) }))
            HStack {
                Text("Vol")
                Slider(value: Binding(
                    get: { model.monitorVolume },
                    set: { model.setMonitor(enabled: model.monitorEnabled, volume: $0) }),
                    in: 0...1)
            }
            .disabled(!model.monitorEnabled)
        }
        .modifier(StudioPanel())
    }
}

struct MeterBar: View {
    let level: Int  // 0..100

    var body: some View {
        GeometryReader { geometry in
            ZStack(alignment: .leading) {
                RoundedRectangle(cornerRadius: 2).fill(.black.opacity(0.4))
                RoundedRectangle(cornerRadius: 2)
                    .fill(level > 90 ? Color.red : level > 70 ? .yellow : Studio.accent)
                    .frame(width: geometry.size.width * CGFloat(min(100, max(0, level))) / 100.0)
            }
        }
        .frame(height: 10)
        .animation(.linear(duration: 0.08), value: level)
    }
}

struct WarningsPane: View {
    @EnvironmentObject var model: AppModel

    var body: some View {
        VStack(alignment: .leading, spacing: 6) {
            Text("Warnings").font(.headline)
            if model.warnings.isEmpty {
                Text("None.").font(.caption).foregroundStyle(Studio.secondary)
            }
            ScrollView {
                VStack(alignment: .leading, spacing: 3) {
                    ForEach(Array(model.warnings.enumerated()), id: \.offset) { _, warning in
                        Text(warning)
                            .font(.system(.caption2, design: .monospaced))
                            .foregroundStyle(.orange)
                            .textSelection(.enabled)
                    }
                }
            }
            .frame(maxHeight: 140)
        }
        .frame(maxWidth: .infinity, alignment: .leading)
        .modifier(StudioPanel())
    }
}
