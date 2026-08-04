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
    @EnvironmentObject var model: AppModel
    @State private var surface = "SHOW"
    @State private var selectedStripId: String?

    var selected: AudioStrip? {
        model.strips.first { $0.id == (selectedStripId ?? model.strips.first?.id) }
    }

    var body: some View {
        VStack(alignment: .leading, spacing: 10) {
            HStack(spacing: 8) {
                ForEach(["SHOW", "ROUTING", "SETUP"], id: \.self) { name in
                    Button(name) { surface = name }
                        .buttonStyle(.plain)
                        .font(.plexMono(10, .semibold))
                        .foregroundStyle(surface == name ? Studio.onAccent : Studio.secondary)
                        .padding(.horizontal, 10).padding(.vertical, 5)
                        .background(RoundedRectangle(cornerRadius: 6)
                            .fill(surface == name ? Studio.blue : Studio.surface))
                }
                Spacer()
                Button("Pop out mixer") {}
                    .buttonStyle(GhostButtonStyle())
                    .disabled(true)
                    .help("Coming to the macOS shell")
            }
            switch surface {
            case "ROUTING":
                Text("Audio routing matrix arrives with the bus editor — "
                     + "sends currently follow the default topology "
                     + "(sources → master/pgm/stream/mon).")
                    .font(.grotesk(12)).foregroundStyle(Studio.secondary)
                    .padding(20)
            case "SETUP":
                VStack(alignment: .leading, spacing: 8) {
                    Text("Monitor output").font(.grotesk(13, .semibold))
                    Toggle("Monitor (system default output)", isOn: Binding(
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

    func stripLabel(_ strip: AudioStrip) -> String {
        strip.id == "zoom-mix" ? "Zoom mix"
            : strip.id == "local-machine-audio" ? "Local machine audio"
            : model.slots.first { $0.sourceId == strip.id.replacingOccurrences(
                of: "capture:", with: "") }?.name ?? strip.id
    }
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
                Button("+ Add processing") {}
                    .buttonStyle(GhostButtonStyle())
                    .disabled(true)
                    .help("VST inserts arrive with the plugin host port")
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
                SegmentedMeter(level: strip.muted ? 0 : strip.outputLevel)
                    .frame(width: 12, height: 150)
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

    var lufsLabel: String {
        model.shortTermLufs <= -119 ? "—" : String(format: "%.1f", model.shortTermLufs)
    }

    var body: some View {
        VStack(spacing: 8) {
            MonoLabel("Master")
            HStack(alignment: .firstTextBaseline, spacing: 4) {
                Text("LUFS").font(.grotesk(13, .semibold))
                Text(lufsLabel).font(.grotesk(24, .semibold))
            }
            Text(model.masterLevel > 0 ? "Live" : "Standing by")
                .font(.grotesk(11)).foregroundStyle(Studio.secondary)
            HStack(spacing: 6) {
                SegmentedMeter(level: model.masterLevel).frame(width: 12, height: 170)
                SegmentedMeter(level: model.masterLevel).frame(width: 12, height: 170)
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

struct SegmentedMeter: View {
    let level: Int  // 0..100

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
                let color: Color = threshold > 90 ? Studio.red
                    : threshold > 70 ? Studio.amber : Studio.accent
                context.fill(Path(rect),
                             with: .color(lit ? color : Studio.surfaceRaised))
            }
        }
    }
}
