// Audio tab — composition per docs/design-reference/06-audio.png: the
// SHOW / ROUTING / SETUP pills, the channel processing workspace (GATE /
// COMP / EQ cells with response curves), pan + MUTE/S + fader strips, and
// the MASTER LUFS rail with the Monitor toggle. Sliders/toggles deliberately
// keep the stock blue accent — the Windows build has the same documented
// inconsistency and the handoff says match the reference, don't unify.
//
// Wiring: gain/pan/mute/solo AND the GATE/COMP/EQ inserts all ride
// sync-participant-audio-mix. The insert names and parameter keys are the
// core's exact contract (AudioDsp.h applyChannelInserts + the Windows
// BuiltInInsertNames) — an insert only processes when its name is in
// pluginInserts, and unknown keys are silently ignored, so both must match.
// Each cell's IN badge is the enable toggle.

import SwiftUI

struct AudioPane: View {
    @Environment(\.openWindow) private var openWindow
    // The pop-out window hosts this same view; it just hides the pop-out button.
    var isPopout: Bool = false
    @EnvironmentObject var model: AppModel
    @State private var surface = AudioPane.initialSurface
    /// The offscreen tab renderer (COREVIDEO_SHELL_TAB_SNAPSHOT) may open another
    /// surface with COREVIDEO_SHELL_AUDIO_SURFACE=ROUTING|SETUP|PLUG-INS; the
    /// running app always opens on SHOW.
    static var initialSurface: String {
        guard ShellHarness.isActive,
              let wanted = ProcessInfo.processInfo.environment["COREVIDEO_SHELL_AUDIO_SURFACE"],
              ["SHOW", "ROUTING", "PLUG-INS", "SETUP"].contains(wanted) else { return "SHOW" }
        return wanted
    }
    @State private var selectedStripId: String?

    var selected: AudioStrip? {
        model.strips.first { $0.id == (selectedStripId ?? model.strips.first?.id) }
    }

    var body: some View {
        VStack(alignment: .leading, spacing: 10) {
            HStack(spacing: 8) {
                ForEach(["SHOW", "ROUTING", "PLUG-INS", "SETUP"], id: \.self) { name in
                    Button(name) { surface = name }
                        .buttonStyle(.plain)
                        .font(.plexMono(10, .semibold))
                        .foregroundStyle(surface == name ? Studio.onAccent : Studio.secondary)
                        .padding(.horizontal, 10).padding(.vertical, 5)
                        .background(RoundedRectangle(cornerRadius: 6)
                            .fill(surface == name ? Studio.blue : Studio.surface))
                }
                Spacer()
                // Real second-display mixer (was a disabled placeholder). Hidden
                // inside the pop-out so it cannot spawn copies of itself.
                if !isPopout {
                    Button("Pop out mixer") { openWindow(id: PopoutWindow.audioMixer) }
                        .buttonStyle(GhostButtonStyle())
                        .help("Open the mixer on a second display")
                }
            }
            switch surface {
            case "ROUTING":
                AudioRoutingSurface()
            case "PLUG-INS":
                VstInsertsSurface()
                    .padding(.top, 2)
            case "SETUP":
                VStack(alignment: .leading, spacing: 12) {
                    MasteringGuideMeters()
                    MasteringRack()
                    Text("Monitor output").font(.grotesk(13, .semibold))
                    MonitorDevicePicker()
                    Toggle("Monitor", isOn: Binding(
                        get: { model.monitorEnabled },
                        set: { model.setMonitor(enabled: $0, volume: model.monitorVolume) }))
                    HStack {
                        Text("Volume").font(.grotesk(12))
                        Slider(value: Binding(
                            get: { model.monitorVolume },
                            set: {
                                model.setMonitor(enabled: model.monitorEnabled, volume: $0)
                            }), in: 0...1)
                            .frame(width: 220)
                        if !model.monitorStatus.isEmpty {
                            Text(model.monitorStatus).font(.plexMono(10))
                                .foregroundStyle(Studio.secondary)
                        }
                    }
                    if !model.monitorControlNotice.isEmpty {
                        Text(model.monitorControlNotice).font(.plexMono(10))
                            .foregroundStyle(Studio.amber)
                    }
                    if model.monitorFeedbackRisk {
                        Text("Monitor output matches the loopback device — feedback risk")
                            .font(.grotesk(12)).foregroundStyle(Studio.amber)
                    }
                }
                .padding(10)
            default:
                showSurface
            }
        }
        .padding(14)
        .background(RoundedRectangle(cornerRadius: 10).fill(Studio.panel))
        .overlay(RoundedRectangle(cornerRadius: 10).stroke(Studio.border, lineWidth: 1))
        .frame(maxWidth: 1000)
    }

    var showSurface: some View {
        VStack(alignment: .leading, spacing: 10) {
            HStack(spacing: 6) {
                ForEach(model.strips) { strip in
                    Button {
                        selectedStripId = strip.id
                    } label: {
                        VStack(alignment: .leading, spacing: 0) {
                            Text(stripLabel(strip)).font(.grotesk(11, .medium))
                                .lineLimit(1)
                            MonoLabel("Master", dim: true)
                        }
                        .padding(.horizontal, 8).padding(.vertical, 4)
                        .background(RoundedRectangle(cornerRadius: 6)
                            .fill(selected?.id == strip.id
                                  ? Studio.surfaceRaised : Studio.surface))
                        .overlay(RoundedRectangle(cornerRadius: 6)
                            .stroke(selected?.id == strip.id
                                    ? Studio.accent.opacity(0.5) : Studio.border,
                                    lineWidth: 1))
                    }
                    .buttonStyle(.plain)
                }
                if model.strips.isEmpty {
                    Text("Channels appear once sources carry PCM.")
                        .font(.grotesk(12)).foregroundStyle(Studio.secondary)
                }
            }
            if let strip = selected {
                ProcessingWorkspace(stripId: strip.id, stripName: stripLabel(strip))
            }
            HStack(alignment: .top, spacing: 10) {
                ForEach(model.strips) { strip in
                    ConsoleStrip(strip: strip, label: stripLabel(strip))
                }
                Spacer()
                MasterLufsRail()
            }
        }
    }

    func stripLabel(_ strip: AudioStrip) -> String { model.audioSourceLabel(strip.id) }
}

// GATE / COMP / EQ cells with drawn response curves (composition per the
// reference; parameter state is local until the chain's key names are
// verified — see the header note).
struct ProcessingWorkspace: View {
    @EnvironmentObject var model: AppModel
    let stripId: String
    let stripName: String

    let bandLabels = ["63", "125", "250", "500", "1k", "2k", "4k", "8k"]

    var inserts: ChannelInserts { model.channelInserts[stripId] ?? ChannelInserts() }

    func bind(_ keyPath: WritableKeyPath<ChannelInserts, Double>) -> Binding<Double> {
        Binding(
            get: { inserts[keyPath: keyPath] },
            set: { value in model.editInserts(stripId) { $0[keyPath: keyPath] = value } })
    }

    var body: some View {
        HStack(alignment: .top, spacing: 10) {
            VStack(alignment: .leading, spacing: 6) {
                MonoLabel("Channel")
                Text(stripName).font(.grotesk(13, .semibold)).lineLimit(2)
                Spacer()
                // Real VST3 insert selection (was a permanently disabled
                // placeholder). The control disables itself — with the reason —
                // whenever the isolated host cannot actually run an insert; see
                // VstInserts.swift for what does and does not work on macOS.
                VstChannelInsertControl(stripId: stripId)
            }
            .frame(width: 150, alignment: .leading)
            cell("GATE", on: inserts.gateEnabled,
                 toggle: { model.editInserts(stripId) { $0.gateEnabled.toggle() } }) {
                CurveCanvas { size in
                    var path = Path()
                    let knee = (inserts.gateThresholdDb + 80) / 68 * size.width * 0.8
                    path.move(to: CGPoint(x: 0, y: size.height * 0.95))
                    path.addLine(to: CGPoint(x: knee, y: size.height * 0.95))
                    path.addLine(to: CGPoint(x: knee + 14, y: size.height * 0.35))
                    path.addLine(to: CGPoint(x: size.width, y: 0))
                    return path
                }
                HStack(spacing: 6) {
                    labeledSlider("Threshold (dB)", bind(\.gateThresholdDb), in: -80...(-12))
                    labeledSlider("Range (dB)", bind(\.gateRangeDb), in: -80...(-6))
                }
                HStack(spacing: 6) {
                    labeledSlider("Attack", bind(\.gateAttackMs), in: 0.5...50)
                    labeledSlider("Hold", bind(\.gateHoldMs), in: 0...500)
                    labeledSlider("Release", bind(\.gateReleaseMs), in: 20...500)
                }
            }
            cell("COMP", on: inserts.compEnabled,
                 toggle: { model.editInserts(stripId) { $0.compEnabled.toggle() } }) {
                CurveCanvas { size in
                    var path = Path()
                    let knee = (inserts.compThresholdDb + 40) / 34 * size.width
                    path.move(to: CGPoint(x: 0, y: size.height))
                    path.addLine(to: CGPoint(x: knee, y: size.height - knee))
                    let over = size.width - knee
                    path.addLine(to: CGPoint(
                        x: size.width,
                        y: size.height - knee - over / max(1, inserts.compRatio)))
                    return path
                }
                HStack(spacing: 6) {
                    labeledSlider("Threshold (dB)", bind(\.compThresholdDb), in: -40...(-6))
                    labeledSlider("Ratio", bind(\.compRatio), in: 1...20)
                }
                let gr = MasterMeterReadout.gainReduction(
                    model.strips.first { $0.id == stripId }?.gainReductionDb ?? 0)
                HStack(spacing: 6) {
                    Text(gr.label).font(.plexMono(9)).foregroundStyle(Studio.secondary)
                        .frame(width: 70, alignment: .leading)
                    GainReductionBar(level: gr.level).frame(height: 6)
                }
                HStack(spacing: 6) {
                    labeledSlider("Attack", bind(\.compAttackMs), in: 0.1...100)
                    labeledSlider("Release", bind(\.compReleaseMs), in: 10...1000)
                    labeledSlider("Knee", bind(\.compKneeDb), in: 0...24)
                    labeledSlider("Makeup", bind(\.compMakeupDb), in: 0...24)
                }
            }
            cell("EQ", on: inserts.eqEnabled,
                 toggle: { model.editInserts(stripId) { $0.eqEnabled.toggle() } }) {
                CurveCanvas { size in
                    var path = Path()
                    let bands = inserts.eqBandsDb
                    let step = size.width / CGFloat(max(1, bands.count - 1))
                    for (index, band) in bands.enumerated() {
                        let point = CGPoint(
                            x: CGFloat(index) * step,
                            y: size.height / 2 - CGFloat(band / 12) * size.height / 2)
                        if index == 0 { path.move(to: point) } else { path.addLine(to: point) }
                    }
                    return path
                }
                HStack(alignment: .bottom, spacing: 6) {
                    VStack(spacing: 1) {
                        MonoLabel("Low cut", dim: true)
                        Slider(value: bind(\.eqHighpassHz), in: 20...400).frame(width: 74)
                        Text("\(Int(inserts.eqHighpassHz)) Hz")
                            .font(.plexMono(8)).foregroundStyle(Studio.textDim)
                    }
                    ForEach(0..<8, id: \.self) { index in
                        VStack(spacing: 2) {
                            Slider(value: Binding(
                                get: { inserts.eqBandsDb[index] },
                                set: { value in
                                    model.editInserts(stripId) { $0.eqBandsDb[index] = value }
                                }), in: -12...12)
                                .frame(width: 60)
                                .rotationEffect(.degrees(-90))
                                .frame(width: 18, height: 60)
                            Text(bandLabels[index]).font(.plexMono(8))
                                .foregroundStyle(Studio.textDim)
                        }
                    }
                }
            }
        }
        .frame(height: 220)
    }

    func cell(_ title: String, on: Bool, toggle: @escaping () -> Void,
              @ViewBuilder content: () -> some View) -> some View {
        VStack(alignment: .leading, spacing: 6) {
            HStack {
                MonoLabel(title)
                Spacer()
                Button("IN", action: toggle)
                    .buttonStyle(.plain)
                    .font(.plexMono(9, .semibold))
                    .padding(.horizontal, 5).padding(.vertical, 1)
                    .background(RoundedRectangle(cornerRadius: 3)
                        .fill(on ? Studio.accent.opacity(0.22) : Studio.surface))
                    .foregroundStyle(on ? Studio.accent : Studio.textDim)
            }
            content()
        }
        .padding(8)
        .frame(maxWidth: .infinity, maxHeight: .infinity, alignment: .topLeading)
        .background(RoundedRectangle(cornerRadius: 8).fill(Studio.field))
        .overlay(RoundedRectangle(cornerRadius: 8)
            .stroke(on ? Studio.accent.opacity(0.4) : Studio.border, lineWidth: 1))
        .opacity(on ? 1 : 0.55)
    }

    func labeledSlider(_ label: String, _ value: Binding<Double>,
                       in range: ClosedRange<Double>) -> some View {
        VStack(alignment: .leading, spacing: 0) {
            Text(label).font(.plexMono(9)).foregroundStyle(Studio.secondary)
            Slider(value: value, in: range)
        }
    }
}

struct CurveCanvas: View {
    let makePath: (CGSize) -> Path

    var body: some View {
        Canvas { context, size in
            context.stroke(makePath(size), with: .color(Studio.secondary.opacity(0.8)),
                           lineWidth: 1.5)
        }
        .frame(height: 70)
        .background(Rectangle().fill(Color.black.opacity(0.35)))
        .overlay(Rectangle().stroke(Studio.line2, lineWidth: 1))
    }
}

// Fader strip: PAN on top, MUTE/S, dB readout, vertical fader beside the
// segmented meter, name chip at the bottom (reference strip bank).
struct ConsoleStrip: View {
    @EnvironmentObject var model: AppModel
    let strip: AudioStrip
    let label: String

    var body: some View {
        VStack(spacing: 6) {
            MonoLabel("Pan", dim: true)
            Slider(value: Binding(
                get: { strip.pan },
                set: { value in model.editStrip(strip.id) { $0.pan = value } }),
                in: -1...1)
                .frame(width: 90)
            HStack(spacing: 4) {
                Button("MUTE") {
                    model.editStrip(strip.id) { $0.muted.toggle() }
                }
                .buttonStyle(.plain)
                .font(.plexMono(9, .semibold))
                .padding(.horizontal, 7).padding(.vertical, 3)
                .background(RoundedRectangle(cornerRadius: 4)
                    .fill(strip.muted ? Studio.red : Studio.surface))
                .foregroundStyle(strip.muted ? .white : Studio.secondary)
                Button("S") {
                    model.editStrip(strip.id) { $0.solo.toggle() }
                }
                .buttonStyle(.plain)
                .font(.plexMono(9, .semibold))
                .padding(.horizontal, 7).padding(.vertical, 3)
                .background(RoundedRectangle(cornerRadius: 4)
                    .fill(strip.solo ? Studio.amber : Studio.surface))
                .foregroundStyle(strip.solo ? .black : Studio.secondary)
            }
            Text(String(format: "%.1f dB", strip.manualGainDb))
                .font(.plexMono(10)).foregroundStyle(Studio.secondary)
            HStack(spacing: 5) {
                Slider(value: Binding(
                    get: { strip.manualGainDb },
                    set: { value in model.editStrip(strip.id) { $0.manualGainDb = value } }),
                    in: -24...24)
                    .frame(width: 150)
                    .rotationEffect(.degrees(-90))
                    .frame(width: 26, height: 150)
                let reading = AudioConsolePolicy.meterReading(strip)
                SegmentedMeter(level: reading.level, dimmed: reading.dimmed)
                    .frame(width: 12, height: 150)
                MeterTickScale().frame(width: 18, height: 150)
            }
            if strip.sourceMuted {
                // Display only (#481): the guest muted themselves in Zoom.
                Text("ZOOM MUTED").font(.plexMono(9, .semibold)).foregroundStyle(Studio.amber)
                    .accessibilityLabel("\(label) is muted in Zoom")
            }
            Text(label).font(.grotesk(10, .medium))
                .lineLimit(1).truncationMode(.tail)
                .padding(.horizontal, 8).padding(.vertical, 3)
                .background(RoundedRectangle(cornerRadius: 4).fill(Studio.surface))
                .frame(maxWidth: 110)
        }
        .padding(8)
        .background(RoundedRectangle(cornerRadius: 8).fill(Studio.field))
        .overlay(RoundedRectangle(cornerRadius: 8).stroke(Studio.border, lineWidth: 1))
    }
}

struct MasterLufsRail: View {
    @EnvironmentObject var model: AppModel

    /// Windows MasterLufsLabel: the core's loudnessLufs (short-term, else momentary).
    var lufsLabel: String {
        MasterMeterReadout.railLufsLabel(loudnessLufs: model.loudnessLufs, mixedFrames: model.mixedFrameCount)
    }

    var body: some View {
        VStack(spacing: 8) {
            MonoLabel("Master")
            Text(lufsLabel).font(.grotesk(22, .semibold))
                .accessibilityLabel("Master loudness \(lufsLabel)")
            // #870: the clock re-evaluates the observation age between snapshots,
            // so a stalled audio worker cannot leave held meters looking live.
            TimelineView(.periodic(from: .now, by: 0.25)) { context in
                if let warning = model.meterWarning(now: context.date) {
                    Text(warning).font(.grotesk(11, .medium)).foregroundStyle(Studio.amber)
                        .multilineTextAlignment(.center)
                } else {
                    Text(model.masterLevel > 0 ? "Live" : "Standing by")
                        .font(.grotesk(11)).foregroundStyle(Studio.secondary)
                }
            }
            // L and R from the pgm-l / pgm-r bus taps (StudioFooterReadout.masterLevels,
            // Windows ResolveMasterMeterReadout) — no longer the mono level twice.
            HStack(spacing: 4) {
                MeterTickScale().frame(width: 20, height: 170)
                SegmentedMeter(level: model.masterLevelLeft).frame(width: 12, height: 170)
                    .accessibilityLabel("Master left")
                SegmentedMeter(level: model.masterLevelRight).frame(width: 12, height: 170)
                    .accessibilityLabel("Master right")
            }
            Toggle("Monitor", isOn: Binding(
                get: { model.monitorEnabled },
                set: { model.setMonitor(enabled: $0, volume: model.monitorVolume) }))
                .toggleStyle(.switch)
                .controlSize(.small)
            Slider(value: Binding(
                get: { model.monitorVolume },
                set: { model.setMonitor(enabled: model.monitorEnabled, volume: $0) }),
                in: 0...1)
                .frame(width: 110)
            Text("\(Int(model.monitorVolume * 100))%")
                .font(.plexMono(9)).foregroundStyle(Studio.textDim)
            Text("L / R").font(.plexMono(10, .semibold))
                .frame(maxWidth: .infinity)
                .padding(.vertical, 4)
                .background(RoundedRectangle(cornerRadius: 4)
                    .fill(Studio.blue.opacity(0.25)))
        }
        .padding(10)
        .frame(width: 160)
        .background(RoundedRectangle(cornerRadius: 8).fill(Studio.field))
        .overlay(RoundedRectangle(cornerRadius: 8).stroke(Studio.border, lineWidth: 1))
    }
}

/// The Windows AudioMeterScale major ticks beside a 0..100 meter.
struct MeterTickScale: View {
    var body: some View {
        GeometryReader { geo in
            ForEach(AudioMeterScale.majorTicksDbfs, id: \.self) { tick in
                Text(tick == 0 ? "0" : String(Int(tick)))
                    .font(.plexMono(7)).foregroundStyle(Studio.textDim)
                    .position(x: geo.size.width / 2,
                              y: geo.size.height * (1 - CGFloat(AudioMeterScale.toLevel(tick + 0.0001)) / 100))
            }
        }
    }
}

/// Compressor gain reduction, 0..24 dB, filled from the left.
struct GainReductionBar: View {
    let level: Int
    var body: some View {
        GeometryReader { geo in
            ZStack(alignment: .leading) {
                Rectangle().fill(Studio.surfaceRaised)
                Rectangle().fill(Studio.amber).frame(width: geo.size.width * CGFloat(level) / 100)
            }
        }
    }
}

/// SETUP guide meters (Windows AudioPage.xaml:349-380, MasteringGuideMeter):
/// integrated loudness against the target, true peak against the ceiling.
/// Measured post-master values only; no evidence reads a dash.
struct MasteringGuideMeters: View {
    @EnvironmentObject var model: AppModel

    var body: some View {
        HStack(spacing: 16) {
            meter(title: "INTEGRATED · POST-MASTER",
                  value: model.integratedLufs, min: -36, max: 0,
                  guide: model.mastering.targetLufs, warnAbove: false,
                  label: MasterMeterReadout.integratedLabel(model.integratedLufs),
                  guideLabel: MasterMeterReadout.targetLabel(model.mastering.targetLufs))
            meter(title: "TRUE PEAK · POST-MASTER",
                  value: model.truePeakDbfs, min: -24, max: 0,
                  guide: model.mastering.ceilingDbfs, warnAbove: true,
                  label: MasterMeterReadout.truePeakLabel(model.truePeakDbfs),
                  guideLabel: MasterMeterReadout.ceilingLabel(model.mastering.ceilingDbfs))
        }
    }

    func meter(title: String, value: Double, min: Double, max: Double, guide: Double,
               warnAbove: Bool, label: String, guideLabel: String) -> some View {
        let engaged = model.mastering.enabled
        let fraction = MasterMeterReadout.guideFraction(value: value, min: min, max: max)
        let guideFraction = MasterMeterReadout.guideFraction(value: guide, min: min, max: max)
        let fill: Color = !engaged ? Studio.textDim
            : (warnAbove && value > guide + 0.05) ? Studio.red : Studio.accent
        return VStack(alignment: .leading, spacing: 4) {
            MonoLabel(title, dim: !engaged)
            Text(label).font(.grotesk(20, .semibold))
            GeometryReader { geo in
                ZStack(alignment: .leading) {
                    Rectangle().fill(Studio.surfaceRaised)
                    Rectangle().fill(fill).frame(width: geo.size.width * fraction)
                    Rectangle().fill(Studio.textPrimary.opacity(engaged ? 1 : 0.55))
                        .frame(width: 2).offset(x: geo.size.width * guideFraction - 1)
                }
            }
            .frame(height: 10)
            Text(guideLabel).font(.plexMono(9)).foregroundStyle(Studio.secondary)
        }
        .frame(maxWidth: 300)
        .accessibilityElement(children: .combine)
    }
}

struct SegmentedMeter: View {
    let level: Int  // 0..100
    /// Dimmed = a muted strip's PRE-MUTE input (#481): visible, but plainly not
    /// reaching any bus.
    var dimmed = false

    var body: some View {
        Canvas { context, size in
            let segments = 24
            let gap: CGFloat = 2
            let height = (size.height - gap * CGFloat(segments - 1)) / CGFloat(segments)
            for index in 0..<segments {
                let threshold = Double(segments - index) / Double(segments) * 100
                let rect = CGRect(
                    x: 0, y: CGFloat(index) * (height + gap),
                    width: size.width, height: height)
                let lit = Double(level) >= threshold
                let hot: Color = threshold > 90 ? Studio.red
                    : threshold > 70 ? Studio.amber : Studio.accent
                let color = dimmed ? hot.opacity(0.35) : hot
                context.fill(Path(rect),
                             with: .color(lit ? color : Studio.surfaceRaised))
            }
        }
    }
}


// Audio routing: a gain crosspoint matrix (routing-ux-spec §2b — every cell
// carries on/off AND a level). Hydrated from the CORE's sends, never from
// client defaults; select-never-destroys with an explicit Remove.
struct AudioRoutingSurface: View {
    @EnvironmentObject var model: AppModel

    var sources: [(id: String, label: String)] {
        model.strips.map { strip in (strip.id, model.audioSourceLabel(strip.id)) }
    }

    var body: some View {
        VStack(alignment: .leading, spacing: 8) {
            Text("Every crosspoint carries a level. Click to route or select; "
                 + "gain edits apply to the selected send.")
                .font(.grotesk(12)).foregroundStyle(Studio.secondary)
            if !model.audioRouteControlNotice.isEmpty {
                Text(model.audioRouteControlNotice)
                    .font(.grotesk(12)).foregroundStyle(Studio.amber)
            }
            if sources.isEmpty {
                Text("Sources appear here once channels carry PCM.")
                    .font(.grotesk(12)).foregroundStyle(Studio.textDim)
            }
            Grid(horizontalSpacing: 2, verticalSpacing: 2) {
                GridRow {
                    Color.clear.frame(width: 150, height: 14)
                    ForEach(AppModel.routingBuses, id: \.self) { bus in
                        MonoLabel(bus).frame(width: 62)
                    }
                }
                ForEach(sources, id: \.id) { source in
                    GridRow {
                        Text(source.label).font(.grotesk(12)).lineLimit(1)
                            .frame(width: 150, alignment: .leading)
                            .padding(.vertical, 4).padding(.leading, 6)
                            .background(Rectangle().fill(Studio.surface))
                        ForEach(AppModel.routingBuses, id: \.self) { bus in
                            let gain = model.sendGain(source: source.id, bus: bus)
                            let selected = model.selectedSend?.source == source.id
                                && model.selectedSend?.bus == bus
                            Button {
                                model.selectOrRouteSend(source: source.id, bus: bus)
                            } label: {
                                Text(gain.map { String(format: "%.1f", $0) } ?? "·")
                                    .font(.plexMono(9, gain != nil ? .semibold : .regular))
                                    .foregroundStyle(gain != nil ? Studio.accent
                                                                 : Studio.secondary)
                                    .frame(maxWidth: .infinity).frame(height: 24)
                                    .background(Rectangle().fill(
                                        gain != nil ? Studio.accent.opacity(0.16)
                                                    : Studio.field))
                                    .overlay(Rectangle().stroke(
                                        selected ? Studio.textPrimary
                                            : gain != nil ? Studio.accent.opacity(0.7)
                                                          : Studio.border,
                                        lineWidth: selected ? 2 : 1))
                            }
                            .buttonStyle(.plain)
                            .frame(width: 62)
                        }
                    }
                }
            }
            if let selected = model.selectedSend,
               let gain = model.sendGain(source: selected.source, bus: selected.bus) {
                HStack(spacing: 8) {
                    Text("\(selected.source) → \(selected.bus)")
                        .font(.grotesk(12, .medium))
                    Slider(value: Binding(
                        get: { gain },
                        set: { value in
                            model.setSendGain(source: selected.source,
                                              bus: selected.bus, gainDb: value)
                        }), in: -60...10)
                        .frame(width: 220)
                    Text(String(format: "%.1f dB", gain))
                        .font(.plexMono(10)).foregroundStyle(Studio.secondary)
                    Button("Remove route") {
                        model.removeSend(source: selected.source, bus: selected.bus)
                    }
                    .buttonStyle(GhostButtonStyle(tint: Studio.red))
                }
                .padding(8)
                .background(RoundedRectangle(cornerRadius: 8).fill(Studio.field))
                .overlay(RoundedRectangle(cornerRadius: 8)
                    .stroke(Studio.border, lineWidth: 1))
            }
        }
    }
}

// Master-bus mastering rack. Stages render in DSP order; the engage opacity
// mirrors the exact neutral-bypass condition (honesty rule: dim == a no-op).
// A3: COMPARE A/B, Starting point, Reset stage and MY PRESETS rows follow the
// Windows rack (W/Views/AudioPage.xaml:199-330); ranges follow its sliders.
struct MasteringRack: View {
    @EnvironmentObject var model: AppModel
    @State private var presetName = ""
    @State private var selectedPresetId: String?

    var params: MasteringParams { model.mastering }

    func bind(_ keyPath: WritableKeyPath<MasteringParams, Double>) -> Binding<Double> {
        Binding(get: { model.mastering[keyPath: keyPath] },
                set: { v in model.editMastering { $0[keyPath: keyPath] = v } })
    }

    var body: some View {
        VStack(alignment: .leading, spacing: 8) {
            HStack {
                Text("Master rack").font(.grotesk(13, .semibold))
                Toggle("", isOn: Binding(
                    get: { params.enabled },
                    set: { value in model.editMastering { $0.enabled = value } }))
                    .toggleStyle(.switch)
                    .controlSize(.mini)
                    .labelsHidden()
                    .accessibilityLabel("Master in")
                Text(params.enabled ? "MASTER IN" : "BYPASS")
                    .font(.plexMono(9, .semibold))
                    .foregroundStyle(params.enabled ? Studio.accent : Studio.textDim)
                Spacer()
                if params.enabled, abs(model.masteringRideDb) > 0.05 {
                    Text(String(format: "ride %+.1f dB", model.masteringRideDb))
                        .font(.plexMono(10)).foregroundStyle(Studio.secondary)
                }
            }
            compareRow
            presetsRow
            if !model.masteringStatus.isEmpty {
                Text(model.masteringStatus).font(.grotesk(11)).foregroundStyle(Studio.secondary)
            }
            HStack(alignment: .top, spacing: 8) {
                stage("01 INPUT", engaged: abs(params.inputGainDb) > 0.05) {
                    knob("Gain", value: bind(\.inputGainDb), in: -12...12, unit: "dB")
                }
                // The core runs high-pass when > 0 and low-pass when > 0
                // (AudioMastering.h:421-424): the light follows the value SENT.
                stage("02 FILTER", engaged: params.highPassHz > 0 || params.lowPassHz > 0) {
                    knob("Low cut", value: bind(\.highPassHz), in: 0...300, unit: "Hz")
                    knob("High cut", value: bind(\.lowPassHz), in: 0...20000, unit: "Hz")
                }
                stage("03 TONE", engaged: abs(params.lowShelfDb) > 0.05
                      || abs(params.presenceDb) > 0.05 || abs(params.highShelfDb) > 0.05) {
                    knob("Low", value: bind(\.lowShelfDb), in: -12...12, unit: "dB")
                    knob("Presence", value: bind(\.presenceDb), in: -12...12, unit: "dB")
                    knob("Air", value: bind(\.highShelfDb), in: -12...12, unit: "dB")
                }
                stage("04 RIDE", engaged: params.maxRideDb > 0.05) {
                    knob("Target", value: bind(\.targetLufs), in: -24...(-9), unit: "LUFS")
                    knob("Max ride", value: bind(\.maxRideDb), in: 0...12, unit: "dB")
                }
                stage("05 GLUE", engaged: params.glueAmount > 0.005) {
                    knob("Amount", value: bind(\.glueAmount), in: 0...1, unit: "")
                    knob("Ratio", value: bind(\.glueRatio), in: 1...10, unit: ":1")
                    knob("Attack", value: bind(\.glueAttackMs), in: 1...200, unit: "ms")
                    knob("Release", value: bind(\.glueReleaseMs), in: 20...1000, unit: "ms")
                    knob("Makeup", value: bind(\.glueMakeupDb), in: -6...6, unit: "dB")
                    Toggle("Multiband", isOn: Binding(
                        get: { params.glueMultiband },
                        set: { v in model.editMastering { $0.glueMultiband = v } }))
                        .toggleStyle(.switch).controlSize(.mini).font(.plexMono(9))
                    if params.glueMultiband {
                        knob("Low band", value: bind(\.glueBandLowDb), in: -6...6, unit: "dB")
                        knob("Mid band", value: bind(\.glueBandMidDb), in: -6...6, unit: "dB")
                        knob("High band", value: bind(\.glueBandHighDb), in: -6...6, unit: "dB")
                    }
                }
                stage("06 WIDTH", engaged: abs(params.stereoWidth - 1) > 0.005) {
                    knob("Width", value: bind(\.stereoWidth), in: 0...2, unit: "×")
                }
                stage("07 CEILING", engaged: params.limiterEnabled) {
                    knob("True peak", value: bind(\.ceilingDbfs), in: -6...(-0.5), unit: "dBFS")
                    Toggle("Master protection", isOn: Binding(
                        get: { params.limiterEnabled },
                        set: { v in model.editMastering { $0.limiterEnabled = v } }))
                        .toggleStyle(.switch).controlSize(.mini).font(.plexMono(9))
                    Text(params.limiterEnabled ? "In" : "Bypass")
                        .font(.plexMono(9)).foregroundStyle(params.limiterEnabled ? Studio.accent : Studio.amber)
                }
            }
            .opacity(params.enabled ? 1 : 0.5)
            .disabled(!params.enabled)
        }
    }

    var compareRow: some View {
        HStack(spacing: 8) {
            MonoLabel("Compare")
            ForEach(["A", "B"], id: \.self) { slot in
                Button(slot) { model.selectMasteringCompareSlot(slot) }
                    .buttonStyle(.plain)
                    .font(.plexMono(10, .semibold))
                    .padding(.horizontal, 9).padding(.vertical, 3)
                    .background(RoundedRectangle(cornerRadius: 4)
                        .fill(model.masteringCompareSlot == slot ? Studio.blue : Studio.surface))
                    .foregroundStyle(model.masteringCompareSlot == slot ? Studio.onAccent : Studio.secondary)
                    .help("Recall comparison \(slot); the current settings are retained in the other slot")
                    .accessibilityLabel("Comparison \(slot)")
            }
            Menu("Starting point") {
                Button("Streaming · -14 LUFS") { model.applyMasteringPreset("streaming") }
                Button("Podcast · -16 LUFS") { model.applyMasteringPreset("podcast") }
                Button("Broadcast · -23 LUFS") { model.applyMasteringPreset("broadcast") }
                Divider()
                Button("Neutral · protection only") { model.applyMasteringPreset("neutral") }
            }
            .menuStyle(.borderlessButton).fixedSize()
            Menu("Reset stage") {
                ForEach(Array(["Input", "Filter", "Tone", "Ride", "Glue", "Width", "Ceiling"].enumerated()),
                        id: \.offset) { index, name in
                    Button(String(format: "%02d · %@", index + 1, name)) {
                        model.resetMasteringStage(name.lowercased())
                    }
                }
            }
            .menuStyle(.borderlessButton).fixedSize()
            Text("A/B slots and the whole rack persist across restarts.")
                .font(.grotesk(11)).foregroundStyle(Studio.textDim)
        }
    }

    var presetsRow: some View {
        HStack(spacing: 8) {
            MonoLabel("My presets")
            TextField("Preset name", text: $presetName)
                .textFieldStyle(.roundedBorder).frame(width: 160)
            Button("Save") {
                if let saved = model.saveMasteringUserPreset(name: presetName) { selectedPresetId = saved.id }
            }
            .buttonStyle(GhostButtonStyle())
            .help("Save the current rack as a named preset (same name = overwrite)")
            Picker("Saved presets", selection: $selectedPresetId) {
                Text("Saved presets").tag(String?.none)
                ForEach(model.masteringUserPresets) { preset in
                    Text(preset.name).tag(Optional(preset.id))
                }
            }
            .labelsHidden().frame(width: 170)
            let hasPresets = !model.masteringUserPresets.isEmpty
            Button("Apply") { model.applyMasteringUserPreset(id: selectedPresetId) }
                .buttonStyle(GhostButtonStyle()).disabled(!hasPresets)
            Button("Rename") { model.renameMasteringUserPreset(id: selectedPresetId, to: presetName) }
                .buttonStyle(GhostButtonStyle()).disabled(!hasPresets)
                .help("Rename the selected preset to the name typed on the left")
            Button("Delete") {
                if model.deleteMasteringUserPreset(id: selectedPresetId) { selectedPresetId = nil }
            }
            .buttonStyle(GhostButtonStyle(tint: Studio.red)).disabled(!hasPresets)
        }
    }

    func stage(_ title: String, engaged: Bool,
               @ViewBuilder content: () -> some View) -> some View {
        VStack(alignment: .leading, spacing: 4) {
            MonoLabel(title, dim: !engaged)
            content()
        }
        .padding(8)
        .frame(maxWidth: .infinity, alignment: .leading)
        .background(RoundedRectangle(cornerRadius: 8).fill(Studio.field))
        .overlay(RoundedRectangle(cornerRadius: 8)
            .stroke(engaged ? Studio.accent.opacity(0.35) : Studio.border, lineWidth: 1))
        .opacity(engaged ? 1 : 0.65)
    }

    func knob(_ label: String, value: Binding<Double>,
              in range: ClosedRange<Double>, unit: String) -> some View {
        VStack(alignment: .leading, spacing: 1) {
            HStack(spacing: 3) {
                Text(label).font(.plexMono(9)).foregroundStyle(Studio.secondary)
                Spacer()
                Text(unit.isEmpty
                     ? String(format: "%.2f", value.wrappedValue)
                     : String(format: "%.1f %@", value.wrappedValue, unit))
                    .font(.plexMono(9)).foregroundStyle(Studio.textPrimary)
            }
            Slider(value: value, in: range)
        }
    }
}
