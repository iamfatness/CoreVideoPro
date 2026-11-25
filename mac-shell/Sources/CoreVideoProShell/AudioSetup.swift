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

    /// XAML:690-707 readouts for the selected strip. SVM:2200-2213
    /// (SelectedGainLabel, `{gain:0} dB` with a sign), SVM:2215-2227
    /// (SelectedManualGainLabel, 0 = "Auto"), SVM:2233 (SelectedOutputLevel,
    /// the core's 0..100). The Mac strip has ONE gain (manualGainDb, the fader
    /// the core applies), so both gain readouts read it. A bus target, or no
    /// strip, reads a dash.
    static func readouts(strip: AudioStrip?, targetIsBus: Bool) -> (gain: String, manual: String, output: String) {
        guard !targetIsBus, let strip else { return ("—", "—", "—") }
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

    var selectedTargetUnroutedNote: String? {
        AudioSetupPolicy.unroutedNote(target: selectedProcessingTargetId, routedBusIds: busesWithSourceSends)
    }
}

/// XAML:37-797. The header row (pills, Zoom mode, pop-out) sits above the
/// surface switch in AudioPane and spans both columns, as on Windows.
struct AudioSetupSurface: View {
    @EnvironmentObject var model: AppModel
    @Binding var vstBrowserOpen: Bool

    /// The master rack keeps the width the capped 820 column gives it
    /// (820 - 2 x 12 padding). In a narrower column (the 900-wide pop-out gives
    /// it ~500) the rack scrolls sideways inside the column instead of
    /// crushing its seven stages' labels letter by letter.
    static let rackMinWidth: CGFloat = AudioLayout.setupRightMaxWidth - 2 * 12

    /// The rack's content width in a surface `total` wide: the right column
    /// (capped at 820) less the section padding.
    static func rackWidth(surface total: CGFloat) -> CGFloat {
        let right = min(AudioLayout.setupRightMaxWidth,
                        total - AudioLayout.setupLeftWidth - AudioLayout.setupColumnSpacing - 8)
        return right - 2 * 12
    }

    var body: some View {
        GeometryReader { geo in
            let rackScrolls = Self.rackWidth(surface: geo.size.width) < Self.rackMinWidth
            ScrollView(.vertical) {
                // XAML:44-48: 340 | * (MaxWidth 820), ColumnSpacing 12.
                HStack(alignment: .top, spacing: AudioLayout.setupColumnSpacing) {
                    devicesColumn.frame(width: AudioLayout.setupLeftWidth)
                    processingColumn(rackScrolls: rackScrolls)
                        .frame(maxWidth: AudioLayout.setupRightMaxWidth, alignment: .topLeading)
                }
                .frame(maxWidth: .infinity, alignment: .topLeading)
                .padding(.trailing, 8).padding(.bottom, 16)   // XAML:38 Padding="0,0,8,16"
            }
        }
    }

    // ── Left: Devices & monitoring (XAML:102-181) ───────────────────────────

    var devicesColumn: some View {
        SetupSection {
            VStack(alignment: .leading, spacing: 10) {
                Text("Devices & monitoring").font(.grotesk(13, .semibold))
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
                    MonitorDevicePicker(width: AudioLayout.setupLeftWidth - 2 * 12 - 2 * 12)
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

    func processingColumn(rackScrolls: Bool) -> some View {
        SetupSection {
            VStack(alignment: .leading, spacing: 10) {
                // XAML:193-205. The MASTER IN toggle is the existing one in the
                // rack's own header row (MasteringRack, unchanged).
                VStack(alignment: .leading, spacing: 2) {
                    Text("Master processor").font(.grotesk(13, .semibold))
                    Text("Program L/R · processing order")   // XAML:199
                        .font(.grotesk(11)).foregroundStyle(Studio.secondary)
                }
                MasteringGuideMeters()
                if rackScrolls {
                    ScrollView(.horizontal) {
                        MasteringRack().frame(width: Self.rackMinWidth)
                    }
                } else {
                    MasteringRack()
                }
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
    var strip: AudioStrip? { parsed.channel.flatMap { id in model.strips.first { $0.id == id } } }

    var body: some View {
        VStack(alignment: .leading, spacing: 10) {
            Text("Inserts & plug-ins").font(.grotesk(13, .semibold))
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
            if let strip, parsed.bus == nil {
                Button(AudioSetupPolicy.muteLabel(muted: strip.muted)) {
                    model.editStrip(strip.id) { $0.muted.toggle() }
                }
                .buttonStyle(GhostButtonStyle())
            }
        }
    }

    var readouts: some View {
        let values = AudioSetupPolicy.readouts(strip: strip, targetIsBus: parsed.bus != nil)
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
        .padding(.horizontal, 10).padding(.vertical, 6)
        .frame(maxWidth: .infinity, alignment: .leading)
        .background(RoundedRectangle(cornerRadius: 4).fill(Studio.field))
        .overlay(RoundedRectangle(cornerRadius: 4).stroke(Studio.border, lineWidth: 1))
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
        .padding(.horizontal, 10).padding(.vertical, 6)
        .frame(maxWidth: .infinity, alignment: .leading)
        .background(RoundedRectangle(cornerRadius: 4).fill(Studio.field))
        .overlay(RoundedRectangle(cornerRadius: 4).stroke(Studio.border, lineWidth: 1))
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
            if model.vstPlugins.isEmpty {
                Button("No VST3 plugins found on this machine") {}.disabled(true)
            }
            ForEach(model.vstPlugins.filter { $0.probe != "fail" }) { plugin in
                Button(plugin.name) {
                    model.addProcessingInsert(AppModel.vstInsertName(plugin: plugin, className: plugin.classNames.first))
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

/// XAML OperatorSectionBorder: the column panel.
private struct SetupSection<Content: View>: View {
    @ViewBuilder let content: Content

    var body: some View {
        content
            .padding(12)
            .frame(maxWidth: .infinity, alignment: .topLeading)
            .background(RoundedRectangle(cornerRadius: 6).fill(Studio.surface))
            .overlay(RoundedRectangle(cornerRadius: 6).stroke(Studio.border, lineWidth: 1))
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
