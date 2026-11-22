// Studio footer — the Mac port of the Windows RC footer
// (native-shell/CoreVideoPro.WinUI/Views/StudioWorkspace.xaml `ProductionFooter`:
// StudioFooterLayoutPanel + FooterTelemetry; ruling text in
// docs/reference/first-release-scene-control.md, reference capture
// docs/reference/images/rc-studio-footer.png).
//
// Row 1: GRAPHICS (lower-third split button) and OUTPUTS (Record / Stream /
// Virtual camera / NDI split buttons) form a left group with a fixed gap; only
// the space before TRANSPORT (Take, transition choice, the black FTB key) grows.
// When the three groups cannot share one row, Graphics takes its own row (the
// StudioFooterLayoutPanel wrap rule). Row 2: telemetry in FIXED-WIDTH columns.
//
// LAYOUT NEVER MOVES ON STATUS (owner report 2026-11-15, "the bottom is
// bouncing"): every status string here sits in a slot whose size does not
// depend on the string — a conditional line under the master meter used to add
// and remove a row twice a second and bounce the whole window. Status changes
// replace text in place; they never add or remove a view.
//
// HONESTY: a Windows output the Mac does not have yet keeps its button (layout
// parity) but looks unavailable and its flyout says so in plain words.

import AppKit
import CameraExtensionKit
import SwiftUI

// ── pure readouts (tested in ShellTests) ─────────────────────────────────────

enum StudioFooterReadout {
    /// Take transitions the Mac actually renders. All four are built by the
    /// shared core (MediaCore::buildTakeTransitionPlan) out of layer opacity, a
    /// sourceless fillColor layer (dip) and a clipRect reveal (wipe); the Metal
    /// compositor honours all three (MetalCompositor.ClipRectConfinesLayerPixels,
    /// SourcelessFillColorLayerPaintsItsParsedColour).
    static let transitions: [(mode: String, label: String)] = [
        ("cut", "Cut"), ("fade", "Fade"), ("dip", "Dip"), ("wipe", "Wipe"),
    ]

    /// Windows `TransitionDuration(mode)`: a cut has no duration.
    static func transitionDuration(_ mode: String) -> String {
        mode == "cut" ? "Instant" : "\u{00B7} 300 ms"
    }

    /// Windows SetupProgressSummary: "N inputs · M Zoom guests".
    static func setupSummary(slots: [ShowInputSlot], guests: Int) -> String {
        let inShow = slots.filter { $0.inShow && $0.kind != "unassigned" }.count
        return "\(inShow) \(inShow == 1 ? "input" : "inputs") \u{00B7} "
            + "\(guests) Zoom \(guests == 1 ? "guest" : "guests")"
    }

    /// Windows MasterLufsLabel.
    static func lufsLabel(_ lufs: Double) -> String {
        lufs <= -59 || !lufs.isFinite ? "LUFS \u{2014}" : String(format: "%.1f LUFS", lufs)
    }

    static func truePeakLabel(_ dbfs: Double) -> String {
        dbfs <= -119 || !dbfs.isFinite ? "\u{2014}" : String(format: "Peak %.1f dBFS", dbfs)
    }

    /// The third line of the MASTER readout. The meter-age warning takes this
    /// line's place (Windows LoudnessLevelLabel shows the warning IN PLACE of
    /// its label) so its appearing and clearing never changes the footer height.
    static func masterStatusLine(warning: String?, limiterActive: Bool) -> String {
        if let warning { return warning }
        return limiterActive ? "Limiter reducing" : "Limiter idle"
    }

    /// AudioMeterScale.cs: linear -60..0 dBFS -> 0..100.
    static func meterLevel(dbfs: Double) -> Int {
        guard dbfs.isFinite, dbfs > -60 else { return 0 }
        return min(100, max(0, Int(((dbfs + 60) / 60 * 100).rounded())))
    }

    /// TransportViewModel.ResolveMasterMeterReadout: per-side peaks from the
    /// measured bus taps (pgm-l / pgm-r, else master); without taps both sides
    /// show the core's mono master level.
    static func masterLevels(snapshot: [String: Any], fallback: Int) -> (left: Int, right: Int) {
        let taps = ((snapshot["audioRoutingMatrix"] as? [String: Any])?["busTaps"]
            as? [[String: Any]]) ?? []
        func peak(_ bus: String) -> Double? {
            taps.first {
                ($0["busId"] as? String)?.lowercased() == bus
                    && (($0["frames"] as? NSNumber)?.intValue ?? 0) > 0
            }.flatMap { ($0["peakDbfs"] as? NSNumber)?.doubleValue }
        }
        let left = peak("pgm-l") ?? peak("master")
        let right = peak("pgm-r") ?? peak("master")
        guard left != nil || right != nil else { return (fallback, fallback) }
        let l = left.map(meterLevel(dbfs:)) ?? 0
        return (l, right.map(meterLevel(dbfs:)) ?? l)
    }

    // Outputs the Mac does not have yet. Verified 2026-11-15:
    //  - NDI output: the macOS release core is configured without
    //    COREVIDEO_WITH_NDI_OUTPUT (scripts/release-macos.sh) and the shell has
    //    no NDI destination — the NDI sender path is Windows-only.
    static let ndiUnavailable =
        "NDI output isn't available on Mac yet \u{2014} it's coming in a later update."
}

// ── shapes + small pieces ────────────────────────────────────────────────────

/// A rectangle with independent left/right corner radii (UnevenRoundedRectangle
/// needs macOS 14; the shell targets 13). Split buttons round the outer corners only.
struct SideRoundedRectangle: Shape {
    var left: CGFloat
    var right: CGFloat

    func path(in rect: CGRect) -> Path {
        var p = Path()
        let l = min(left, rect.height / 2), r = min(right, rect.height / 2)
        p.move(to: CGPoint(x: rect.minX + l, y: rect.minY))
        p.addLine(to: CGPoint(x: rect.maxX - r, y: rect.minY))
        p.addArc(center: CGPoint(x: rect.maxX - r, y: rect.minY + r), radius: r,
                 startAngle: .degrees(-90), endAngle: .degrees(0), clockwise: false)
        p.addLine(to: CGPoint(x: rect.maxX, y: rect.maxY - r))
        p.addArc(center: CGPoint(x: rect.maxX - r, y: rect.maxY - r), radius: r,
                 startAngle: .degrees(0), endAngle: .degrees(90), clockwise: false)
        p.addLine(to: CGPoint(x: rect.minX + l, y: rect.maxY))
        p.addArc(center: CGPoint(x: rect.minX + l, y: rect.maxY - l), radius: l,
                 startAngle: .degrees(90), endAngle: .degrees(180), clockwise: false)
        p.addLine(to: CGPoint(x: rect.minX, y: rect.minY + l))
        p.addArc(center: CGPoint(x: rect.minX + l, y: rect.minY + l), radius: l,
                 startAngle: .degrees(180), endAngle: .degrees(270), clockwise: false)
        p.closeSubpath()
        return p
    }
}

/// Windows OutputStatLabel: small, tracked, uppercase, muted.
struct FooterStatLabel: View {
    let text: String
    init(_ text: String) { self.text = text }

    var body: some View {
        Text(text.uppercased())
            .font(.plexMono(9))
            .tracking(9 * 0.18)
            .foregroundStyle(Studio.textDim)
            .lineLimit(1)
    }
}

struct FooterChevron: View {
    var body: some View {
        Image(systemName: "chevron.down")
            .font(.system(size: 8, weight: .semibold))  // design-lint: allow
            .foregroundStyle(Studio.secondary)
    }
}

/// The flyout body every footer option panel shares (Windows Flyout: a bold
/// title, a muted summary, the controls).
struct FooterFlyout<Content: View>: View {
    let title: String
    var width: CGFloat = 420
    @ViewBuilder let content: () -> Content

    var body: some View {
        VStack(alignment: .leading, spacing: 12) {
            Text(title).font(.grotesk(14, .bold)).foregroundStyle(Studio.textPrimary)
            content()
        }
        .padding(16)
        .frame(width: width, alignment: .leading)
        .background(Studio.panel)
    }
}

struct FooterMutedText: View {
    let text: String
    init(_ text: String) { self.text = text }

    var body: some View {
        Text(text).font(.grotesk(12)).foregroundStyle(Studio.secondary)
            .fixedSize(horizontal: false, vertical: true)
    }
}

// ── the split button (one component for all four outputs) ────────────────────

/// Windows footer output: a 44x48 main button (left corners rounded 8) joined to
/// a 24x48 chevron (right corners rounded 8) that opens the option flyout above.
/// Idle backgrounds are neutral; record/stream colours are reserved for their
/// active state (RC ruling).
struct SplitOutputButton<Icon: View, Flyout: View>: View {
    let name: String            // accessibility name of the main button
    let optionsName: String     // accessibility name + tooltip of the chevron
    let help: String
    var active = false
    var activeFill: Color = Studio.accent
    var available = true
    @Binding var flyoutShown: Bool
    /// nil = the main button opens the flyout (unavailable outputs).
    let action: (() -> Void)?
    @ViewBuilder let icon: () -> Icon
    @ViewBuilder let flyout: () -> Flyout

    private var fill: Color { active ? activeFill : Studio.surface }
    private var stroke: Color { active ? activeFill : Studio.border }

    var body: some View {
        HStack(spacing: 0) {
            Button {
                if let action { action() } else { flyoutShown = true }
            } label: {
                icon()
                    .frame(width: 44, height: 48)
                    .opacity(available ? 1 : 0.4)
                    .background(SideRoundedRectangle(left: 8, right: 0).fill(fill))
                    .overlay(SideRoundedRectangle(left: 8, right: 0).stroke(stroke, lineWidth: 1))
                    .contentShape(Rectangle())
            }
            .buttonStyle(.plain)
            .accessibilityLabel(name)
            .help(help)
            Button {
                flyoutShown.toggle()
            } label: {
                FooterChevron()
                    .frame(width: 24, height: 48)
                    .background(SideRoundedRectangle(left: 0, right: 8).fill(Studio.surface))
                    .overlay(SideRoundedRectangle(left: 0, right: 8).stroke(Studio.border, lineWidth: 1))
                    .contentShape(Rectangle())
            }
            .buttonStyle(.plain)
            .accessibilityLabel(optionsName)
            .help(optionsName)
        }
        .popover(isPresented: $flyoutShown, arrowEdge: .top) { flyout() }
    }
}

// ── footer ───────────────────────────────────────────────────────────────────

struct StudioFooter: View {
    @EnvironmentObject var model: AppModel

    var body: some View {
        VStack(spacing: 0) {
            ViewThatFits(in: .horizontal) {
                HStack(alignment: .bottom, spacing: 12) {
                    FooterGraphicsGroup()
                    FooterOutputsGroup()
                    Spacer(minLength: 12)
                    FooterTransportGroup()
                }
                VStack(alignment: .leading, spacing: 12) {
                    FooterGraphicsGroup()
                    HStack(alignment: .bottom, spacing: 12) {
                        FooterOutputsGroup()
                        Spacer(minLength: 12)
                        FooterTransportGroup()
                    }
                }
            }
            .padding(.horizontal, 10)
            .padding(.top, 8)
            .padding(.bottom, 12)
            // Windows FooterTelemetry is a horizontal ScrollViewer: below its
            // natural width the row scrolls rather than clipping or squeezing.
            ViewThatFits(in: .horizontal) {
                FooterTelemetry()
                ScrollView(.horizontal, showsIndicators: true) { FooterTelemetry() }
            }
                .overlay(Rectangle().frame(height: 1).foregroundStyle(Studio.border),
                         alignment: .top)
        }
        .frame(maxWidth: .infinity, alignment: .leading)
        .background(Studio.panel)
        .overlay(Rectangle().frame(height: 1).foregroundStyle(Studio.border), alignment: .top)
    }
}

// ── GRAPHICS ────────────────────────────────────────────────────────────────

struct FooterGraphicsGroup: View {
    @EnvironmentObject var model: AppModel
    @State private var optionsShown = false

    private var onAir: Bool {
        model.lowerThirdPhase == "on-air" || model.lowerThirdPhase == "building-in"
    }

    /// Fixed-width status (Windows GraphicsFooterLowerThirdStatus); a phase like
    /// "building-out" truncates instead of reflowing the row.
    private var status: String {
        switch model.lowerThirdPhase {
        case "hidden": return model.programLowerThirdSubject() == nil ? "No source" : "Ready"
        case "building-in": return model.lowerThirdVisible ? "Building in" : "Sync pending"
        case "on-air": return model.lowerThirdVisible ? "On air" : "Sync pending"
        case "building-out": return "Building out"
        default: return model.lowerThirdPhase
        }
    }

    var body: some View {
        VStack(alignment: .leading, spacing: 6) {
            FooterStatLabel("Graphics")
            HStack(spacing: 0) {
                Button {
                    model.toggleLowerThirdIntent()   // the one lower-third entry (spec 5.5)
                } label: {
                    HStack(spacing: 6) {
                        Text("L3")
                            .font(.plexMono(8, .bold))
                            .foregroundStyle(onAir ? Studio.onAccent : Studio.accent)
                            .frame(width: 18, height: 18)
                            .background(RoundedRectangle(cornerRadius: 3)
                                .fill(onAir ? Studio.accent : Studio.accent.opacity(0.14)))
                        VStack(alignment: .leading, spacing: 1) {
                            Text("Lower third").font(.grotesk(12, .bold))
                                .foregroundStyle(Studio.textPrimary)
                            Text(status).font(.grotesk(10, .bold))
                                .foregroundStyle(onAir ? Studio.accent : Studio.secondary)
                                .lineLimit(1).truncationMode(.tail)
                                .frame(maxWidth: 72, alignment: .leading)
                        }
                        Spacer(minLength: 0)
                    }
                    .padding(.horizontal, 10)
                    .frame(width: 118, height: 48)
                    .background(SideRoundedRectangle(left: 8, right: 0).fill(Studio.surface))
                    .overlay(SideRoundedRectangle(left: 8, right: 0)
                        .stroke(onAir ? Studio.accent.opacity(0.6) : Studio.border, lineWidth: 1))
                    .contentShape(Rectangle())
                }
                .buttonStyle(.plain)
                .accessibilityLabel(onAir ? "Lower third, key out" : "Lower third, key in")
                .help(onAir ? "Key the lower third out"
                      : (model.programLowerThirdSubject().map { "Key in \u{201C}\($0.name)\u{201D}" }
                         ?? "Take a scene with a source first"))
                Button { optionsShown.toggle() } label: {
                    FooterChevron()
                        .frame(width: 30, height: 48)
                        .background(SideRoundedRectangle(left: 0, right: 8).fill(Studio.surface))
                        .overlay(SideRoundedRectangle(left: 0, right: 8).stroke(Studio.border, lineWidth: 1))
                        .contentShape(Rectangle())
                }
                .buttonStyle(.plain)
                .accessibilityLabel("Lower third options")
                .help("Lower third options")
                .popover(isPresented: $optionsShown, arrowEdge: .top) { options }
            }
        }
    }

    private var options: some View {
        FooterFlyout(title: "Lower third controls", width: 300) {
            FooterMutedText(onAir ? model.lowerThirdKeyStatus
                            : (model.programLowerThirdSubject().map { "Ready: \($0.name)" }
                               ?? "Take a scene with a source first"))
            VStack(alignment: .leading, spacing: 4) {
                Text("Position").font(.grotesk(11)).foregroundStyle(Studio.secondary)
                Picker("", selection: $model.lowerThirdPosition) {
                    ForEach(LowerThirdPositions.all, id: \.value) { Text($0.label).tag($0.value) }
                }.labelsHidden()
            }
            HStack(spacing: 10) {
                msStepper("Build in", Binding(get: { model.overlays.buildInMs }, set: { model.setBuildInMs($0) }))
                msStepper("Build out", Binding(get: { model.overlays.buildOutMs }, set: { model.setBuildOutMs($0) }))
            }
            Text("in \(model.overlays.buildInMs) ms \u{00B7} out \(model.overlays.buildOutMs) ms")
                .font(.plexMono(10)).foregroundStyle(Studio.textDim)
            HStack(spacing: 8) {
                Button("Rebuild lower third") { model.rebuildLowerThird() }
                    .buttonStyle(AccentButtonStyle())
                    .disabled(!onAir)
                Button("Graphics") { optionsShown = false; model.selectedTab = .overlays }
                    .buttonStyle(GhostButtonStyle())
            }
        }
    }

    private func msStepper(_ title: String, _ value: Binding<Int>) -> some View {
        VStack(alignment: .leading, spacing: 4) {
            Text(title).font(.grotesk(11)).foregroundStyle(Studio.secondary)
            Stepper(value: value, in: 50...2000, step: 50) {
                Text("\(value.wrappedValue) ms").font(.plexMono(11))
            }
        }
    }
}

// ── OUTPUTS ─────────────────────────────────────────────────────────────────

struct FooterOutputsGroup: View {
    @EnvironmentObject var model: AppModel
    @State private var recordShown = false
    @State private var streamShown = false
    @State private var vcamShown = false
    @State private var ndiShown = false

    private var recording: Bool {
        model.recordingStatus == "recording" || model.recordingStatus == "warning"
    }
    private var streaming: Bool { model.streamingDesired }

    var body: some View {
        VStack(alignment: .leading, spacing: 6) {
            FooterStatLabel("Outputs")
            HStack(spacing: 8) {
                SplitOutputButton(
                    name: recording ? "Stop recording" : "Record",
                    optionsName: "Recording output options",
                    help: recording ? "Stop recording"
                        : (model.isoRecordingEnabled
                           ? "Record MP4 + \(model.peekIsoSourceIds().count) ISOs" : "Record MP4"),
                    active: recording, activeFill: Studio.red,
                    flyoutShown: $recordShown,
                    action: { model.toggleRecording() }
                ) {
                    ZStack {
                        if recording {
                            RoundedRectangle(cornerRadius: 2).fill(Studio.textPrimary)
                                .frame(width: 14, height: 14)
                        } else {
                            Circle().stroke(Studio.red, lineWidth: 2).frame(width: 22, height: 22)
                        }
                    }
                    .frame(width: 26, height: 26)
                } flyout: { recordFlyout }

                SplitOutputButton(
                    name: streaming ? "Stop streaming" : "Start streaming",
                    optionsName: "Streaming output options",
                    help: streaming
                        ? "Stop streaming \u{2014} \(model.streamSummary)"
                        : "Start streaming",
                    active: streaming, activeFill: Studio.blue,
                    flyoutShown: $streamShown,
                    action: {
                        // Nothing valid to start: open the options instead of a doomed start.
                        if !streaming && model.streamStartRefusal != nil { streamShown = true }
                        else { model.toggleStreaming() }
                    }
                ) {
                    Image(systemName: "dot.radiowaves.left.and.right")
                        .font(.system(size: 17, weight: .medium))  // design-lint: allow
                        .foregroundStyle(streaming ? Studio.textPrimary : Studio.textPrimary.opacity(0.9))
                } flyout: { streamFlyout }

                SplitOutputButton(
                    name: "Virtual camera",
                    optionsName: "Virtual camera options",
                    help: model.virtualCameraLabel,
                    active: model.virtualCamera.enabled,
                    flyoutShown: $vcamShown,
                    action: { model.setVirtualCamera(enabled: !model.virtualCamera.enabled) }
                ) {
                    Image(systemName: "web.camera")
                        .font(.system(size: 17, weight: .medium))  // design-lint: allow
                        .foregroundStyle(model.virtualCamera.enabled ? Studio.onAccent : Studio.textPrimary)
                } flyout: { VirtualCameraFlyout() }

                SplitOutputButton(
                    name: "NDI output, not available on Mac yet",
                    optionsName: "NDI options",
                    help: StudioFooterReadout.ndiUnavailable,
                    available: false,
                    flyoutShown: $ndiShown,
                    action: nil
                ) {
                    Text("NDI").font(.grotesk(14, .semibold)).foregroundStyle(Studio.textPrimary)
                } flyout: {
                    FooterFlyout(title: "NDI output", width: 280) {
                        FooterMutedText(StudioFooterReadout.ndiUnavailable)
                    }
                }
            }
        }
    }

    private var recordFlyout: some View {
        FooterFlyout(title: "Recording output") {
            FooterMutedText("\(model.recordFormat.uppercased()) \u{00B7} "
                            + "\(model.canvasWidth)\u{00D7}\(model.canvasHeight) "
                            + "\u{00B7} \(model.canvasFps) fps \u{00B7} H.264 (VideoToolbox) "
                            + "\u{00B7} filename \u{201C}\(model.recordPrefix.isEmpty ? "show" : model.recordPrefix)\u{201D}")
            VStack(alignment: .leading, spacing: 6) {
                HStack(alignment: .center) {
                    VStack(alignment: .leading, spacing: 2) {
                        Text("ISO recording").font(.grotesk(13, .semibold))
                            .foregroundStyle(Studio.textPrimary)
                        Text(model.isoRecordingEnabled
                             ? "Program + \(model.peekIsoSourceIds().count) ISOs armed"
                             : "Program only")
                            .font(.grotesk(12)).foregroundStyle(Studio.secondary)
                    }
                    Spacer()
                    Picker("", selection: $model.isoRecordingEnabled) {
                        Text("Program only").tag(false)
                        Text("Program + ISOs").tag(true)
                    }
                    .pickerStyle(.segmented).labelsHidden().fixedSize()
                    .disabled(recording)
                    .help(recording ? "ISO arming is fixed for the take in progress." : "")
                }
                Text("Pick per-source ISOs in Sources \u{2192} Inputs (the ISO checkbox on each "
                     + "Zoom guest / capture device).")
                    .font(.grotesk(11)).foregroundStyle(Studio.secondary)
                    .fixedSize(horizontal: false, vertical: true)
                if !model.recordingWarning.isEmpty {
                    Text(model.recordingWarning).font(.grotesk(11, .semibold))
                        .foregroundStyle(Studio.amber)
                        .fixedSize(horizontal: false, vertical: true)
                }
                Text(model.recordFlyoutFolder)
                    .font(.plexMono(10)).foregroundStyle(Studio.secondary)
                    .textSelection(.enabled)
                    .accessibilityLabel("Recording folder")
                    .help(model.recordFlyoutFolder)
                if !model.recordingFolderWarning.isEmpty {
                    Text(model.recordingFolderWarning).font(.grotesk(11))
                        .foregroundStyle(Studio.amber)
                        .fixedSize(horizontal: false, vertical: true)
                }
            }
            .padding(.horizontal, 10).padding(.vertical, 8)
            .background(RoundedRectangle(cornerRadius: 6).fill(Studio.field))
            HStack {
                Text("Dropped \(model.droppedFramesLabel)")
                    .font(.grotesk(12)).foregroundStyle(Studio.secondary)
                Spacer()
                Button("More") { recordShown = false; model.selectedTab = .settings }
                    .buttonStyle(GhostButtonStyle())
            }
        }
    }

    private var streamFlyout: some View {
        FooterFlyout(title: "Streaming output") {
            FooterMutedText(streaming
                            ? "Streaming \u{2014} \(model.streamSummary)"
                            : (model.streamStartRefusal ?? "Ready to stream."))
            VStack(alignment: .leading, spacing: 8) {
                TextField("RTMP URL", text: $model.streamUrl)
                    .textFieldStyle(StudioFieldStyle())
                SecureField("Stream key (stored in Keychain)", text: $model.streamKey)
                    .textFieldStyle(StudioFieldStyle())
            }
            VStack(alignment: .leading, spacing: 4) {
                Text("Destinations").font(.grotesk(11)).foregroundStyle(Studio.secondary)
                // One row per destination; the status is the core's lifecycle +
                // supervisor (never "live" from a launch). SRT/HLS fields: More.
                DestinationRowsView()
            }
            HStack {
                Text(ProductionSettingsPolicy.streamRecipeLabel(
                    bitrateMbps: model.streamBitrateMbps,
                    height: model.coreOutputProfile?.height ?? model.canvasHeight,
                    encoder: model.streamEncoder))
                    .font(.grotesk(11)).foregroundStyle(Studio.secondary)
                    .fixedSize(horizontal: false, vertical: true)
                Spacer()
                Button("More") { streamShown = false; model.selectedTab = .settings }
                    .buttonStyle(GhostButtonStyle())
            }
            HStack {
                Spacer()
                Button(streaming ? "Stop streaming" : "Start streaming") {
                    model.toggleStreaming()
                    streamShown = false
                }
                .buttonStyle(AccentButtonStyle())
                .disabled(!streaming && model.streamStartRefusal != nil)
            }
        }
    }
}

// ── virtual camera flyout (Windows StudioWorkspace.xaml, V5) ──────────────

/// The virtual camera option flyout. Copy matches Windows verbatim; the status
/// line and the System Settings affordance are the Mac activation layer.
struct VirtualCameraFlyout: View {
    @EnvironmentObject var model: AppModel
    @State private var nameDraft = ""
    @FocusState private var nameFocused: Bool

    var body: some View {
        FooterFlyout(title: "Virtual camera", width: 300) {
            VStack(alignment: .leading, spacing: 4) {
                Toggle("OH Framer (webcam only)", isOn: Binding(
                    get: { model.virtualCamera.framerEnabled },
                    set: { model.setVirtualCameraFramer($0) }))
                    .toggleStyle(StudioToggleStyle())
                    .font(.grotesk(13, .semibold)).foregroundStyle(Studio.textPrimary)
                caption("Framing guides over live video. Program, Preview, recordings and streams stay clean.")
                Text(model.virtualCameraFramerLabel)
                    .font(.grotesk(11)).foregroundStyle(Studio.textPrimary)
                    .fixedSize(horizontal: false, vertical: true)
            }
            VStack(alignment: .leading, spacing: 4) {
                Toggle("Mirror (self-view)", isOn: Binding(
                    get: { model.virtualCamera.mirror },
                    set: { model.setVirtualCameraMirror($0) }))
                    .toggleStyle(StudioToggleStyle())
                    .font(.grotesk(13, .semibold)).foregroundStyle(Studio.textPrimary)
                caption("Flips the camera horizontally, like most webcam self-views.")
            }
            VStack(alignment: .leading, spacing: 4) {
                Text("Camera name").font(.grotesk(11)).foregroundStyle(Studio.secondary)
                // Committed on Return or focus loss, never per keystroke.
                TextField(CameraConstants.defaultDeviceName, text: $nameDraft)
                    .textFieldStyle(StudioFieldStyle())
                    .focused($nameFocused)
                    .onSubmit { commitName() }
                    .onChange(of: nameFocused) { focused in if !focused { commitName() } }
                    .accessibilityLabel("Camera name")
                caption("The name apps show in their camera list. Takes effect next time you enable the camera.")
            }
            VStack(alignment: .leading, spacing: 6) {
                Text(model.virtualCameraLabel)
                    .font(.grotesk(12, .semibold)).foregroundStyle(Studio.textPrimary)
                    .fixedSize(horizontal: false, vertical: true)
                    .accessibilityLabel("Virtual camera status")
                    .accessibilityValue(model.virtualCameraLabel)
                if let notice = model.virtualCameraUpdateNotice {
                    caption(notice)
                }
                if VirtualCameraPresentation.showsUpdateButton(model.cameraExtensionState,
                                                             updating: model.cameraUpdateInProgress) {
                    Button("Update camera") { model.updateCameraExtension() }
                        .buttonStyle(AccentButtonStyle())
                        .accessibilityLabel("Update camera")
                }
                if VirtualCameraPresentation.showsSettingsButton(model.cameraExtensionState) {
                    caption(CameraExtensionPolicy.settingsInstruction(
                        osMajor: ProcessInfo.processInfo.operatingSystemVersion.majorVersion))
                    Button("Open System Settings") { model.openCameraExtensionSettings() }
                        .buttonStyle(AccentButtonStyle())
                }
            }
            .padding(.horizontal, 10).padding(.vertical, 8)
            .frame(maxWidth: .infinity, alignment: .leading)
            .background(RoundedRectangle(cornerRadius: 6).fill(Studio.field))
        }
        .onAppear { nameDraft = model.virtualCamera.deviceName }
        .onDisappear { commitName() }
    }

    private func caption(_ text: String) -> some View {
        Text(text).font(.grotesk(11)).foregroundStyle(Studio.secondary)
            .fixedSize(horizontal: false, vertical: true)
    }

    private func commitName() {
        model.setVirtualCameraDeviceName(nameDraft)  // the model drops an unchanged name
    }
}

// ── TRANSPORT ───────────────────────────────────────────────────────────────

struct FooterTransportGroup: View {
    @EnvironmentObject var model: AppModel
    @State private var transitionsShown = false

    private var takeHelp: String {
        let base = "Take Preview to Program using \(model.takeTransitionLabel), "
            + "then swap the previous Program back to Preview (Space)."
        return model.takeStatus.isEmpty ? base : model.takeStatus + "\n" + base
    }

    var body: some View {
        HStack(spacing: 12) {
            // Last take / transition feedback, in a FIXED slot: text replaces text,
            // the transport never shifts.
            Text(model.takeStatus)
                .font(.grotesk(10)).foregroundStyle(Studio.secondary)
                .lineLimit(2).truncationMode(.tail)
                .multilineTextAlignment(.trailing)
                .frame(width: 150, height: 48, alignment: .trailing)
                .help(model.takeStatus)
            Button {
                Task { await model.take() }
            } label: {
                Text("Take").font(.grotesk(13, .bold))
                    .foregroundStyle(model.canTake ? Studio.textPrimary : Studio.textDim)
                    .frame(width: 84, height: 48)
                    .background(RoundedRectangle(cornerRadius: 8)
                        .fill(model.canTake ? Studio.amber.opacity(0.10) : Studio.surface))
                    .overlay(RoundedRectangle(cornerRadius: 8)
                        .stroke(model.canTake ? Studio.amber : Studio.border, lineWidth: 1))
                    .contentShape(Rectangle())
            }
            .buttonStyle(.plain)
            .disabled(!model.canTake)
            .accessibilityLabel("Take")
            .help(takeHelp)

            Button { transitionsShown.toggle() } label: {
                HStack(spacing: 6) {
                    Text(model.takeTransitionLabel).font(.grotesk(12, .semibold))
                        .foregroundStyle(Studio.textPrimary)
                    Text(StudioFooterReadout.transitionDuration(model.takeTransitionMode))
                        .font(.grotesk(11)).foregroundStyle(Studio.secondary)
                    Spacer(minLength: 0)
                    FooterChevron()
                }
                .padding(.horizontal, 12)
                .frame(width: 128, height: 48)
                .background(RoundedRectangle(cornerRadius: 8).fill(Studio.surface))
                .overlay(RoundedRectangle(cornerRadius: 8).stroke(Studio.border, lineWidth: 1))
                .contentShape(Rectangle())
            }
            .buttonStyle(.plain)
            .accessibilityLabel("Choose Take transition")
            .accessibilityValue(model.takeTransitionLabel)
            .help("Choose Take transition. Cut is instant; Fade, Dip and Wipe run 300 ms.")
            .popover(isPresented: $transitionsShown, arrowEdge: .top) {
                VStack(alignment: .leading, spacing: 2) {
                    ForEach(StudioFooterReadout.transitions, id: \.mode) { option in
                        Button {
                            model.setTakeTransition(option.mode)
                            transitionsShown = false
                        } label: {
                            HStack(spacing: 8) {
                                Image(systemName: "checkmark")
                                    .font(.system(size: 10, weight: .bold))  // design-lint: allow
                                    .foregroundStyle(Studio.accent)
                                    .opacity(model.takeTransitionMode == option.mode ? 1 : 0)
                                Text(option.label).font(.grotesk(13, .medium))
                                    .foregroundStyle(Studio.textPrimary)
                                Spacer()
                                Text(StudioFooterReadout.transitionDuration(option.mode))
                                    .font(.grotesk(11)).foregroundStyle(Studio.secondary)
                            }
                            .padding(.horizontal, 10).padding(.vertical, 7)
                            .contentShape(Rectangle())
                        }
                        .buttonStyle(.plain)
                    }
                }
                .padding(6)
                .frame(width: 180)
                .background(Studio.panel)
            }

            // Fade to Black — RC key: black fill, no visible text, red outline when
            // black is selected. The label lives in help/accessibility only.
            Button {
                Task { await model.toggleProgramBlackout() }
            } label: {
                RoundedRectangle(cornerRadius: 8)
                    .fill(Color.black)
                    .frame(width: 80, height: 48)
                    .overlay(RoundedRectangle(cornerRadius: 8)
                        .stroke(model.programBlackout ? Studio.red : Studio.border,
                                lineWidth: 2))
                    .contentShape(Rectangle())
            }
            .buttonStyle(.plain)
            .help("\(model.programBlackoutLabel) (\u{2318}\u{21E7}B). Fades Program video over 300 ms; audio continues.")
            .accessibilityLabel(model.programBlackoutLabel)
        }
        .padding(.leading, 14)
        .overlay(Rectangle().frame(width: 1).foregroundStyle(Studio.border), alignment: .leading)
    }
}

// ── telemetry row ───────────────────────────────────────────────────────────

/// One fixed-width telemetry column: label, value, health. Every line always
/// exists (an empty string keeps its height), so no state change resizes it.
struct FooterStat: View {
    let title: String
    let value: String
    var valueColor: Color = Studio.textPrimary
    let health: String
    var healthColor: Color = Studio.secondary
    let width: CGFloat

    var body: some View {
        VStack(alignment: .leading, spacing: 3) {
            FooterStatLabel(title)
            Text(value).font(.plexMono(12, .medium)).foregroundStyle(valueColor)
                .lineLimit(1).truncationMode(.tail)
            Text(health.isEmpty ? " " : health).font(.grotesk(10)).foregroundStyle(healthColor)
                .lineLimit(1).truncationMode(.tail)
                .help(health)
        }
        .frame(width: width, alignment: .leading)
    }
}

struct FooterTelemetry: View {
    @EnvironmentObject var model: AppModel

    private var recording: Bool {
        model.recordingStatus == "recording" || model.recordingStatus == "warning"
    }

    var body: some View {
        HStack(alignment: .top, spacing: 16) {
            FooterStat(title: "Program",
                       value: model.coreCanvasLabel,
                       health: model.programSceneId.isEmpty ? "Idle" : "Live",
                       healthColor: model.programSceneId.isEmpty ? Studio.secondary : Studio.accent,
                       width: 110)
            // Dropped frames: amber the moment ANY frame is lost — a silent "0" is
            // the only acceptable healthy state, so a non-zero count must draw the eye.
            FooterStat(title: "Dropped",
                       value: recording || model.recordingDroppedFrames > 0
                           ? model.droppedFramesLabel : "\u{2014}",
                       valueColor: model.recordingDroppedFrames > 0 ? Studio.amber : Studio.textPrimary,
                       health: "",
                       width: 80)
            VStack(alignment: .leading, spacing: 3) {
                FooterStatLabel("Feeds")
                // Windows MaxLines=2; both lines are reserved so a longer summary
                // never changes the row height.
                Text(StudioFooterReadout.setupSummary(slots: model.slots, guests: model.roster.count))
                    .font(.grotesk(12)).foregroundStyle(Studio.textPrimary)
                    .lineLimit(2, reservesSpace: true).truncationMode(.tail)
                    .fixedSize(horizontal: false, vertical: true)
                // #1074: two facts, never a cause. The line's slot is always there.
                Text(model.stalledFeedsLine ?? " ")
                    .font(.grotesk(10, .semibold)).foregroundStyle(Studio.amber)
                    .lineLimit(1).truncationMode(.tail)
                    .help(model.stalledFeedsLine ?? "")
                    .accessibilityLabel(model.stalledFeedsLine.map { "Stream health: \($0)" } ?? "")
            }
            .frame(width: 130, alignment: .leading)
            if recording, let started = model.recordingStartedAt {
                TimelineView(.periodic(from: started, by: 1)) { context in
                    FooterStat(title: "Record",
                               value: elapsedString(from: started, to: context.date),
                               health: model.recordingStatus == "warning" ? "Warning" : "Recording",
                               healthColor: model.recordingStatus == "warning" ? Studio.amber : Studio.red,
                               width: 100)
                }
                .frame(width: 100)
                .accessibilityLabel("Recording, live elapsed")
            } else {
                FooterStat(title: "Record", value: "\u{2014}", health: "Idle", width: 100)
            }
            FooterStat(title: "Stream",
                       value: model.streamingDesired ? (model.streamDegradationLine ?? "6 Mbps") : "\u{2014}",
                       valueColor: model.streamingDesired && model.streamDegradationLine != nil
                           ? Studio.amber : Studio.textPrimary,
                       health: model.streamingDesired ? model.streamHealthLabel : "Idle",
                       healthColor: model.streamingDesired ? Studio.blue : Studio.secondary,
                       width: 100)
            FooterMasterCard()
                .frame(minWidth: 280, maxWidth: .infinity)
            FooterGhostButton(title: model.monitorEnabled ? "Monitor on" : "Monitor",
                              lit: model.monitorEnabled) {
                model.setMonitor(enabled: !model.monitorEnabled, volume: model.monitorVolume)
            }
            .frame(width: 100)
            .accessibilityLabel("Monitor")
            .accessibilityValue(model.monitorEnabled ? "on" : "off")
            .help(model.monitorStatus.isEmpty ? "Listen to Program on the monitor output" : model.monitorStatus)
            FooterGhostButton(title: "Audio mixer") { model.selectedTab = .audio }
                .frame(width: 112)
                .help("Open the Audio mixer")
            FooterGhostButton(title: "Health") { model.selectedTab = .diagnose }
                .frame(width: 86)
                .help("Open Health")
        }
        .padding(.horizontal, 16)
        .padding(.vertical, 12)
        .frame(minHeight: 54)
    }

    private func elapsedString(from start: Date, to now: Date) -> String {
        let seconds = max(0, Int(now.timeIntervalSince(start)))
        return String(format: "%02d:%02d", seconds / 60, seconds % 60)
    }
}

struct FooterGhostButton: View {
    let title: String
    var lit = false
    let action: () -> Void

    var body: some View {
        Button(action: action) {
            Text(title).font(.grotesk(13, .semibold))
                .foregroundStyle(lit ? Studio.onAccent : Studio.textPrimary)
                .lineLimit(1)
                .frame(maxWidth: .infinity, minHeight: 36, maxHeight: 36)
                .background(RoundedRectangle(cornerRadius: 8)
                    .fill(lit ? Studio.accent : Studio.surface))
                .overlay(RoundedRectangle(cornerRadius: 8)
                    .stroke(lit ? Studio.accent : Studio.border, lineWidth: 1))
                .contentShape(Rectangle())
        }
        .buttonStyle(.plain)
    }
}

/// MASTER: L and R meters, then LUFS / true peak / status in a FIXED three-line
/// column. The meter-age warning occupies the status line in place.
struct FooterMasterCard: View {
    @EnvironmentObject var model: AppModel

    var body: some View {
        HStack(alignment: .center, spacing: 10) {
            VStack(alignment: .leading, spacing: 4) {
                Text("MASTER").font(.plexMono(10, .bold)).tracking(10 * 0.06)
                    .foregroundStyle(Studio.accent)
                meterRow("L", model.masterLevelLeft)
                meterRow("R", model.masterLevelRight)
            }
            TimelineView(.periodic(from: .now, by: 0.25)) { context in
                let warning = model.meterWarning(now: context.date)
                VStack(alignment: .trailing, spacing: 2) {
                    Text(StudioFooterReadout.lufsLabel(model.shortTermLufs))
                        .font(.plexMono(12, .semibold)).foregroundStyle(Studio.textPrimary)
                    Text(StudioFooterReadout.truePeakLabel(model.truePeakDbfs))
                        .font(.plexMono(10)).foregroundStyle(Studio.secondary)
                    Text(StudioFooterReadout.masterStatusLine(warning: warning,
                                                               limiterActive: model.limiterActive))
                        .font(.plexMono(10, warning == nil ? .regular : .bold))
                        .foregroundStyle(warning == nil ? Studio.secondary : Studio.amber)
                }
                .lineLimit(1)
                .frame(width: 150, alignment: .trailing)
            }
            .frame(width: 150)
        }
        .padding(.horizontal, 10).padding(.vertical, 7)
        .overlay(Rectangle().frame(width: 1).foregroundStyle(Studio.border), alignment: .leading)
    }

    private func meterRow(_ side: String, _ level: Int) -> some View {
        HStack(spacing: 7) {
            Text(side).font(.plexMono(9, .bold)).foregroundStyle(Studio.secondary)
                .frame(width: 12, alignment: .leading)
            MeterBar(level: level)
        }
    }
}

// ── offscreen proof shots ───────────────────────────────────────────────────

/// COREVIDEO_SHELL_FOOTER_SNAPSHOT=<dir>: renders the footer OFFSCREEN with
/// ImageRenderer (no window, no core, no desktop capture) at a normal width, the
/// window's minimum width and a width narrow enough to wrap, idle and live, then
/// exits. Popovers are not rendered — they are separate windows.
@MainActor
enum FooterSnapshotHarness {
    static func seedLive(_ model: AppModel, now: Date = Date()) {
        model.recordingStatus = "recording"
        model.recordingStartedAt = now.addingTimeInterval(-754)
        model.recordingFramesWritten = 45_000
        model.recordingDroppedFrames = 3
        model.streamingDesired = true
        model.streamStatus = "live"
        model.streamKey = "snapshot-harness-key"
        model.destinationRows = ["rtmp": DestinationStatus(label: "Live", tone: .good, isLive: true)]
        model.lowerThirdName = "Jordan Wallace"
        model.lowerThirdKeyedName = "Jordan Wallace"
        model.lowerThirdPhase = "on-air"
        model.lowerThirdVisible = true
        model.programSceneId = "fullscreen"
        model.shortTermLufs = -16.4
        model.truePeakDbfs = -3.2
        model.masterLevelLeft = 72
        model.masterLevelRight = 66
        model.takeStatus = "Take transition set to Fade"
        ShowInputWriteScope.run("snapshot-seed") {
            model.slots[0].kind = "zoom"; model.slots[0].sourceId = "1"; model.slots[0].inShow = true
            model.slots[1].kind = "capture"; model.slots[1].sourceId = "c"; model.slots[1].inShow = true
        }
        model.roster = (1...4).map {
            RosterParticipant(id: "\($0)", name: "Guest \($0)", hasVideo: true,
                              muted: false, talking: false, assigned: $0 == 1)
        }
        model.observeAudioObservation(
            ["audioObservation": ["sequence": 4, "ageMs": 20, "status": "fresh"]], now: now)
    }

    static func render(_ model: AppModel, width: CGFloat, to url: URL) -> CGSize? {
        let renderer = ImageRenderer(content:
            StudioFooter().environmentObject(model)
                .frame(width: width)
                .background(Studio.background)
                .environment(\.colorScheme, .dark))
        renderer.scale = 2
        guard let image = renderer.nsImage,
              let tiff = image.tiffRepresentation,
              let bitmap = NSBitmapImageRep(data: tiff),
              let png = bitmap.representation(using: .png, properties: [:])
        else { return nil }
        try? png.write(to: url)
        return image.size
    }

    static func runAndExit(dir: String) -> Never {
        try? FileManager.default.createDirectory(atPath: dir, withIntermediateDirectories: true)
        let idle = AppModel()
        let live = AppModel()
        seedLive(live)
        for (name, model) in [("idle", idle), ("live", live)] {
            for (label, width) in [("normal-1600", 1600.0), ("min-1360", 1360.0), ("narrow-980", 980.0)] {
                let url = URL(fileURLWithPath: dir).appendingPathComponent("footer-\(name)-\(label).png")
                let size = render(model, width: CGFloat(width), to: url)
                print("footer snapshot \(url.path) size=\(size.map { "\($0.width)x\($0.height)" } ?? "FAILED")")
            }
        }
        exit(0)
    }
}
