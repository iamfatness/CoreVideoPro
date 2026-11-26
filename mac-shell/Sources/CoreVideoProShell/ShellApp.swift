// CoreVideo Pro — macOS shell. Composition mirrors the Windows product
// (ground truth: artifacts/alpha-evidence/soak-*.png + StudioWorkspace.xaml):
// header with pill tabs under the Produce group label + Capture/Zoom Live
// pills, readiness strip, and the Studio workspace — scene rail (numbered
// rows, PGM amber / PVW green), PROGRAM + PREVIEW monitors (preview LEFT),
// show-inputs strip, "Video in room" participant rail — over the transport
// (Magic Scene green, Take amber, Record red, Stream blue) and the OUTPUTS
// status row with the MASTER meter. Palette from App.xaml.

import AppKit
import Combine
import IOSurface
import SwiftUI
import UniformTypeIdentifiers

struct StudioPanel: ViewModifier {
    func body(content: Content) -> some View {
        content
            .padding(10)
            .background(RoundedRectangle(cornerRadius: 8).fill(Studio.panel))
            .overlay(RoundedRectangle(cornerRadius: 8).stroke(Studio.stroke, lineWidth: 1))
    }
}

// Routes Cmd-Q / Dock Quit through the quit guard (QuitGuard.swift). The model
// registers itself on start(); until then (and in the test/selfcheck harnesses)
// a quit is immediate.
final class ShellAppDelegate: NSObject, NSApplicationDelegate {
    @MainActor static weak var quitHost: AppModel?

    func applicationShouldTerminate(_ sender: NSApplication) -> NSApplication.TerminateReply {
        MainActor.assumeIsolated {
            guard let host = Self.quitHost else { return .terminateNow }
            return host.handleQuitRequest()
        }
    }

    // Dock click / `reopen` AppleEvent. The main window is single-instance: bring
    // the existing one forward; only when it has been CLOSED does one get made,
    // and that one shares the same AppModel (start() is once-per-process).
    func applicationShouldHandleReopen(_ sender: NSApplication, hasVisibleWindows flag: Bool) -> Bool {
        MainActor.assumeIsolated {
            switch MainWindowTracker.reopenAction(otherWindowsVisible: flag) {
            case .broughtForward: return false
            case .openedMain: return false
            case .letSwiftUIOpenMain: return true
            }
        }
    }
}

@main
struct ShellApp: App {
    @NSApplicationDelegateAdaptor(ShellAppDelegate.self) private var appDelegate
    @StateObject private var model = AppModel()

    init() {
        setvbuf(stdout, nil, _IOLBF, 0)
        // Redaction self-check harness (there is no XCTest target in this SPM
        // package): seeds every secret shape the product handles, exports a REAL
        // support bundle to a temp dir and greps it. Runs before any UI.
        //   COREVIDEO_SHELL_SELFCHECK=1 .build/release/CoreVideoProShell
        if ProcessInfo.processInfo.environment["COREVIDEO_SHELL_TESTS"] != nil {
            ShellTests.runAndExit()
        }
        if ProcessInfo.processInfo.environment["COREVIDEO_SHELL_SELFCHECK"] != nil {
            DiagnosticsSelfCheck.runAndExit()
        }
        // Headless bundle export for the case the UI is what's broken.
        if let destination =
            ProcessInfo.processInfo.environment["COREVIDEO_SHELL_EXPORT_BUNDLE"] {
            DiagnosticsSelfCheck.exportHeadlessAndExit(destination: destination)
        }
        DesignFonts.register()
        if let dir = ProcessInfo.processInfo.environment["COREVIDEO_SHELL_FOOTER_SNAPSHOT"] {
            MainActor.assumeIsolated { FooterSnapshotHarness.runAndExit(dir: dir) }
        }
        if let dir = ProcessInfo.processInfo.environment["COREVIDEO_SHELL_GRAPHICS_SNAPSHOT"] {
            MainActor.assumeIsolated { GraphicsLayoutSnapshot.runAndExit(dir: dir) }
        }
        if let dir = ProcessInfo.processInfo.environment["COREVIDEO_SHELL_TAB_SNAPSHOT"] {
            MainActor.assumeIsolated { TabSnapshotHarness.runAndExit(dir: dir) }
        }
        // A bare (non-bundled) executable defaults to an activation policy
        // whose windows cannot become KEY — text fields silently refuse
        // input (the engine-bundle lesson). Force Regular + activate.
        NSApplication.shared.setActivationPolicy(.regular)
        DispatchQueue.main.async {
            NSApplication.shared.activate(ignoringOtherApps: true)
        }
    }

    var body: some Scene {
        // The console is SINGLE-INSTANCE. A WindowGroup (kept so closing the
        // console does not quit a recording app, and the quit prompt can fall back
        // to its panel) would otherwise mint a second console on File > New Window
        // or on an external URL; New Window is removed below, external events are
        // routed to the existing window, and Dock reopen is handled in the delegate.
        WindowGroup("CoreVideo Pro", id: MainWindowTracker.sceneId) {
            RootView()
                .environmentObject(model)
                .handlesExternalEvents(preferring: ["*"], allowing: ["*"])
                .modifier(MainWindowOpenerRegistration())
                .onAppear {
                    ShellAppDelegate.quitHost = model
                    model.start()
                }
                .modifier(PopoutAutoOpen())
                .onOpenURL { url in model.handleOAuthCallback(url) }
                .frame(minWidth: 1360, minHeight: 860)
                .studioChrome()
        }
        .commands {
            // No File > New Window (⌘N): there is exactly one console.
            CommandGroup(replacing: .newItem) {}
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
                .modifier(MainWindowOpenerRegistration())
                .padding(8)
                .frame(minWidth: 640, minHeight: 400)
                .studioChrome()
        }

        Window("Program / Preview", id: PopoutWindow.busMonitor) {
            BusMonitorPopout()
                .environmentObject(model)
                .modifier(MainWindowOpenerRegistration())
                .frame(minWidth: 720, minHeight: 260)
                .studioChrome()
        }

        // The Program Look (grading plan D1): the programme-wide picture control
        // for sources without their own grade. A window (not a tab) because
        // grading is done while WATCHING program.
        Window("Program Look", id: PopoutWindow.colorGrade) {
            ColorGradePane()
                .environmentObject(model)
                .modifier(MainWindowOpenerRegistration())
                .frame(minWidth: 420, minHeight: 320)
                .studioChrome()
        }

        // Per-source grade editors (grading plan Task 6), one value window per
        // source; opened from the Sources inspector's Picture tab.
        WindowGroup("Color grade", id: PopoutWindow.sourceGrade, for: String.self) { $sourceId in
            SourceGradeEditorWindow(sourceId: sourceId ?? "")
                .environmentObject(model)
                .modifier(MainWindowOpenerRegistration())
                // Room for the 360 pt controls column plus a usable picture over the 300 pt scopes.
                .frame(minWidth: SourceGradeEditorPane.minimumSize.width,
                       minHeight: SourceGradeEditorPane.minimumSize.height)
                .studioChrome()
        }
        .defaultSize(width: SourceGradeEditorPane.defaultSize.width, height: SourceGradeEditorPane.defaultSize.height)

        Window("Audio Mixer", id: PopoutWindow.audioMixer) {
            AudioPane(isPopout: true)
                .environmentObject(model)
                .modifier(MainWindowOpenerRegistration())
                .frame(minWidth: AudioLayout.popoutMin.width, minHeight: AudioLayout.popoutMin.height)
                .studioChrome()
        }
        // W/Views/AudioMixerWindow.xaml.cs:17-18, :68: Windows opens the mixer at 1080 x 700.
        .defaultSize(width: AudioLayout.popoutDefault.width, height: AudioLayout.popoutDefault.height)
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
            StudioFooter()
        }
        .background(Studio.background)
        .overlay { QuitConfirmation() }
        // Identifies THE main window for the quit prompt (not windows.first,
        // which can be a pop-out).
        .background(MainWindowReader())
        .onAppear { model.syncTransportHotkeys(); model.syncCommsKeys() }
        .onChange(of: model.selectedTab) { _ in model.syncTransportHotkeys(); model.syncCommsKeys() }
    }
}

// ── Quit prompt placement (final review IMPORTANT 7) ─────────────────────────
// The confirmation is mounted in RootView, so it can only be answered while the
// main window is on screen. The WindowGroup app keeps running with that window
// closed; then the same design-system view is hosted in its own panel.

@MainActor
enum MainWindowTracker {
    private static weak var window: NSWindow?

    static func track(_ candidate: NSWindow?) {
        if let candidate { window = candidate }
    }

    static func isMain(_ candidate: NSWindow?) -> Bool {
        guard let candidate, let window else { return false }
        return candidate === window
    }

    static var isAvailable: Bool {
        guard let window else { return false }
        return window.isVisible || window.isMiniaturized
    }

    static func bringForward() {
        guard let window else { return }
        if window.isMiniaturized { window.deminiaturize(nil) }
        window.makeKeyAndOrderFront(nil)
    }

    /// Scene id of the single console window.
    static let sceneId = "main"
    /// Opens the console scene; registered by every window's view so the Dock
    /// handler can reach SwiftUI's openWindow while only a pop-out is on screen.
    static var openMain: (() -> Void)?

    enum ReopenAction: Equatable { case broughtForward, openedMain, letSwiftUIOpenMain }

    /// The console exists (visible or minimized) -> bring it forward, never a new
    /// one. It was closed while a pop-out is still up -> open the ONE console
    /// (SwiftUI does nothing when any window is visible). Nothing on screen ->
    /// SwiftUI's default reopen makes the one console.
    static func reopenAction(otherWindowsVisible: Bool) -> ReopenAction {
        if isAvailable { bringForward(); return .broughtForward }
        if otherWindowsVisible, let openMain { openMain(); return .openedMain }
        return .letSwiftUIOpenMain
    }
}

struct MainWindowOpenerRegistration: ViewModifier {
    @Environment(\.openWindow) private var openWindow

    func body(content: Content) -> some View {
        content.onAppear {
            let open = openWindow
            MainWindowTracker.openMain = { open(id: MainWindowTracker.sceneId) }
        }
    }
}

struct MainWindowReader: NSViewRepresentable {
    final class Probe: NSView {
        override func viewDidMoveToWindow() {
            super.viewDidMoveToWindow()
            MainWindowTracker.track(window)
        }
    }

    func makeNSView(context: Context) -> NSView { Probe() }
    func updateNSView(_ nsView: NSView, context: Context) {}
}

@MainActor
enum QuitPromptPanel {
    private static var panel: NSPanel?
    private static var watch: AnyCancellable?
    /// Proof-shot only (COREVIDEO_SHELL_QUITPANEL): the panel's content view.
    static var proofView: NSView? { panel?.contentView }

    static func show(model: AppModel) {
        if let panel {
            panel.makeKeyAndOrderFront(nil)
            return
        }
        let host = NSHostingController(
            rootView: QuitConfirmation(standalone: true).environmentObject(model))
        let created = NSPanel(contentRect: NSRect(x: 0, y: 0, width: 460, height: 200),
                              styleMask: [.titled, .fullSizeContentView],
                              backing: .buffered, defer: false)
        created.titleVisibility = .hidden
        created.titlebarAppearsTransparent = true
        created.isReleasedWhenClosed = false
        created.level = .modalPanel
        created.contentViewController = host
        created.center()
        created.makeKeyAndOrderFront(nil)
        panel = created
        // Close once nothing is pending (Keep running). Checked on the next
        // turn: confirm flips the prompt off and the finishing notice on.
        watch = model.$quitPromptVisible.combineLatest(model.$quitFinishing)
            .sink { [weak model] _, _ in
                DispatchQueue.main.async {
                    guard let model, !model.quitPromptVisible, !model.quitFinishing else { return }
                    close()
                }
            }
    }

    static func close() {
        panel?.orderOut(nil)
        panel = nil
        watch = nil
    }
}

// In-app "Stop outputs and quit?" confirmation (design-system styled — no stock
// alert). Keep running is the default action (Return / Esc).
struct QuitConfirmation: View {
    @EnvironmentObject var model: AppModel
    /// Hosted in its own panel (main window closed): no dimming scrim.
    var standalone = false

    var body: some View {
        if model.quitPromptVisible || model.quitFinishing {
            ZStack {
                if standalone {
                    Studio.background.ignoresSafeArea()
                } else {
                    Color.black.opacity(0.55).ignoresSafeArea()
                }
                VStack(alignment: .leading, spacing: 14) {
                    Text(model.quitFinishing ? "Finishing your files" : "Stop outputs and quit?")
                        .font(.grotesk(16, .semibold))
                        .foregroundStyle(Studio.textPrimary)
                    Text(model.quitFinishing
                         ? "Waiting for the recording to close so it stays playable. CoreVideo Pro quits when it is done."
                         : "A recording or stream is live. Quitting stops it and finishes the files first, so the recording stays playable.")
                        .font(.grotesk(13))
                        .foregroundStyle(Studio.secondary)
                        .fixedSize(horizontal: false, vertical: true)
                    if !model.quitFinishing {
                        HStack(spacing: 10) {
                            Spacer()
                            Button("Stop and quit") { model.confirmQuit() }
                                .buttonStyle(GhostButtonStyle(tint: Studio.red))
                            Button("Keep running") { model.cancelQuit() }
                                .buttonStyle(AccentButtonStyle())
                                .keyboardShortcut(.defaultAction)
                        }
                    }
                }
                .padding(20)
                .frame(width: 420)
                .background(RoundedRectangle(cornerRadius: 10).fill(Studio.panel))
                .overlay(RoundedRectangle(cornerRadius: 10).stroke(Studio.border, lineWidth: 1))
                .onExitCommand { if !model.quitFinishing { model.cancelQuit() } }
            }
            // The standalone panel sizes to its content, so give the 420pt card real
            // margins (it was flush to the panel edge, its border clipped).
            .frame(minWidth: standalone ? 470 : nil, minHeight: standalone ? 240 : nil)
            .transition(.opacity)
        }
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
            GroupLabel(StudioTab.navGroups[0])
            ForEach(StudioTab.navigation, id: \.self) { TabPill(tab: $0) }
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
        Button(tab.navLabel) { model.selectedTab = tab }
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
    /// The header clock: main view only, and only with the Clock overlay on (Windows
    /// host ShowClock="False" by default; StudioWorkspace.xaml:429-433).
    static func showsHeaderClock(isPopout: Bool, overlays: MultiviewOverlays) -> Bool {
        !isPopout && overlays.showClock
    }

    /// The pop-out's on-canvas clock badge (MultiviewPopoutWindow.xaml:39-48 hosts
    /// ShowMultiviewHost with the Clock toggle).
    static func showsCanvasClock(isPopout: Bool, overlays: MultiviewOverlays) -> Bool {
        isPopout && overlays.showClock
    }

    @EnvironmentObject var model: AppModel
    @Environment(\.openWindow) private var openWindow
    // The pop-out hosts this same view; it just hides the pop-out affordances.
    var isPopout: Bool = false

    var body: some View {
        VStack(spacing: 6) {
            HStack(spacing: 8) {
                Text("MULTIVIEWER").font(.grotesk(12, .semibold))
                MonoChip(model.coreCanvasLabel)
                Text("\(model.liveInputCount) live inputs")
                    .font(.grotesk(11)).foregroundStyle(Studio.secondary)
                Spacer()
                // Windows StudioWorkspace.xaml:429-433: the main view's clock follows the
                // Clock overlay (default off); the pop-out draws it on the PGM cell.
                if Self.showsHeaderClock(isPopout: isPopout, overlays: model.multiviewOverlays) {
                    Text(model.clockText)
                        .font(.plexMono(12, .medium)).foregroundStyle(Studio.textPrimary)
                }
                // Windows POS-1/POS-2 (Tier 0.14): edit the PREVIEW scene in
                // place, and add an overlay layer to it. The multiview layout
                // mode lives in Settings > Multiview, as on Windows.
                if !isPopout {
                    ActionChip("Edit layout", accent: model.studioLayoutEditing) {
                        model.toggleStudioLayoutEditing()
                    }
                    .help("Move and resize the preview scene's sources. Changes ride the preview until you Take.")
                    AddOverlayMenu()
                }
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
            if !isPopout, !model.studioCommandStatus.isEmpty {
                Text(model.studioCommandStatus)
                    .font(.grotesk(11)).foregroundStyle(Studio.secondary)
                    .lineLimit(2)
                    .frame(maxWidth: .infinity, alignment: .leading)
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
                        MultiviewTileChrome(tile: tile, overlays: model.multiviewOverlays,
                                            masterLeft: Double(model.masterLevelLeft) / 100,
                                            masterRight: Double(model.masterLevelRight) / 100)
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
                            // Drag one source tile onto another: swap the two
                            // input slots (Windows TileSwapCommand). A short
                            // press-release stays a click (cue).
                            .gesture(DragGesture(minimumDistance: 8, coordinateSpace: .named("multiview"))
                                .onEnded { value in
                                    guard tile.role == "source",
                                          geometry.size.width > 0, geometry.size.height > 0 else { return }
                                    let point = CGPoint(x: value.location.x / geometry.size.width,
                                                        y: value.location.y / geometry.size.height)
                                    guard let target = MultiviewDragPolicy.dropTarget(
                                              tiles: model.multiviewTiles, from: tile, at: point),
                                          let fromSlot = model.slotForSourceId(tile.id),
                                          let toSlot = model.slotForSourceId(target.id) else { return }
                                    model.swapMultiviewSlots(fromSlot, toSlot)
                                })
                    }
                }
                .coordinateSpace(name: "multiview")
                // Windows ShowMultiviewHost ClockChrome: ONE clock, top-right of the whole
                // multiview, whatever the layout (so Grid has one too). Pop-out only; the
                // main view's clock is in the header.
                if Self.showsCanvasClock(isPopout: isPopout, overlays: model.multiviewOverlays) {
                    MultiviewClockBadge(text: model.clockText)
                        .padding(10)
                        .frame(maxWidth: .infinity, maxHeight: .infinity, alignment: .topTrailing)
                        .allowsHitTesting(false)
                }
                if MultiviewEmptyStatePolicy.showAssignmentGuidance(
                    tileCount: model.multiviewTiles.count,
                    multiviewSurfaceId: model.multiviewSurfaceId) {
                    VStack(spacing: 6) {
                        Text("Multiviewer")
                            .font(.grotesk(14, .semibold))
                            .foregroundStyle(Studio.secondary)
                        Text("Join Zoom or connect sources, assign inputs, "
                             + "pick a scene — preview and program composite here.")
                            .font(.grotesk(12)).foregroundStyle(Studio.textDim)
                    }
                }
                if model.studioLayoutEditing && !isPopout {
                    StudioLayoutEditorOverlay()
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
    var overlays = MultiviewOverlays()
    var masterLeft = 0.0
    var masterRight = 0.0

    var meter: (Double, Double)? {
        MultiviewOverlayFormatting.programMeter(
            left: masterLeft, right: masterRight, showMeters: overlays.showMeters,
            isProgramTile: MultiviewOverlayFormatting.isProgramTile(role: tile.role, tally: tile.tally))
    }

    var accent: Color {
        switch tile.role {
        case "pgm": return Studio.red
        case "pvw": return Studio.accent
        default:
            // The core's tally words are "pgm" / "pvw" (Windows ResolveTally).
            switch MultiviewOverlayFormatting.normalizeTally(tile.tally) {
            case "pgm": return Studio.red
            case "pvw": return Studio.accent
            default: return tile.activeSpeaker ? Studio.accent : Studio.border
            }
        }
    }

    var title: String {
        switch tile.role {
        // The core appends a bus's subscription-limit notice to the cell label.
        case "pgm", "pvw": return AppModel.busCaption(role: tile.role, label: tile.label)
        default: return tile.label.isEmpty ? "Source" : tile.label
        }
    }

    var isBus: Bool { tile.role == "pgm" || tile.role == "pvw" }

    var body: some View {
        ZStack(alignment: .bottomLeading) {
            Rectangle().fill(Color.clear)
            if let meter {
                HStack(alignment: .bottom, spacing: 2) {
                    SegmentedMeter(level: Int((meter.0 * 100).rounded())).frame(width: 6)
                    SegmentedMeter(level: Int((meter.1 * 100).rounded())).frame(width: 6)
                }
                .padding(.vertical, 22).padding(.trailing, 6)
                .frame(maxWidth: .infinity, maxHeight: .infinity, alignment: .trailing)
            }
            if overlays.showLabels {
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
        }
        .overlay(Rectangle().stroke(overlays.showTally ? accent : Color.clear, lineWidth: isBus ? 3 : 2))
        .contentShape(Rectangle())
        .allowsHitTesting(!isBus)
    }
}

/// Windows ClockChrome: HH:MM:SS on a dark rounded badge.
struct MultiviewClockBadge: View {
    let text: String
    var body: some View {
        Text(text)
            .font(.plexMono(14, .semibold)).foregroundStyle(Studio.textPrimary)
            .padding(.horizontal, 8).padding(.vertical, 3)
            .background(RoundedRectangle(cornerRadius: 4).fill(Color.black.opacity(0.69)))
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
    @State private var renaming: String?

    // A solo cue shows the SOURCE, not a scene name — the rail has no row for
    // the reserved solo scenes (on either bus, now that one can be Taken).
    var programName: String { model.busLabel(for: model.programSceneId) }

    var previewName: String { model.busLabel(for: model.previewSceneId) }

    var body: some View {
        VStack(alignment: .leading, spacing: 8) {
            Text("Scenes").font(.grotesk(13, .semibold))
            Text("\(model.railScenes.count) scenes · 1920x1080 · 60 fps")
                .font(.grotesk(10)).foregroundStyle(Studio.secondary)
            HStack(spacing: 6) {
                SceneChip(label: "PGM \(programName)", color: Studio.amber)
                SceneChip(label: "PVW \(previewName)", color: Studio.accent)
            }
            ForEach(Array(model.railScenes.enumerated()), id: \.element.id) { index, scene in
                SceneRow(index: index + 1, scene: scene, rename: { renaming = scene.id })
            }
            if !model.sceneEditMessage.isEmpty {
                Text(model.sceneEditMessage).font(.grotesk(10)).foregroundStyle(Studio.secondary)
                    .fixedSize(horizontal: false, vertical: true)
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
            SuperSourceBackgroundPicker()
            Spacer()
        }
        .modifier(StudioPanel())
        .sheet(isPresented: Binding(get: { renaming != nil }, set: { if !$0 { renaming = nil } })) {
            if let id = renaming {
                SceneRenameSheet(sceneId: id, presented: $renaming).environmentObject(model)
            }
        }
    }
}

/// SuperSource background for the PREVIEW scene — the scene the operator is
/// building, matching where WinUI puts it on the Studio rail. The core
/// composites it full-canvas behind every route ("media-background", order
/// -100), and it is per-scene, so the summary always names which scene it
/// describes rather than reading like a global setting.
struct SuperSourceBackgroundPicker: View {
    @EnvironmentObject var model: AppModel

    var scene: SceneDef? {
        // Solo scenes have no background to pick (they are not operator scenes).
        model.railScenes.first(where: { $0.id == model.previewSceneId })
            ?? model.railScenes.first(where: { $0.id == model.programSceneId })
    }

    var selected: MediaAsset? {
        guard let id = scene?.backgroundAssetId, !id.isEmpty else { return nil }
        return model.mediaAssets.first(where: { $0.id == id })
    }

    // Stills and AVFoundation-playable video: a background is composited behind
    // every route for the whole scene (a video one loops), and the core enables
    // it only with a real asset PATH. The old filter asked for kinds "image" /
    // "background", which the bin classifier never produces, so this menu was
    // always empty.
    var options: [MediaAsset] {
        MediaBin.backgroundOptions(model.mediaAssets)
    }

    var body: some View {
        VStack(alignment: .leading, spacing: 6) {
            Text("SuperSource background")
                .font(.grotesk(11, .semibold)).foregroundStyle(Studio.secondary)
            Menu {
                Button("No background") {
                    if let scene { model.setSceneBackground(sceneId: scene.id, assetId: "") }
                }
                if !options.isEmpty { Divider() }
                ForEach(options) { asset in
                    Button(asset.name) {
                        if let scene {
                            model.setSceneBackground(sceneId: scene.id, assetId: asset.id)
                        }
                    }
                }
            } label: {
                Text(selected?.name ?? "No background")
                    .font(.grotesk(11))
                    .foregroundStyle(selected == nil ? Studio.secondary : Studio.textPrimary)
                    .lineLimit(1)
            }
            .menuStyle(.borderlessButton)
            .disabled(scene == nil)
            .frame(maxWidth: .infinity)
            .padding(.horizontal, 8).padding(.vertical, 5)
            .background(RoundedRectangle(cornerRadius: 6).fill(Studio.field))
            .overlay(RoundedRectangle(cornerRadius: 6).stroke(Studio.border, lineWidth: 1))

            // Say WHICH scene this applies to, and say plainly when the bin has
            // nothing to offer rather than presenting an empty menu.
            Text(summary)
                .font(.grotesk(10)).foregroundStyle(Studio.textDim)
                .fixedSize(horizontal: false, vertical: true)
            Button("Import background media") { model.selectedTab = .media }
                .buttonStyle(GhostButtonStyle())
                .frame(maxWidth: .infinity)
        }
        .padding(.top, 6)
    }

    var summary: String {
        guard let scene else { return "No scene selected." }
        if options.isEmpty {
            return "No stills or videos in the media bin — import one to use as a "
                + "background for \(scene.name)."
        }
        if let selected {
            return "\(selected.name) sits behind every source in \(scene.name)."
        }
        return "No SuperSource background for \(scene.name)."
    }
}

struct SceneRow: View {
    @EnvironmentObject var model: AppModel
    let index: Int
    let scene: SceneDef
    var rename: () -> Void = {}

    var onProgram: Bool { scene.id == model.programSceneId }
    var onPreview: Bool { scene.id == model.previewSceneId }

    var body: some View {
        HStack(alignment: .top, spacing: 4) {
            VStack(spacing: 0) {
                rowButton
                // Windows StudioWorkspace.xaml: "Edit Tiles" inside a Tiles scene's
                // row card. Cues the wall on Preview and opens its inspector;
                // Program never changes.
                if scene.tiles != nil {
                    Button { model.editTiles(scene.id) } label: {
                        HStack(spacing: 6) {
                            Image(systemName: "square.grid.3x3").font(.system(size: 11))  // design-lint: allow
                            Text("Edit Tiles").font(.grotesk(12, .semibold))
                        }
                        .foregroundStyle(Studio.accent)
                        .frame(maxWidth: .infinity)
                        .padding(.vertical, 5)
                        .background(RoundedRectangle(cornerRadius: 6).fill(Studio.accent.opacity(0.12)))
                        .overlay(RoundedRectangle(cornerRadius: 6).stroke(Studio.accent, lineWidth: 1))
                    }
                    .buttonStyle(.plain)
                    .padding([.horizontal, .bottom], 10)
                    .help("Cue this wall on Preview and open its inspector")
                }
            }
            .background(RoundedRectangle(cornerRadius: 8)
                .fill(onProgram ? Studio.amber.opacity(0.12)
                      : onPreview ? Studio.accent.opacity(0.08) : Studio.card))
            .overlay(RoundedRectangle(cornerRadius: 8)
                .stroke(onProgram ? Studio.amber.opacity(0.7)
                        : onPreview ? Studio.accent.opacity(0.6) : Studio.stroke,
                        lineWidth: 1))
            // Windows: a Duplicate button beside every row (the copy is cued
            // on Preview).
            Button {
                model.duplicateScene(scene.id)
            } label: {
                Image(systemName: "plus.square.on.square").font(.system(size: 10))  // design-lint: allow
                    .frame(width: 22, height: 22)
            }
            .buttonStyle(.plain)
            .foregroundStyle(Studio.secondary)
            .help("Duplicate scene")
            .padding(.top, 8)
        }
    }

    var rowButton: some View {
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
                    // Windows LayoutLabel (SceneDisplayItem.cs): "dynamic gallery".
                    Text(scene.layout.replacingOccurrences(of: "-", with: " ")).font(.grotesk(9))
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
            .contentShape(Rectangle())
        }
        .buttonStyle(.plain)
        // Windows rail context menu: by this row's stable id, without cueing it.
        .contextMenu {
            Button("Rename scene", action: rename)
            if model.railCanRemoveScene(scene.id) {
                Button("Remove scene") { model.deleteScene(scene.id) }
            }
        }
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

    // Width per tab comes from TabPageLayout (a flat 720 pt cap clipped the
    // Sources rows and the Routing grid).
    var body: some View {
        if !Self.wrapsInScrollView(model.selectedTab, tilesEditing: model.tilesEditTargetId != nil) {
            TabPageContent(tab: model.selectedTab).frame(maxHeight: .infinity, alignment: .top)
        } else {
            ScrollView {
                TabPageContent(tab: model.selectedTab)
            }
        }
    }

    /// OD-3 (Windows AudioPage.xaml:31-36): the Audio tab's SHOW surface is a
    /// fixed viewport that fills the window and never scrolls, so the Audio tab
    /// is not wrapped in the page ScrollView; its other surfaces scroll inside
    /// AudioPane. The Tiles workspace (Scenes while a wall is edited) does not
    /// scroll either; every other tab keeps the page scroll.
    static func wrapsInScrollView(_ tab: StudioTab, tilesEditing: Bool = false) -> Bool {
        TabPageLayout.scrolls(tab, tilesEditing: tilesEditing)
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

// ── Studio footer: StudioFooter.swift (Windows RC footer port) ─────────────

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
        if !model.zoomJoinStatus.isEmpty { return model.zoomJoinStatus }
        return model.meetingState == "error" ? "See Health for the engine warning"
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
                ZoomCameraQualityControls()
                HStack(spacing: 8) {
                    if inMeeting {
                        Button("Leave meeting") { model.leaveZoom() }
                            .buttonStyle(GhostButtonStyle(tint: Studio.red))
                    } else {
                        Button("Join Zoom") { model.joinZoom() }
                            .buttonStyle(AccentButtonStyle())
                            .disabled(model.joinMeetingId.isEmpty)
                        // #475: shown ONLY after a join failed because this
                        // account is already in the meeting. It ends the
                        // operator's own Zoom session, so it is a separate,
                        // explicit gesture and never the default Join.
                        if model.canEndOtherZoomSession {
                            Button("Join and end my other Zoom session") {
                                model.joinZoom(endOtherMeeting: true)
                            }
                            .buttonStyle(GhostButtonStyle())
                            .help("Ends this Zoom account's other session, including your own Zoom app, and joins here.")
                        }
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
                // From the core's zoom.readiness, like the Windows SDK chip
                // (SettingsPage.xaml:148-167) — not a fixed "Ready".
                let readiness = ZoomOperatorStatus.readiness(snapshot: model.lastSnapshot)
                let blocked = ZoomOperatorStatus.isBlocked(readiness, inMeeting: inMeeting)
                Text("Zoom status").font(.grotesk(14, .semibold))
                Text(ZoomOperatorStatus.chipLabel(readiness, inMeeting: inMeeting))
                    .font(.grotesk(12, .medium))
                    .foregroundStyle(blocked ? Studio.red : (inMeeting ? Studio.accent : Studio.textPrimary))
                    .padding(.horizontal, 12).padding(.vertical, 8)
                    .frame(maxWidth: .infinity, alignment: .leading)
                    .background(RoundedRectangle(cornerRadius: 8).fill(Studio.field))
                    .overlay(RoundedRectangle(cornerRadius: 8)
                        .stroke((blocked ? Studio.red : Studio.accent).opacity(0.6), lineWidth: 1))
                Text(ZoomOperatorStatus.guidance(readiness, inMeeting: inMeeting))
                    .font(.grotesk(12)).foregroundStyle(Studio.secondary)
                if blocked, let reason = ZoomOperatorStatus.blockingReason(snapshot: model.lastSnapshot) {
                    Text(reason).font(.plexMono(10)).foregroundStyle(Studio.amber)
                        .fixedSize(horizontal: false, vertical: true)
                }
            }
            .padding(14)
            .background(RoundedRectangle(cornerRadius: 10).fill(Studio.panel))
            .overlay(RoundedRectangle(cornerRadius: 10).stroke(Studio.border, lineWidth: 1))

            // D-T3: after "Zoom status" (Windows hosts it on its Zoom-tab settings page).
            TelemetryCard()
        }
        .frame(maxWidth: 520, alignment: .leading)
    }
}

// Sources tab — the inputs-1–10 patch bay (03-sources.png + routing-ux-spec
// tab 1): "Show inputs (multiview)" slot table with in-show checkbox, source
// menu, editable name, ISO checkbox and Unassign, over the Zoom-feed-health
// and capture-devices reference cards.
struct MediaPane: View {
    @EnvironmentObject var model: AppModel

    var selected: MediaAsset? {
        model.mediaAssets.first { $0.id == model.selectedAssetId }
    }

    var body: some View {
        VStack(alignment: .leading, spacing: 10) {
            HStack {
                Text("Media").font(.grotesk(14, .semibold))
                Spacer()
                Button("Import…") {
                    let panel = NSOpenPanel()
                    panel.allowsMultipleSelection = true
                    panel.canChooseDirectories = false
                    // The Windows picker filter: only what the bin accepts.
                    panel.allowedContentTypes = MediaBin.supportedExtensions
                        .compactMap { UTType(filenameExtension: $0) }
                    // A cancelled panel is an import of nothing ("Media import canceled").
                    model.importMedia(urls: panel.runModal() == .OK ? panel.urls : [])
                }
                .accessibilityLabel("Import media files")
                Button("Refresh") { model.refreshMediaBin() }
                    .accessibilityLabel("Refresh the media bin")
            }
            MediaSelectedCard(asset: selected, playing: model.selectedMediaPlaying)
            HStack(spacing: 8) {
                Text(model.selectedMediaSummary)
                    .font(.grotesk(12, .semibold)).foregroundStyle(Studio.textPrimary)
                    .accessibilityLabel("Selected media: \(model.selectedMediaSummary)")
                Spacer()
                if let selected, selected.supportsPlayback {
                    Button(model.mediaPlaybackButtonLabel) { model.toggleSelectedMediaPlayback() }
                        .buttonStyle(GhostButtonStyle())
                        .disabled(!model.mediaClipPlaybackAvailable)
                        .help(model.mediaClipPlaybackAvailable
                              ? "Pause or resume the clip on Program; off Program, audition it here (muted)"
                              : "Clip playback is unavailable in this build")
                        .accessibilityLabel("\(model.mediaPlaybackButtonLabel): \(selected.name)")
                }
                Button("Cue in Preview") { model.cueSelectedInPreview() }
                    .buttonStyle(GhostButtonStyle())
                    .disabled(!model.canCueSelectedMedia)
                    .help(model.cueRefusalReason ?? "Add the selected asset to the Preview scene as a full-canvas layer")
                    .accessibilityLabel("Cue the selected asset in Preview")
            }
            // Plan D9: never the same sentence twice (the summary line above already says it).
            if let status = MediaRoutePlayback.statusLine(status: model.mediaStatus,
                                                          summary: model.selectedMediaSummary) {
                Text(status)
                    .font(.grotesk(11)).foregroundStyle(Studio.secondary)
                    .fixedSize(horizontal: false, vertical: true)
                    .accessibilityLabel("Media status: \(status)")
            }
            if let warning = model.mediaPlayback?.warnings.first {
                Text(warning)
                    .font(.grotesk(11)).foregroundStyle(Studio.amber)
                    .fixedSize(horizontal: false, vertical: true)
                    .accessibilityLabel("Media warning: \(warning)")
            }
            VStack(alignment: .leading, spacing: 2) {
                Text(MediaBin.summary(model.mediaAssets))
                    .font(.grotesk(11)).foregroundStyle(Studio.secondary)
                Text(MediaBin.root)
                    .font(.plexMono(8))
                    .foregroundStyle(Studio.secondary.opacity(0.6))
                    .lineLimit(1).truncationMode(.head)
            }
            ForEach(MediaBin.groups(model.mediaAssets), id: \.kind) { group in
                VStack(alignment: .leading, spacing: 2) {
                    Text(group.label.uppercased())
                        .font(.plexMono(9, .semibold)).foregroundStyle(Studio.textDim)
                        .padding(.top, 4)
                    ForEach(group.assets) { asset in
                        MediaBinRow(asset: asset)
                    }
                }
            }
        }
        .modifier(StudioPanel())
    }
}

/// "<Kind> · <duration> · <W>x<H>", omitting what is unknown.
func mediaAssetDetail(_ asset: MediaAsset) -> String {
    var parts = [MediaBin.kindLabel(asset.kind)]
    if !asset.durationLabel.isEmpty { parts.append(asset.durationLabel) }
    if asset.naturalWidth > 0 { parts.append("\(asset.naturalWidth)×\(asset.naturalHeight)") }
    return parts.joined(separator: " · ")
}

/// The selected asset: what it is, and a local look at it. A clip sits paused
/// on its first frame; a still is drawn; audio has no picture.
struct MediaSelectedCard: View {
    let asset: MediaAsset?
    var playing = false

    private var isAudio: Bool {
        guard let asset else { return false }
        return MediaBin.audioExtensions.contains((asset.filePath as NSString).pathExtension.lowercased())
    }

    private var playsLocally: Bool {
        guard let asset else { return false }
        return MediaBin.avFoundationVideoExtensions.contains((asset.filePath as NSString).pathExtension.lowercased())
    }

    var body: some View {
        VStack(alignment: .leading, spacing: 6) {
            if let asset {
                Text(asset.name).font(.grotesk(13, .semibold)).foregroundStyle(Studio.textPrimary).lineLimit(1)
                Text(mediaAssetDetail(asset)).font(.plexMono(9)).foregroundStyle(Studio.secondary)
                if asset.isStillImage {
                    if let image = NSImage(contentsOfFile: asset.filePath) {
                        Image(nsImage: image).resizable().scaledToFit()
                            .frame(maxWidth: 320, maxHeight: 180, alignment: .leading)
                            .accessibilityLabel("Preview of \(asset.name)")
                    }
                } else if playsLocally {
                    MediaPreviewPlayer(url: URL(fileURLWithPath: asset.filePath), playing: playing)
                        .frame(width: 320, height: 180)
                        .background(Studio.field)
                        .clipShape(RoundedRectangle(cornerRadius: 6))
                } else if !isAudio {
                    Text("No local preview: this build cannot play \((asset.filePath as NSString).pathExtension) files.")
                        .font(.grotesk(11)).foregroundStyle(Studio.textDim)
                }
            } else {
                Text("No media asset selected").font(.grotesk(12)).foregroundStyle(Studio.secondary)
            }
        }
        .frame(maxWidth: .infinity, alignment: .leading)
        .padding(8)
        .background(RoundedRectangle(cornerRadius: 6).fill(Studio.field))
        .overlay(RoundedRectangle(cornerRadius: 6).stroke(Studio.border, lineWidth: 1))
    }
}

struct MediaBinRow: View {
    @EnvironmentObject var model: AppModel
    let asset: MediaAsset

    var body: some View {
        let isSelected = model.selectedAssetId == asset.id
        HStack(spacing: 8) {
            VStack(alignment: .leading, spacing: 1) {
                Text(asset.name).font(.grotesk(12)).lineLimit(1)
                Text(mediaAssetDetail(asset))
                    .font(.plexMono(9))
                    .foregroundStyle(Studio.secondary)
            }
            Spacer()
            if asset.isStillImage {
                Button(model.logoBug?.id == asset.id ? "Unbug" : "Bug") {
                    model.toggleLogoBug(asset)
                }
                .font(.grotesk(11))
                .accessibilityLabel(model.logoBug?.id == asset.id
                                    ? "Remove \(asset.name) as the logo bug" : "Use \(asset.name) as the logo bug")
            }
            // Windows PlaybackLabel: Play / Pause for anything that plays, "Still" otherwise.
            let rowPlaying = MediaBinPlaybackProjection.binRowIsPlaying(
                asset.id, supportsPlayback: asset.supportsPlayback, rows: model.mediaSources,
                selectedId: model.selectedAssetId, selectedPlaying: model.selectedMediaPlaying)
            if asset.supportsPlayback {
                Button(rowPlaying ? "Pause" : "Play") { model.playMediaAsset(asset.id) }
                    .font(.grotesk(11))
                    .disabled(!model.mediaClipPlaybackAvailable)
                    .help(model.mediaClipPlaybackAvailable ? "" : "Clip playback is unavailable in this build")
                    .accessibilityLabel(rowPlaying ? "Pause \(asset.name)" : "Play \(asset.name)")
            } else {
                Text("Still").font(.plexMono(9)).foregroundStyle(Studio.textDim)
            }
            Button(isSelected ? "Selected" : "Select") { model.selectMediaAsset(asset.id) }
                .font(.grotesk(11))
                .disabled(isSelected)
                .accessibilityLabel(isSelected ? "\(asset.name) is selected" : "Select \(asset.name)")
            Button("Remove") { model.removeMediaAsset(asset.id) }
                .font(.grotesk(11))
                .accessibilityLabel("Remove \(asset.name) from the media bin")
        }
        .padding(.vertical, 2)
    }
}

struct AutomationPane: View {
    @EnvironmentObject var model: AppModel

    var body: some View {
        VStack(alignment: .leading, spacing: 8) {
            Text("Manual production").font(.grotesk(14, .semibold))
            Text("Scene automation is unavailable in this release. Compose your show and use manual Take.").font(.grotesk(12))
            // Not scene automation: the slot law's newcomer fill, live in this release.
            Divider()
            AutoAssignInputsToggle()
        }
        .modifier(StudioPanel())
    }
}

// DiagnosePane now lives in Diagnostics.swift (the full Health (DIAGNOSE) surface plus
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
