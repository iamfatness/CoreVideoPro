// A7 Task 5: the Audio SETUP surface in the Windows two-column layout
// (native-shell/CoreVideoPro.WinUI/Views/AudioPage.xaml:37-797, "XAML" below;
// StudioViewModel.cs, "SVM"):
//   - left, 340 wide: "Devices & monitoring" with the Monitor output and Local
//     audio source cards (XAML:102-181);
//   - right, flexible up to 820: "Master processor" (guide meters and rack,
//     XAML:187-627), a divider, then "Inserts & plug-ins" (XAML:630-797):
//     processing target, the collapsed VST3 browser, readouts, SIGNAL FLOW,
//     the 2x2 add grid, and the selected channel's Mute.
// OD-2: the Mac-only PLUG-INS pill is folded into the collapsed
// "Advanced · VST3 plug-in browser" disclosure here, as on Windows.

import SwiftUI

enum AudioSetupPolicy {
    struct AddButton: Equatable {
        let title: String
        let enabled: Bool
        let reason: String?
    }

    static let eqInsert = "Built-in EQ"          // XAML:766 CommandParameter
    static let compressorInsert = "Compressor"   // XAML:771 CommandParameter

    /// XAML:762-786: Add EQ, Add compressor, Add VST3 plug-in, Clear chain.
    /// Add EQ is dimmed on a bus target with the OD-1 sentence
    /// (AudioProcessingTargetPolicy.busRefusal, the same text the model's
    /// refusal gives); Add VST3 is dimmed with the host status line while the
    /// host cannot run an insert; Clear chain is dimmed on an empty chain.
    static func addButtons(target: String, vstLive: Bool, vstStatusLine: String,
                           chain: [String]) -> [AddButton] {
        let onBus = AudioProcessingTargetPolicy.parse(target).bus != nil
        let eqRefusal = onBus ? AudioProcessingTargetPolicy.busRefusal(eqInsert) : nil
        let compRefusal = onBus ? AudioProcessingTargetPolicy.busRefusal(compressorInsert) : nil
        return [
            AddButton(title: "Add EQ", enabled: eqRefusal == nil, reason: eqRefusal),
            AddButton(title: "Add compressor", enabled: compRefusal == nil, reason: compRefusal),
            AddButton(title: "Add VST3 plug-in", enabled: vstLive, reason: vstLive ? nil : vstStatusLine),
            AddButton(title: "Clear chain", enabled: !chain.isEmpty,
                      reason: chain.isEmpty ? "The insert chain is already empty." : nil),
        ]
    }

    /// The core reads a bus chain only from the routing sends TO that bus
    /// (MediaCore.cpp:7195-7200), and runs it only on a bus that has PCM
    /// (:7201-7206). A bus-to-bus OUTPUT send carries no chain and feeds no
    /// PCM into the bus, so it does not count. Honest reporting: an add to an
    /// unrouted bus still says "<n> added to <bus>" (SVM:5594), so the target
    /// section says why nothing will be heard.
    static let unroutedBusNote = "Nothing is routed to this bus yet, so its inserts do not run."

    static func unroutedNote(target: String, routedBusIds: Set<String>) -> String? {
        guard let bus = AudioProcessingTargetPolicy.parse(target).bus else { return nil }
        return routedBusIds.contains(bus) ? nil : unroutedBusNote
    }

    /// XAML:692-709 readouts. Windows binds them to SelectedAudioMix, the
    /// strip selected on SHOW, whatever the processing target (SVM:2195-2233;
    /// XAML:692-709 and the Mute button at :794-796 carry no visibility
    /// binding). SVM:2200-2213 (SelectedGainLabel, `{gain:0} dB` with a sign),
    /// SVM:2215-2227 (SelectedManualGainLabel, 0 = "Auto"), SVM:2233
    /// (SelectedOutputLevel, the core's 0..100). The Mac strip has ONE gain
    /// (manualGainDb, the fader the core applies; there is no separate
    /// GainDb), so both gain readouts read it. No selected strip reads a dash
    /// (Windows prints "0 dB" / "Auto" / 0 there, which reads as a real value).
    static func readouts(strip: AudioStrip?) -> (gain: String, manual: String, output: String) {
        guard let strip else { return ("—", "—", "—") }
        func signed(_ db: Double) -> String { (db > 0 ? "+" : "") + String(format: "%.0f dB", db) }
        let manual = strip.manualGainDb == 0 ? "Auto" : signed(strip.manualGainDb)
        return (signed(strip.manualGainDb), manual, "\(strip.outputLevel)")
    }

    /// SVM:2229-2230 (SelectedMuteButtonLabel).
    static func muteLabel(muted: Bool) -> String { muted ? "Unmute in mix" : "Mute in mix" }
}

extension AppModel {
    var selectedProcessingTarget: AudioProcessingTarget? {
        processingTargets.first { $0.id == selectedProcessingTargetId }
    }

    /// The selected target's chain: a channel's ordered chain, or a bus chain.
    var selectedTargetChain: [String] {
        let target = AudioProcessingTargetPolicy.parse(selectedProcessingTargetId)
        if let channel = target.channel { return channelInserts[channel]?.chain ?? [] }
        if let bus = target.bus { return busInserts[bus] ?? [] }
        return []
    }

    /// XAML:714-756 SIGNAL FLOW slots (target style, "01").
    var selectedTargetSlots: [InsertSlot] {
        InsertChainPolicy.slots(chain: selectedTargetChain,
                                onBus: AudioProcessingTargetPolicy.parse(selectedProcessingTargetId).bus != nil,
                                vstLive: vstInsertsLive, vstStatusLine: vstStatusLine, labelStyle: .target)
    }

    /// Buses that some source send names: the base matrix's sends (defaults
    /// plus operator edits, AudioConsolePolicy.baseRoutingSends) and the
    /// legacy audioSends grid.
    var busesWithSourceSends: Set<String> {
        var ids = Set((routingBaseCommand()?["sends"] as? [JSONObject] ?? []).compactMap { $0["busId"] as? String })
        for routes in audioSends.values { ids.formUnion(routes.keys) }
        return ids
    }

    /// SVM:2195-2198 (SelectedAudioMix): the SHOW-selected strip, which the
    /// SETUP readouts and Mute follow whatever the processing target. The
    /// same effective strip SHOW highlights (first strip when none is chosen).
    var setupSelectedStrip: AudioStrip? { effectiveAudioStrip }

    /// SETUP's add-VST menu says the same scan state as SHOW's add menu
    /// (VstAddMenuPolicy): not run yet / scanning / none found, nil = list.
    var setupVstMenuEmptyLine: String? {
        VstAddMenuPolicy.emptyLine(VstAddMenuPolicy.state(status: vstHost.status, pluginCount: vstPlugins.count,
                                                          scanCompleted: vstScanCompleted))
    }

    var setupReadouts: (gain: String, manual: String, output: String) {
        AudioSetupPolicy.readouts(strip: setupSelectedStrip)
    }

    var selectedTargetUnroutedNote: String? {
        AudioSetupPolicy.unroutedNote(target: selectedProcessingTargetId, routedBusIds: busesWithSourceSends)
    }
}

/// XAML:37-797. The header row (pills, Zoom mode, pop-out) sits above the
/// surface switch in AudioPane and spans both columns, as on Windows.
struct AudioSetupSurface: View {
    @EnvironmentObject var model: AppModel
    @Binding var vstBrowserOpen: Bool

    // The master rack reflows inside the column (MasteringRack: the compare
    // and preset rows wrap, the stages fall into as many columns as fit), so a
    // narrow column (the 900-wide pop-out) clips nothing and needs no sideways scroll.
    var body: some View {
        ScrollView(.vertical) {
            // XAML:44-48: 340 | * (MaxWidth 820), ColumnSpacing 12.
            HStack(alignment: .top, spacing: AudioLayout.setupColumnSpacing) {
                devicesColumn.frame(width: AudioLayout.setupLeftWidth)
                processingColumn
                    .frame(maxWidth: AudioLayout.setupRightMaxWidth, alignment: .topLeading)
            }
            .frame(maxWidth: .infinity, alignment: .topLeading)
            .padding(.trailing, 8).padding(.bottom, 16)   // XAML:38 Padding="0,0,8,16"
        }
    }

    // ── Left: Devices & monitoring (XAML:102-181) ───────────────────────────

    var devicesColumn: some View {
        SetupSection {
            VStack(alignment: .leading, spacing: 10) {
                SetupSectionTitle("Devices & monitoring")
                monitorCard
                // Notices about the monitor stay under its card.
                if !model.monitorControlNotice.isEmpty {
                    Text(model.monitorControlNotice).font(.plexMono(10))
                        .foregroundStyle(Studio.amber)
                        .fixedSize(horizontal: false, vertical: true)
                }
                if model.monitorFeedbackRisk {
                    Text("Monitor output matches the loopback device — feedback risk")
                        .font(.grotesk(12)).foregroundStyle(Studio.amber)
                        .fixedSize(horizontal: false, vertical: true)
                }
                SetupCard { LocalAudioSourceCard() }
            }
        }
    }

    /// XAML:106-150: title 12 semibold, status 10 muted, Device, a full-width
    /// Monitor toggle, the volume slider and a 46-wide percentage.
    var monitorCard: some View {
        SetupCard {
            VStack(alignment: .leading, spacing: 8) {
                VStack(alignment: .leading, spacing: 5) {
                    Text("Monitor output").font(.grotesk(12, .semibold))
                    // The engine's own monitorStatus; nothing reported reads a dash.
                    Text(model.monitorStatus.isEmpty ? "—" : model.monitorStatus)
                        .font(.plexMono(10)).foregroundStyle(Studio.secondary)
                        .fixedSize(horizontal: false, vertical: true)
                }
                VStack(alignment: .leading, spacing: 3) {
                    Text("Device").font(.grotesk(11)).foregroundStyle(Studio.secondary)
                    MonitorDevicePicker(width: AudioLayout.setupLeftWidth - 2 * setupSectionPadding - 2 * 12)
                }
                SetupToggleButton(title: "Monitor", isOn: Binding(
                    get: { model.monitorEnabled },
                    set: { model.setMonitor(enabled: $0, volume: model.monitorVolume) }))
                HStack(spacing: 8) {
                    Slider(value: Binding(
                        get: { model.monitorVolume },
                        set: { model.setMonitor(enabled: model.monitorEnabled, volume: $0) }), in: 0...1)
                        .controlSize(.small)
                        .accessibilityLabel("Monitor volume")
                    // SVM:1394 AudioMonitorVolumeLabel `{volume*100:0}%`.
                    Text(String(format: "%.0f%%", model.monitorVolume * 100))
                        .font(.grotesk(11, .semibold)).foregroundStyle(Studio.textPrimary)
                        .frame(width: 46, alignment: .trailing)
                }
            }
        }
    }

    // ── Right: Master processor, then Inserts & plug-ins ────────────────────

    var processingColumn: some View {
        SetupSection {
            VStack(alignment: .leading, spacing: 10) {
                // XAML:193-205. The MASTER IN toggle is the existing one in the
                // rack's own header row (MasteringRack, unchanged).
                VStack(alignment: .leading, spacing: 2) {
                    SetupSectionTitle("Master processor")
                    Text("Program L/R · processing order")   // XAML:199
                        .font(.grotesk(11)).foregroundStyle(Studio.secondary)
                }
                MasteringGuideMeters()
                MasteringRack()
                Rectangle().fill(Studio.border).frame(height: 1).padding(.vertical, 4)   // XAML:629
                InsertsAndPlugins(vstBrowserOpen: $vstBrowserOpen)
            }
        }
    }
}

/// XAML:630-797 "Inserts & plug-ins".
struct InsertsAndPlugins: View {
    @EnvironmentObject var model: AppModel
    @Binding var vstBrowserOpen: Bool

    var target: AudioProcessingTarget? { model.selectedProcessingTarget }
    var parsed: (channel: String?, bus: String?) { AudioProcessingTargetPolicy.parse(model.selectedProcessingTargetId) }
    /// The readouts and Mute follow the SHOW-selected strip (SVM:2195-2233).
    var strip: AudioStrip? { model.setupSelectedStrip }

    var body: some View {
        VStack(alignment: .leading, spacing: 10) {
            SetupSectionTitle("Inserts & plug-ins")
            Text("Choose the exact channel or bus, then build its insert chain in processing order.")   // XAML:632-633
                .font(.grotesk(11)).foregroundStyle(Studio.secondary)
                .fixedSize(horizontal: false, vertical: true)
            VStack(alignment: .leading, spacing: 3) {
                Text("Processing target").font(.grotesk(11)).foregroundStyle(Studio.secondary)
                Picker("", selection: Binding(
                    get: { model.selectedProcessingTargetId },
                    set: { model.selectProcessingTarget($0) })) {
                    ForEach(model.processingTargets) { option in Text(option.label).tag(option.id) }
                }
                .labelsHidden()
                .frame(maxWidth: .infinity)
                .accessibilityLabel("Processing target")
            }
            VStack(alignment: .leading, spacing: 2) {
                Text(target?.label ?? "—").font(.grotesk(16, .semibold))
                Text(target?.detail ?? "").font(.grotesk(11)).foregroundStyle(Studio.secondary)
                if let note = model.selectedTargetUnroutedNote {
                    Text(note).font(.grotesk(11)).foregroundStyle(Studio.textDim)
                        .fixedSize(horizontal: false, vertical: true)
                }
            }
            // XAML:645-686: collapsed by default; plug-in discovery is opened by
            // the operator only.
            DisclosureGroup(isExpanded: $vstBrowserOpen) {
                VstInsertsSurface().padding(.top, 8)
            } label: {
                Text("Advanced · VST3 plug-in browser").font(.grotesk(12, .medium))
                    .foregroundStyle(Studio.textPrimary)
            }
            readouts
            signalFlow
            addGrid
            // XAML:794-796: always shown; it toggles the SHOW-selected strip
            // (SVM:7175 ToggleSelectedParticipantMute) and is dimmed with none.
            Button(AudioSetupPolicy.muteLabel(muted: strip?.muted ?? false)) {
                if let strip { model.editStrip(strip.id) { $0.muted.toggle() } }
            }
            .buttonStyle(GhostButtonStyle())
            .disabled(strip == nil)
            .opacity(strip == nil ? 0.45 : 1)
        }
        // SHOW's workspace kicks the scan on appear; SETUP may be opened first.
        .onAppear { model.kickVstScanIfNeeded() }
    }

    var readouts: some View {
        let values = model.setupReadouts
        return VStack(alignment: .leading, spacing: 6) {
            readoutBox("Audio gain", values.gain)
            readoutBox("Manual gain", values.manual)
            readoutBox("Output level", values.output)
        }
    }

    func readoutBox(_ title: String, _ value: String) -> some View {
        VStack(alignment: .leading, spacing: 2) {
            Text(title).font(.grotesk(10)).foregroundStyle(Studio.secondary)
            Text(value).font(.grotesk(12)).foregroundStyle(Studio.textPrimary)
        }
        .modifier(SetupReadoutBorder())
    }

    var signalFlow: some View {
        VStack(alignment: .leading, spacing: 4) {
            Text("SIGNAL FLOW · TOP TO BOTTOM").font(.plexMono(9, .bold)).foregroundStyle(Studio.secondary)
            InsertSlotList(slots: model.selectedTargetSlots,
                           onRemove: { model.removeProcessingInsert($0) },
                           flyout: { slot in AnyView(SetupSlotFlyout(stripId: parsed.channel, slot: slot)) })
            Text(target?.insertLabel ?? "No inserts")
                .font(.grotesk(10)).foregroundStyle(Studio.secondary)
                .fixedSize(horizontal: false, vertical: true)
        }
        .modifier(SetupReadoutBorder())
    }

    var addGrid: some View {
        let buttons = AudioSetupPolicy.addButtons(target: model.selectedProcessingTargetId,
                                                  vstLive: model.vstInsertsLive,
                                                  vstStatusLine: model.vstStatusLine,
                                                  chain: model.selectedTargetChain)
        return VStack(alignment: .leading, spacing: 8) {
            HStack(spacing: 8) {
                gridButton(buttons[0]) { model.addProcessingInsert(AudioSetupPolicy.eqInsert) }
                gridButton(buttons[1]) { model.addProcessingInsert(AudioSetupPolicy.compressorInsert) }
            }
            HStack(spacing: 8) {
                vstMenu(buttons[2])
                gridButton(buttons[3]) { model.clearProcessingInserts() }
            }
            // XAML:787-790. Shown only while the host can run a plug-in: on a
            // scan-only Mac host no plug-in has controls to open.
            if model.vstInsertsLive {
                Text("Select a VST3 plug-in above to open its controls.")
                    .font(.grotesk(10)).foregroundStyle(Studio.secondary)
                    .fixedSize(horizontal: false, vertical: true)
            }
            ForEach(buttons.filter { !$0.enabled }, id: \.title) { button in
                if let reason = button.reason {
                    Text("\(button.title): \(reason)").font(.grotesk(10)).foregroundStyle(Studio.textDim)
                        .fixedSize(horizontal: false, vertical: true)
                }
            }
        }
    }

    func gridButton(_ spec: AudioSetupPolicy.AddButton, action: @escaping () -> Void) -> some View {
        Button(action: action) {
            Text(spec.title).frame(maxWidth: .infinity)
        }
        .buttonStyle(GhostButtonStyle())
        .disabled(!spec.enabled)
        .opacity(spec.enabled ? 1 : 0.45)
        .help(spec.reason ?? spec.title)
        .frame(maxWidth: .infinity)
    }

    /// XAML:774-779 (DropDownButton) and AudioPage.xaml.cs
    /// OnAddProcessingVstFlyoutOpening: the scanned plug-ins.
    func vstMenu(_ spec: AudioSetupPolicy.AddButton) -> some View {
        Menu {
            if let line = model.setupVstMenuEmptyLine {
                Button(line) {}.disabled(true)
            } else {
                ForEach(model.vstPlugins.filter { $0.probe != "fail" }) { plugin in
                    Button(plugin.name) {
                        model.addProcessingInsert(AppModel.vstInsertName(plugin: plugin, className: plugin.classNames.first))
                    }
                }
            }
        } label: {
            Text(spec.title).font(.grotesk(13, .semibold)).frame(maxWidth: .infinity)
        }
        .menuStyle(.borderlessButton)
        .menuIndicator(.visible)
        .padding(.horizontal, 14).padding(.vertical, 9)
        .frame(maxWidth: .infinity)
        .overlay(RoundedRectangle(cornerRadius: 8).stroke(Studio.border, lineWidth: 1))
        .disabled(!spec.enabled)
        .opacity(spec.enabled ? 1 : 0.45)
        .help(spec.reason ?? spec.title)
    }
}

/// A SETUP slot's popover body. A channel slot edits the strip's settings
/// (InsertChainPolicy.paramSpecs, the same fields the SHOW cells edit); a bus
/// slot has no per-bus settings in the core (applyBusInsertChain runs fixed
/// values), so it shows the slot's status sentence.
struct SetupSlotFlyout: View {
    @EnvironmentObject var model: AppModel
    let stripId: String?
    let slot: InsertSlot

    var body: some View {
        let specs = stripId == nil ? [] : InsertChainPolicy.paramSpecs(slot.name)
        if specs.isEmpty {
            Text(stripId == nil ? slot.tooltip
                 : slot.isBuiltIn ? "No adjustable parameters."   // AudioPage.xaml.cs:219-227
                 : "Parameters appear here once the plug-in is processing. If the plug-in becomes unavailable, CoreVideo keeps program audio running.")
                .font(.grotesk(10)).foregroundStyle(Studio.secondary)
                .fixedSize(horizontal: false, vertical: true)
        } else if let stripId {
            let inserts = model.channelInserts[stripId] ?? ChannelInserts()
            ForEach(specs, id: \.key) { spec in
                let value = inserts.param(slot.name, key: spec.key) ?? spec.defaultValue
                VStack(alignment: .leading, spacing: 1) {
                    HStack {
                        Text(spec.label).font(.plexMono(9)).foregroundStyle(Studio.secondary)
                        Spacer()
                        Text(String(format: spec.step < 1 ? "%.1f" : "%.0f", value))
                            .font(.plexMono(9)).foregroundStyle(Studio.textPrimary)
                    }
                    Slider(value: Binding(get: { value }, set: { new in
                        model.editInserts(stripId) { $0.setParam(slot.name, key: spec.key, new) }
                    }).stepped(spec.step, in: spec.min...spec.max), in: spec.min...spec.max)
                        .controlSize(.small)
                }
            }
        }
    }
}

/// W/Views/OperatorTabResources.xaml:8-14 OperatorSectionBorder: StudioPanelBrush
/// (#101315, W/App.xaml:21 = Studio.panel), StudioBorderBrush, corner 8, padding 16.
private let setupSectionPadding: CGFloat = 16

private struct SetupSection<Content: View>: View {
    @ViewBuilder let content: Content

    var body: some View {
        content
            .padding(setupSectionPadding)
            .frame(maxWidth: .infinity, alignment: .topLeading)
            .background(RoundedRectangle(cornerRadius: 8).fill(Studio.panel))
            .overlay(RoundedRectangle(cornerRadius: 8).stroke(Studio.border, lineWidth: 1))
    }
}

/// OperatorTabResources.xaml:16 OperatorSectionTitle, based on StudioSectionTitle
/// (W/Themes/StudioTheme.xaml:38-42): 14, SemiBold.
private struct SetupSectionTitle: View {
    let text: String
    init(_ text: String) { self.text = text }
    var body: some View { Text(text).font(.grotesk(14, .semibold)).foregroundStyle(Studio.textPrimary) }
}

/// OperatorTabResources.xaml:31-38 OperatorReadoutBorder: #0C1118
/// (AudioSetupColor.card), StudioBorderBrush, corner 6, padding 10,8.
private struct SetupReadoutBorder: ViewModifier {
    func body(content: Content) -> some View {
        content
            .padding(.horizontal, 10).padding(.vertical, 8)
            .frame(maxWidth: .infinity, alignment: .leading)
            .background(RoundedRectangle(cornerRadius: 6).fill(AudioSetupColor.card))
            .overlay(RoundedRectangle(cornerRadius: 6).stroke(Studio.border, lineWidth: 1))
    }
}

/// The master rack's reflow (Task 5 review): how many stage columns fit.
enum MasteringRackLayout {
    /// The narrowest a stage card gets before its knob labels and values
    /// collide (the seven stages fit one row in the 820 column's 788).
    static let minStageWidth: CGFloat = 104
    static let stageSpacing: CGFloat = 8

    static func stageColumns(width: CGFloat, stages: Int = 7) -> Int {
        guard width.isFinite, width > 0 else { return stages }
        let fit = Int((width + stageSpacing) / (minStageWidth + stageSpacing))
        return max(1, min(stages, fit))
    }
}

/// Lays children left to right and wraps to a new line when the next one
/// does not fit. A child wider than the line is proposed the line's width.
struct WrapRowLayout: Layout {
    var spacing: CGFloat = 8
    var lineSpacing: CGFloat = 6

    private func arrange(_ maxWidth: CGFloat, _ subviews: Subviews) -> (frames: [CGRect], size: CGSize) {
        var frames: [CGRect] = []
        var x: CGFloat = 0, y: CGFloat = 0, lineHeight: CGFloat = 0, widest: CGFloat = 0
        for subview in subviews {
            var size = subview.sizeThatFits(.unspecified)
            if size.width > maxWidth {
                size = subview.sizeThatFits(ProposedViewSize(width: maxWidth, height: nil))
                size.width = min(size.width, maxWidth)
            }
            if x > 0, x + size.width > maxWidth {
                x = 0; y += lineHeight + lineSpacing; lineHeight = 0
            }
            frames.append(CGRect(origin: CGPoint(x: x, y: y), size: size))
            x += size.width + spacing
            lineHeight = max(lineHeight, size.height)
            widest = max(widest, x - spacing)
        }
        // Centre each child vertically on its line.
        var lineTop: CGFloat = -1, lineMax: CGFloat = 0, members: [Int] = []
        func flush() {
            for member in members { frames[member].origin.y += (lineMax - frames[member].height) / 2 }
            members = []; lineMax = 0
        }
        for index in frames.indices {
            if frames[index].minY != lineTop { flush(); lineTop = frames[index].minY }
            members.append(index); lineMax = max(lineMax, frames[index].height)
        }
        flush()
        return (frames, CGSize(width: widest, height: y + lineHeight))
    }

    func sizeThatFits(proposal: ProposedViewSize, subviews: Subviews, cache: inout ()) -> CGSize {
        let maxWidth = proposal.width ?? .infinity
        let size = arrange(maxWidth, subviews).size
        return CGSize(width: proposal.width ?? size.width, height: size.height)
    }

    func placeSubviews(in bounds: CGRect, proposal: ProposedViewSize, subviews: Subviews, cache: inout ()) {
        let frames = arrange(bounds.width, subviews).frames
        for (subview, frame) in zip(subviews, frames) {
            subview.place(at: CGPoint(x: bounds.minX + frame.minX, y: bounds.minY + frame.minY),
                          proposal: ProposedViewSize(frame.size))
        }
    }
}

/// The master rack's stage cards in MasteringRackLayout.stageColumns columns
/// of equal width; each row is as tall as its tallest card.
struct StageGridLayout: Layout {
    func sizeThatFits(proposal: ProposedViewSize, subviews: Subviews, cache: inout ()) -> CGSize {
        let width = proposal.width ?? 788
        return CGSize(width: width, height: rows(width, subviews).reduce(0) { $0 + $1.height }
                      + CGFloat(max(0, rows(width, subviews).count - 1)) * MasteringRackLayout.stageSpacing)
    }

    private func rows(_ width: CGFloat, _ subviews: Subviews) -> [(range: Range<Int>, height: CGFloat, column: CGFloat)] {
        let columns = MasteringRackLayout.stageColumns(width: width, stages: subviews.count)
        let spacing = MasteringRackLayout.stageSpacing
        let column = max(0, (width - CGFloat(columns - 1) * spacing) / CGFloat(columns))
        return stride(from: 0, to: subviews.count, by: columns).map { start in
            let range = start..<min(subviews.count, start + columns)
            let height = range.map { subviews[$0].sizeThatFits(ProposedViewSize(width: column, height: nil)).height }
                .max() ?? 0
            return (range, height, column)
        }
    }

    func placeSubviews(in bounds: CGRect, proposal: ProposedViewSize, subviews: Subviews, cache: inout ()) {
        var y = bounds.minY
        for row in rows(bounds.width, subviews) {
            for (offset, index) in row.range.enumerated() {
                let x = bounds.minX + CGFloat(offset) * (row.column + MasteringRackLayout.stageSpacing)
                subviews[index].place(at: CGPoint(x: x, y: y),
                                      proposal: ProposedViewSize(width: row.column, height: row.height))
            }
            y += row.height + MasteringRackLayout.stageSpacing
        }
    }
}

/// XAML:104-108 / :153-157: a device card, corner 6, padding 12, #0C1118.
private struct SetupCard<Content: View>: View {
    @ViewBuilder let content: Content

    var body: some View {
        content
            .padding(12)
            .frame(maxWidth: .infinity, alignment: .topLeading)
            .background(RoundedRectangle(cornerRadius: 6).fill(AudioSetupColor.card))
            .overlay(RoundedRectangle(cornerRadius: 6).stroke(Studio.border, lineWidth: 1))
    }
}

enum AudioSetupColor {
    static let card = Color(hex: 0x0C1118)   // XAML:104 device card Background
}

/// XAML:126-129 / :174-177: a full-width ToggleButton ("Monitor", "Source").
struct SetupToggleButton: View {
    let title: String
    @Binding var isOn: Bool

    var body: some View {
        Button { isOn.toggle() } label: {
            Text(title).font(.grotesk(12, .semibold)).frame(maxWidth: .infinity)
                .padding(.vertical, 6)
                .background(RoundedRectangle(cornerRadius: 4).fill(isOn ? Studio.blue : Studio.surfaceRaised))
                .overlay(RoundedRectangle(cornerRadius: 4).stroke(Studio.border, lineWidth: 1))
                .foregroundStyle(isOn ? Studio.onAccent : Studio.textPrimary)
                .contentShape(Rectangle())
        }
        .buttonStyle(.plain)
        .accessibilityLabel(title)
        .accessibilityValue(isOn ? "on" : "off")
    }
}
