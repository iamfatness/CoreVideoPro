// A7 Task 4: the SHOW processing workspace, ported from the Windows console
// (native-shell/CoreVideoPro.WinUI/Views/AudioPage.xaml:977-1308, "XAML" below):
// a 255-tall Border, padding 10,8, columns 190 | * | * | 1.4* with spacing 10.
// RACK (channel name, the slot chain, "＋ Add processing"), then GATE, COMP and
// EQ, each drawing the REAL response curve (DspResponseCurve, Task 1) from the
// same parameters the core chain applies.
//
// Also here: the pure layout numbers (AudioLayout), the strip readout strings
// (ConsoleStripLabels) and the slot popover's parameter table (paramSpecs), so
// the shell tests pin them without a view.

import SwiftUI

/// Layout numbers from the XAML. SHOW: XAML:986-994 (255, 190/*/*/1.4*, 10),
/// XAML:1326 (strip 120), XAML:1435 (rail 150). SETUP (Task 5): XAML:37-49.
enum AudioLayout {
    static let workspaceHeight: CGFloat = 255
    static let rackWidth: CGFloat = 190
    static let stripWidth: CGFloat = 120
    static let railWidth: CGFloat = 150
    static let setupLeftWidth: CGFloat = 340
    static let setupRightMaxWidth: CGFloat = 820
    static let columnSpacing: CGFloat = 10
    static let setupColumnSpacing: CGFloat = 12
    /// AudioPane's padding around the header and surface (main tab and pop-out).
    static let panePadding: CGFloat = 10
    /// XAML:38 Padding="0,0,8,16": SETUP's trailing and bottom padding.
    static let setupTrailingPadding: CGFloat = 8
    static let setupBottomPadding: CGFloat = 16
    /// W/Views/OperatorTabResources.xaml:8-14 OperatorSectionBorder padding 16.
    static let setupSectionPadding: CGFloat = 16

    /// The pop-out mixer window. Windows opens it at 1080 x 700
    /// (W/Views/AudioMixerWindow.xaml.cs:17-18 WindowWidth/WindowHeight, applied
    /// at :68 appWindow.Resize). The minimum keeps 900 wide (the Task 4 render
    /// held at 900) and 640 tall: below that the SHOW strip faders collapse to a
    /// few points, the meter tick labels overlap and the master meters vanish
    /// (Task 4 review, rendered at 900 x 520).
    static let popoutDefault = CGSize(width: 1080, height: 700)
    static let popoutMin = CGSize(width: 900, height: 640)

    /// GATE, COMP and EQ widths inside a workspace row `total` wide (the
    /// workspace's content width, padding already removed): the rack takes 190,
    /// three gaps of 10, and the rest splits 1 : 1 : 1.4 (the XAML star columns).
    static func cellWidths(total: CGFloat) -> (gate: CGFloat, comp: CGFloat, eq: CGFloat) {
        let rest = max(0, total - rackWidth - 3 * columnSpacing)
        let star = rest / 3.4
        return (gate: star, comp: star, eq: star * 1.4)
    }
}

/// The strip's readouts, verbatim from SVM:11451-11456 and SVM:11464.
enum ConsoleStripLabels {
    /// SVM:11452-11456 (PanLabel): |pan| < 0.01 reads "C", else "L 0.25" / "R 0.50".
    static func pan(_ pan: Double) -> String {
        if abs(pan) < 0.01 { return "C" }
        return pan < 0 ? String(format: "L %.2f", abs(pan)) : String(format: "R %.2f", pan)
    }

    /// SVM:11451 (GainLabel): `$"{(gain > 0 ? "+" : "")}{gain:0.0} dB"`.
    static func gain(_ gainDb: Double) -> String {
        (gainDb > 0 ? "+" : "") + String(format: "%.1f dB", gainDb)
    }

    /// SVM:11464 (MuteStateLabel), the meter's tooltip.
    static func meterTooltip(muted: Bool, sourceMuted: Bool) -> String {
        muted ? "Muted in mix" : sourceMuted ? "Source muted" : "Live"
    }
}

/// One slider in an insert slot's popover (W/Views/AudioPage.xaml.cs:137-166,
/// InsertParamSpecs: label, key, min, max, step, default).
struct InsertParamSpec: Equatable {
    let label: String
    let key: String
    let min: Double
    let max: Double
    let step: Double
    let defaultValue: Double
}

extension InsertChainPolicy {
    /// W/Views/AudioPage.xaml.cs:137-166, keyed by the core matcher (coreKind,
    /// the same branch order as the Windows `lowered.Contains` chain). The EQ
    /// row drops Windows' "Presence (Hz)" / "Presence (dB)": they write
    /// presenceHz / presenceDb, keys the core has not read since the 8-band EQ
    /// (C6b), so on Windows they are dead sliders. The bands live in the EQ cell.
    static func paramSpecs(_ name: String) -> [InsertParamSpec] {
        switch coreKind(name) {
        case .gate?:
            return [InsertParamSpec(label: "Threshold (dBFS)", key: "thresholdDb", min: -80, max: -12, step: 1, defaultValue: -48),
                    InsertParamSpec(label: "Release (ms)", key: "releaseMs", min: 20, max: 500, step: 5, defaultValue: 120)]
        case .highpass?, .eq?:
            return [InsertParamSpec(label: "High-pass (Hz)", key: "highpassHz", min: 20, max: 400, step: 5, defaultValue: 90)]
        case .compressor?:
            return [InsertParamSpec(label: "Threshold (dBFS)", key: "thresholdDb", min: -40, max: -6, step: 1, defaultValue: -18),
                    InsertParamSpec(label: "Ratio", key: "ratio", min: 1, max: 20, step: 0.5, defaultValue: 4)]
        case .limiter?:
            return [InsertParamSpec(label: "Ceiling (dBFS)", key: "ceilingDb", min: -12, max: -0.1, step: 0.1, defaultValue: -1)]
        case nil:
            return []
        }
    }
}

extension ChannelInserts {
    /// The ChannelInserts field a popover slider edits. The Mac keeps ONE set
    /// of settings per kind (settingsJson sends it under every chain name of
    /// that kind), so the popover and the cell edit the same value.
    private static func field(_ name: String, _ key: String) -> WritableKeyPath<ChannelInserts, Double>? {
        switch (InsertChainPolicy.coreKind(name), key) {
        case (.gate?, "thresholdDb"): return \.gateThresholdDb
        case (.gate?, "releaseMs"): return \.gateReleaseMs
        case (.highpass?, "highpassHz"), (.eq?, "highpassHz"): return \.eqHighpassHz
        case (.compressor?, "thresholdDb"): return \.compThresholdDb
        case (.compressor?, "ratio"): return \.compRatio
        case (.limiter?, "ceilingDb"): return \.limiterCeilingDb
        default: return nil
        }
    }

    func param(_ name: String, key: String) -> Double? {
        Self.field(name, key).map { self[keyPath: $0] }
    }

    mutating func setParam(_ name: String, key: String, _ value: Double) {
        guard let field = Self.field(name, key) else { return }
        self[keyPath: field] = value
    }
}

/// Console colours taken from the XAML where the plan names them.
enum AudioConsoleColor {
    static let title = Color(hex: 0xA7E8F2)        // XAML:1003 channel name, cell titles
    // Scoped to the Audio pane, so the global Studio tokens other tabs use stay put.
    static let console = Color(hex: 0x070B10)      // XAML:915 the SHOW console Border
    static let stripFill = Color(hex: 0x0B1016)    // XAML:1326 fader strip Border
    static let railFill = Color(hex: 0x0D1520)     // XAML:1443 master rail Border
    static let chipFill = Color(hex: 0x0B2F38)     // XAML:956 channel chip
    static let chipStroke = Color(hex: 0x1B5E6B)   // XAML:956
    static let chipBus = Color(hex: 0x5FA9B5)      // XAML:964 bus label
    static let workspace = Color(hex: 0x0C141B)    // XAML:986 workspace Border
    static let slotFill = Color(hex: 0x10222B)     // XAML:1047 slot button
    static let nameFill = Color(hex: 0x0E4B57)     // XAML:1414 strip name chip
    static let nameText = Color(hex: 0xD5F6FC)     // XAML:1419
    // W/Models/ProductionModels.cs:209-213 (InsertSlotItem LiveBrush / PendingBrush).
    static let slotLive = Color(hex: 0x2ED274)
    static let slotPending = Color(hex: 0xC79A2E)
}

/// The vertical slot chain (XAML:1036-1083), reused by SETUP (Task 5).
/// Clicking a slot opens a 240-wide popover: the slot name, `flyout(slot)`,
/// then "Remove from chain" (W/Views/AudioPage.xaml.cs:252-261).
struct InsertSlotList: View {
    let slots: [InsertSlot]
    let onRemove: (String) -> Void
    let flyout: (InsertSlot) -> AnyView

    var body: some View {
        VStack(alignment: .leading, spacing: 3) {
            ForEach(slots) { slot in
                InsertSlotRow(slot: slot, onRemove: onRemove, flyout: flyout)
            }
        }
    }
}

private struct InsertSlotRow: View {
    let slot: InsertSlot
    let onRemove: (String) -> Void
    let flyout: (InsertSlot) -> AnyView
    @State private var open = false

    var body: some View {
        Button { open = true } label: {
            HStack(spacing: 6) {
                Text(slot.slotLabel).font(.plexMono(9)).foregroundStyle(Studio.secondary)
                Text(slot.name).font(.grotesk(10, .semibold)).foregroundStyle(AudioConsoleColor.title)
                    .lineLimit(1).truncationMode(.tail)
                Spacer(minLength: 0)
                Text(slot.statusLabel).font(.plexMono(8, .bold))
                    .foregroundStyle(slot.statusLabel == "LIVE" ? AudioConsoleColor.slotLive : AudioConsoleColor.slotPending)
            }
            .padding(.horizontal, 8).padding(.vertical, 4)
            .frame(maxWidth: .infinity)
            .background(RoundedRectangle(cornerRadius: 3).fill(AudioConsoleColor.slotFill))
            .overlay(RoundedRectangle(cornerRadius: 3).stroke(AudioConsoleColor.chipStroke, lineWidth: 1))
            .contentShape(Rectangle())
        }
        .buttonStyle(.plain)
        .help(slot.tooltip)
        .accessibilityLabel("Insert \(slot.slotLabel), \(slot.name), \(slot.statusLabel)")
        .popover(isPresented: $open, arrowEdge: .trailing) {
            VStack(alignment: .leading, spacing: 6) {
                Text(slot.name).font(.grotesk(12, .semibold))
                flyout(slot)
                Button("Remove from chain") {
                    onRemove(slot.name)
                    open = false
                }
                .buttonStyle(GhostButtonStyle(tint: Studio.red))
                .padding(.top, 6)
            }
            .padding(12)
            .frame(width: 240, alignment: .leading)
        }
    }
}

/// XAML:984-1307. `stripId` nil = no channel to edit: every cell dims to 0.45
/// and stops taking clicks (SVM:8863-8866, WorkspaceEnabledOpacity).
struct ProcessingWorkspace: View {
    @EnvironmentObject var model: AppModel
    let stripId: String?
    let stripName: String

    static let bandLabels = ["63", "125", "250", "500", "1k", "2k", "4k", "8k"]

    var hasChannel: Bool { stripId != nil }
    var inserts: ChannelInserts { stripId.flatMap { model.channelInserts[$0] } ?? ChannelInserts() }

    func edit(_ apply: (inout ChannelInserts) -> Void) {
        guard let stripId else { return }
        model.editInserts(stripId, apply)
    }

    func bind(_ keyPath: WritableKeyPath<ChannelInserts, Double>) -> Binding<Double> {
        Binding(get: { inserts[keyPath: keyPath] },
                set: { value in edit { $0[keyPath: keyPath] = value } })
    }

    /// The rack edits the strip it shows and never moves SETUP's processing
    /// target: Windows' rack acts on the selected channel, and only a strip
    /// click selects the channel target (SVM:4688-4693). Static so the shell
    /// tests drive the exact path the rack uses.
    static func rackAdd(_ model: AppModel, stripId: String?, _ name: String) {
        guard let stripId else { return }
        model.addChannelInsert(name, stripId: stripId)
    }

    static func rackRemove(_ model: AppModel, stripId: String?, _ name: String) {
        guard let stripId else { return }
        model.removeChannelInsert(name, stripId: stripId)
    }

    var body: some View {
        GeometryReader { geo in
            let widths = AudioLayout.cellWidths(total: geo.size.width)
            HStack(alignment: .top, spacing: AudioLayout.columnSpacing) {
                rack.frame(width: AudioLayout.rackWidth)
                Group {
                    gateCell.frame(width: widths.gate)
                    compCell.frame(width: widths.comp)
                    eqCell.frame(width: widths.eq)
                }
                .frame(maxHeight: .infinity, alignment: .top)
                .opacity(hasChannel ? 1 : 0.45)
                .allowsHitTesting(hasChannel)
            }
        }
        .padding(.horizontal, 10).padding(.vertical, 8)
        .frame(height: AudioLayout.workspaceHeight)
        .background(RoundedRectangle(cornerRadius: 4).fill(AudioConsoleColor.workspace))
        .overlay(RoundedRectangle(cornerRadius: 4).stroke(Studio.border, lineWidth: 1))
        // W/Views/AudioPage.xaml.cs:335-337: Windows makes sure a scan has run
        // when the add menu opens. A SwiftUI Menu has no opening hook, so the
        // workspace asks when it appears (the PLUG-INS surface that used to
        // ask now sits inside SETUP's collapsed disclosure).
        .onAppear { model.vstSurfaceAppeared() }
        .onDisappear { model.vstSurfaceDisappeared() }
    }

    // ── RACK (XAML:996-1092) ────────────────────────────────────────────────

    var slots: [InsertSlot] {
        InsertChainPolicy.slots(chain: inserts.chain, onBus: false, vstLive: model.vstInsertsLive,
                                vstStatusLine: model.vstStatusLine, labelStyle: .channel)
    }

    var rack: some View {
        VStack(alignment: .leading, spacing: 4) {
            VStack(alignment: .leading, spacing: 0) {
                Text("CHANNEL").font(.plexMono(8)).tracking(0.8).foregroundStyle(Studio.secondary)
                Text(hasChannel ? stripName : "—").font(.grotesk(14, .bold))
                    .foregroundStyle(AudioConsoleColor.title)
                    .lineLimit(1).truncationMode(.tail)
                // XAML:1005-1035: the guest lip-sync button and flyout land here
                // in Task 7. Until then nothing renders.
            }
            ScrollView(.vertical) {
                InsertSlotList(
                    slots: slots,
                    onRemove: { name in Self.rackRemove(model, stripId: stripId, name) },
                    flyout: { AnyView(slotFlyout($0)) })
            }
            .frame(maxHeight: .infinity)
            addMenu
        }
        .frame(maxHeight: .infinity, alignment: .top)
        .disabled(!hasChannel)
    }

    /// XAML:1084-1091 and W/Views/AudioPage.xaml.cs:326-362 (OnAddInsertFlyoutOpening).
    var addMenu: some View {
        Menu {
            Button("BUILT-IN — processes live") {}.disabled(true)
            ForEach(ChannelInserts.builtInNames, id: \.self) { name in
                Button(name) { add(name) }
            }
            Divider()
            Button("VST3 PLUG-INS") {}.disabled(true)
            let state = model.vstAddMenuState
            if let line = VstAddMenuPolicy.emptyLine(state) {
                if state == .scanFailed {
                    Button(line) { model.scanVstPlugins() }
                } else {
                    Button(line) {}.disabled(true)
                }
            } else {
                // When the host cannot run an insert on this machine: say why, and
                // list the scanned plug-ins dimmed (honest reporting;
                // VstAddMenuPolicy.pluginRows).
                ForEach(model.vstAddMenuRows) { row in
                    Button(row.title) {
                        if let plugin = row.plugin {
                            add(AppModel.vstInsertName(plugin: plugin, className: plugin.classNames.first))
                        }
                    }
                    .disabled(!row.enabled)
                }
            }
        } label: {
            // XAML:1081 DropDownButton FontSize="10" Padding="10,4". A borderless
            // Menu draws its label through AppKit at the system size and ignores
            // .font, so the label is a SwiftUI view under the .button style.
            HStack(spacing: 6) {
                Text("＋ Add processing").font(.grotesk(10, .medium)).foregroundStyle(Studio.textPrimary)
                Image(systemName: "chevron.down").font(.grotesk(8, .semibold)).foregroundStyle(Studio.secondary)
            }
            .padding(.horizontal, 10).padding(.vertical, 4)
            .frame(maxWidth: .infinity)
            .background(RoundedRectangle(cornerRadius: 4).fill(Studio.surface))
            .overlay(RoundedRectangle(cornerRadius: 4).stroke(Studio.border, lineWidth: 1))
            .contentShape(Rectangle())
        }
        .menuStyle(.button)
        .buttonStyle(.plain)
        .menuIndicator(.hidden)
        .frame(maxWidth: .infinity)
        .help("Add a built-in processor or a VST3 plugin to this channel")
        .accessibilityLabel("Add processing")
    }

    func add(_ name: String) { Self.rackAdd(model, stripId: stripId, name) }

    /// W/Views/AudioPage.xaml.cs:168-250 (OnInsertSlotFlyoutOpening). No
    /// "Open plug-in controls" button: the Mac host has no editor window yet.
    @ViewBuilder
    func slotFlyout(_ slot: InsertSlot) -> some View {
        let specs = InsertChainPolicy.paramSpecs(slot.name)
        if specs.isEmpty {
            // AudioPage.xaml.cs:219-227, verbatim.
            Text(slot.isBuiltIn
                 ? "No adjustable parameters."
                 : "Parameters appear here once the plug-in is processing. If the plug-in becomes unavailable, CoreVideo keeps program audio running.")
                .font(.grotesk(10)).foregroundStyle(Studio.secondary)
                .fixedSize(horizontal: false, vertical: true)
        } else {
            ForEach(specs, id: \.key) { spec in
                let value = inserts.param(slot.name, key: spec.key) ?? spec.defaultValue
                VStack(alignment: .leading, spacing: 1) {
                    HStack {
                        Text(spec.label).font(.plexMono(9)).foregroundStyle(Studio.secondary)
                        Spacer()
                        Text(String(format: spec.step < 1 ? "%.1f" : "%.0f", value))
                            .font(.plexMono(9)).foregroundStyle(Studio.textPrimary)
                    }
                    Slider(value: Binding(get: { value },
                                          set: { new in edit { $0.setParam(slot.name, key: spec.key, new) } })
                                .stepped(spec.step, in: spec.min...spec.max),
                           in: spec.min...spec.max)
                        .controlSize(.small)
                }
            }
        }
    }

    // ── GATE / COMP / EQ (XAML:1094-1306) ───────────────────────────────────

    func cellHeader(_ title: String, on: Bool, insert: String, help: String,
                    @ViewBuilder middle: () -> some View = { Spacer(minLength: 0) }) -> some View {
        HStack(spacing: 8) {
            Text(title).font(.plexMono(10, .heavy)).tracking(0.6).foregroundStyle(AudioConsoleColor.title)
            middle()
            Button("IN") { edit { $0.toggle(insert) } }
                .buttonStyle(.plain)
                .font(.plexMono(9, .semibold))
                .padding(.horizontal, 8).padding(.vertical, 2)
                .background(RoundedRectangle(cornerRadius: 3)
                    .fill(on ? Studio.accent.opacity(0.22) : Studio.surface))
                .overlay(RoundedRectangle(cornerRadius: 3)
                    .stroke(on ? Studio.accent.opacity(0.6) : Studio.border, lineWidth: 1))
                .foregroundStyle(on ? Studio.accent : Studio.textDim)
                .help(help)
                .accessibilityLabel("\(title) in")
                .accessibilityValue(on ? "on" : "off")
        }
        .frame(height: 18)
    }

    func labeledSlider(_ label: String, _ value: Binding<Double>, in range: ClosedRange<Double>,
                       step: Double) -> some View {
        VStack(alignment: .leading, spacing: 0) {
            Text(label).font(.plexMono(9)).foregroundStyle(Studio.secondary)
                .lineLimit(1).minimumScaleFactor(0.7)
            Slider(value: value.stepped(step, in: range), in: range).controlSize(.mini)
        }
        .frame(maxWidth: .infinity)
    }

    var gateCell: some View {
        VStack(alignment: .leading, spacing: 4) {
            cellHeader("GATE", on: inserts.gateEnabled, insert: ChannelInserts.gateName,
                       help: "Engage the gate on this channel")
            DspResponseCurve(kind: .gate, p1: inserts.gateThresholdDb, p2: inserts.gateRangeDb,
                             isEngaged: inserts.gateEnabled)
                .frame(maxHeight: .infinity)
            HStack(spacing: 8) {
                labeledSlider("Threshold (dB)", bind(\.gateThresholdDb), in: -80...(-12), step: 1)
                labeledSlider("Range (dB)", bind(\.gateRangeDb), in: -80...(-6), step: 1)
            }
            HStack(spacing: 8) {
                labeledSlider("Attack", bind(\.gateAttackMs), in: 0.5...50, step: 0.5)
                labeledSlider("Hold", bind(\.gateHoldMs), in: 0...500, step: 10)
                labeledSlider("Release", bind(\.gateReleaseMs), in: 20...500, step: 5)
            }
        }
    }

    var compCell: some View {
        let reduction = stripId.flatMap { id in model.strips.first { $0.id == id }?.gainReductionDb } ?? 0
        let gr = MasterMeterReadout.gainReduction(reduction)
        return VStack(alignment: .leading, spacing: 4) {
            cellHeader("COMP", on: inserts.compEnabled, insert: ChannelInserts.compName,
                       help: "Engage the compressor on this channel") {
                // XAML:1185-1192: a 16-segment GR meter, height 8, and its readout.
                SegmentedBar(level: gr.level, segments: 16).frame(height: 8).padding(.horizontal, 4)
                Text(gr.label).font(.plexMono(9)).foregroundStyle(Studio.secondary).lineLimit(1)
                    .fixedSize()
            }
            DspResponseCurve(kind: .comp, p1: inserts.compThresholdDb, p2: inserts.compRatio,
                             p3: inserts.compKneeDb, isEngaged: inserts.compEnabled)
                .frame(maxHeight: .infinity)
            HStack(spacing: 8) {
                labeledSlider("Threshold (dB)", bind(\.compThresholdDb), in: -40...(-6), step: 1)
                labeledSlider("Ratio", bind(\.compRatio), in: 1...20, step: 0.5)
            }
            HStack(spacing: 8) {
                labeledSlider("Attack", bind(\.compAttackMs), in: 0.1...100, step: 0.5)
                labeledSlider("Release", bind(\.compReleaseMs), in: 10...1000, step: 10)
                labeledSlider("Knee", bind(\.compKneeDb), in: 0...24, step: 1)
                labeledSlider("Makeup", bind(\.compMakeupDb), in: 0...24, step: 0.5)
            }
        }
    }

    var eqCell: some View {
        VStack(alignment: .leading, spacing: 4) {
            cellHeader("EQ", on: inserts.eqEnabled, insert: ChannelInserts.eqName,
                       help: "Engage the voice EQ on this channel")
            DspResponseCurve(kind: .eq, p1: inserts.eqHighpassHz, bands: inserts.eqBandsDb,
                             isEngaged: inserts.eqEnabled)
                .frame(maxHeight: .infinity)
            // XAML:1248-1303: LOW CUT (1.6*) and eight band faders (*), height 86.
            GeometryReader { geo in
                let star = max(0, geo.size.width - 8 * 2) / 9.6
                HStack(spacing: 2) {
                    VStack(spacing: 1) {
                        Text("LOW CUT").font(.plexMono(8)).foregroundStyle(Studio.secondary)
                            .lineLimit(1).minimumScaleFactor(0.7)
                        // XAML:1252-1256: the label and the slider, no readout.
                        Slider(value: bind(\.eqHighpassHz).stepped(5, in: 20...400), in: 20...400).controlSize(.mini)
                    }
                    .padding(.trailing, 6)
                    .frame(width: star * 1.6)
                    .frame(maxHeight: .infinity)
                    ForEach(0..<8, id: \.self) { index in
                        VStack(spacing: 2) {
                            Slider(value: Binding(
                                get: { inserts.eqBandsDb[index] },
                                set: { value in edit { $0.eqBandsDb[index] = value } }).stepped(0.5, in: -12...12),
                                   in: -12...12)
                                .controlSize(.mini)
                                .frame(width: 62)
                                .rotationEffect(.degrees(-90))
                                .frame(width: min(star, 20), height: 62)
                                .accessibilityLabel("EQ \(Self.bandLabels[index]) Hz")
                            Text(Self.bandLabels[index]).font(.plexMono(8)).foregroundStyle(Studio.secondary)
                                .lineLimit(1).fixedSize()
                        }
                        .frame(width: star)
                    }
                }
            }
            .frame(height: 86)
        }
    }
}

extension Binding where Value == Double {
    /// Snaps writes to `step` from the range's lower bound (the Windows
    /// StepFrequency) without passing `step:` to Slider, which on macOS draws a
    /// tick mark per step and turns a 1000-step track into a dashed bar.
    func stepped(_ step: Double, in range: ClosedRange<Double>) -> Binding<Double> {
        Binding(get: { wrappedValue }, set: { raw in
            guard step > 0 else { wrappedValue = raw; return }
            let snapped = range.lowerBound + ((raw - range.lowerBound) / step).rounded() * step
            wrappedValue = Swift.min(range.upperBound, Swift.max(range.lowerBound, snapped))
        })
    }
}

/// A horizontal segmented bar (Windows AudioLevelMeter, horizontal): the COMP
/// cell's gain-reduction meter (XAML:1185, SegmentCount 16).
struct SegmentedBar: View {
    let level: Int  // 0..100
    let segments: Int

    var body: some View {
        Canvas { context, size in
            let gap: CGFloat = 1
            let width = (size.width - gap * CGFloat(segments - 1)) / CGFloat(segments)
            guard width > 0 else { return }
            for index in 0..<segments {
                let threshold = Double(index + 1) / Double(segments) * 100
                let rect = CGRect(x: CGFloat(index) * (width + gap), y: 0, width: width, height: size.height)
                context.fill(Path(rect), with: .color(Double(level) >= threshold ? Studio.amber : Studio.surfaceRaised))
            }
        }
    }
}
