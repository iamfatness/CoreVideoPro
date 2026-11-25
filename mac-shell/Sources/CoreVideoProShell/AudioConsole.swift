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
    @State private var surface = AudioPane.initialSurface.surface
    @State private var vstBrowserOpen = AudioPane.initialSurface.vstBrowserOpen
    /// XAML:62-73: the Windows pills. OD-2: the Mac-only PLUG-INS pill is
    /// folded into SETUP's collapsed "Advanced · VST3 plug-in browser".
    static let surfaces = ["SHOW", "ROUTING", "SETUP"]

    /// COREVIDEO_SHELL_AUDIO_SURFACE for the offscreen tab renderer: SHOW,
    /// ROUTING or SETUP; the retired PLUG-INS opens SETUP with the VST browser
    /// open, so existing snapshot scripts keep working. Anything else is SHOW.
    static func resolveSurface(_ wanted: String?) -> (surface: String, vstBrowserOpen: Bool) {
        if wanted == "PLUG-INS" { return ("SETUP", true) }
        guard let wanted, surfaces.contains(wanted) else { return ("SHOW", false) }
        return (wanted, false)
    }

    /// The running app always opens on SHOW.
    static var initialSurface: (surface: String, vstBrowserOpen: Bool) {
        guard ShellHarness.isActive else { return ("SHOW", false) }
        return resolveSurface(ProcessInfo.processInfo.environment["COREVIDEO_SHELL_AUDIO_SURFACE"])
    }
    // The selection is model state (AppModel.selectedAudioStripId, A7 Task 3):
    // the pop-out and the main window share it.
    // SHOW and SETUP share AppModel.effectiveAudioStrip.
    var selected: AudioStrip? { Self.selectedStrip(model) }
    static func selectedStrip(_ model: AppModel) -> AudioStrip? { model.effectiveAudioStrip }

    // OD-3 (XAML:31-36, :914-917): SHOW is a fixed viewport that fills the tab
    // and never scrolls; the page ScrollView made the console jump on every
    // snapshot. The other surfaces bring their own vertical scrolling, since
    // TabPage no longer wraps the Audio tab (TabPage.wrapsInScrollView).
    var body: some View {
        VStack(alignment: .leading, spacing: 8) {
            header.frame(height: 34)
            switch surface {
            case "ROUTING":
                ScrollView(.vertical) { AudioRoutingSurface() }
            case "SETUP":
                AudioSetupSurface(vstBrowserOpen: $vstBrowserOpen)
            default:
                showSurface
            }
        }
        .padding(10)
        .frame(maxWidth: .infinity, maxHeight: .infinity, alignment: .topLeading)
        // XAML:915: the SHOW console is #070B10; the other surfaces keep the panel.
        .background(RoundedRectangle(cornerRadius: 6)
            .fill(surface == "SHOW" ? AudioConsoleColor.console : Studio.panel))
        .overlay(RoundedRectangle(cornerRadius: 6).stroke(Studio.border, lineWidth: 1))
    }

    var header: some View {
        HStack(spacing: 8) {
            ForEach(Self.surfaces, id: \.self) { name in
                Button(name) { surface = name }
                    .buttonStyle(.plain)
                    .font(.plexMono(10, .semibold))
                    .foregroundStyle(surface == name ? Studio.onAccent : Studio.secondary)
                    .padding(.horizontal, 10).padding(.vertical, 5)
                    .background(RoundedRectangle(cornerRadius: 6)
                        .fill(surface == name ? Studio.blue : Studio.surface))
            }
            Spacer()
            // A1, Windows AudioPage.xaml:79-88: OFF = Zoom's own program mix rides
            // to master (echo-cancelled, continuous); ON = each guest's isolated
            // stem routes through their strip, so the faders shape program audio.
            Toggle(isOn: Binding(
                get: { model.zoomAudioMode == .perGuestIso },
                set: { model.setZoomAudioMode($0 ? .perGuestIso : .programMix) })) {
                Text(model.zoomAudioMode == .perGuestIso ? "Per-guest ISO" : "Zoom program mix")
                    .font(.plexMono(10, .semibold)).foregroundStyle(Studio.secondary)
            }
            .toggleStyle(.switch).controlSize(.small)
            .accessibilityLabel("Per-guest ISO audio to program")
            .help("Route each guest's isolated audio through their fader instead of Zoom's combined mix")
            // Real second-display mixer (was a disabled placeholder). Hidden
            // inside the pop-out so it cannot spawn copies of itself.
            if !isPopout {
                Button("Pop out mixer") { openWindow(id: PopoutWindow.audioMixer) }
                    .buttonStyle(GhostButtonStyle())
                    .help("Open the mixer on a second display")
            }
        }
    }

    /// XAML:919-1491: header, channel chips, the 255-tall workspace, then the
    /// strip bank and master rail filling the remaining height.
    var showSurface: some View {
        VStack(alignment: .leading, spacing: 8) {
            channelChips
            ProcessingWorkspace(stripId: selected?.id, stripName: selected.map(stripLabel) ?? "")
            HStack(alignment: .top, spacing: 8) {
                // XAML:1316-1431: a horizontal ScrollViewer of 120-wide strips.
                ScrollView(.horizontal) {
                    HStack(alignment: .top, spacing: 4) {
                        ForEach(model.strips) { strip in
                            ConsoleStrip(strip: strip, label: stripLabel(strip),
                                         selected: selected?.id == strip.id)
                        }
                    }
                    .frame(maxHeight: .infinity, alignment: .top)
                }
                .frame(maxWidth: .infinity, maxHeight: .infinity)
                // XAML:1434-1488: the master rail, pinned 150 wide at the right.
                MasterLufsRail()
                    .frame(width: AudioLayout.railWidth)
                    .frame(maxHeight: .infinity)
            }
            .frame(maxHeight: .infinity)
        }
        .frame(maxHeight: .infinity, alignment: .top)
    }

    /// XAML:946-975: one chip per channel, 88...170 wide, the name over the
    /// primary routed bus (SVM:11650-11661 ResolvePrimaryAudioBusLabel).
    var channelChips: some View {
        ScrollView(.horizontal, showsIndicators: false) {
            HStack(spacing: 4) {
                ForEach(model.strips) { strip in
                    let isSelected = selected?.id == strip.id
                    Button { model.selectAudioStrip(strip.id) } label: {
                        VStack(alignment: .leading, spacing: 0) {
                            Text(stripLabel(strip)).font(.grotesk(11, .semibold))
                                .foregroundStyle(AudioConsoleColor.title)
                                .lineLimit(1).truncationMode(.tail)
                            Text(model.primaryBusLabel(strip.id)).font(.plexMono(8))
                                .foregroundStyle(AudioConsoleColor.chipBus)
                                .lineLimit(1).truncationMode(.tail)
                        }
                        .frame(minWidth: 72, maxWidth: 154, alignment: .leading)
                        .fixedSize(horizontal: true, vertical: false)
                        .padding(.horizontal, 8).padding(.vertical, 4)
                        .background(RoundedRectangle(cornerRadius: 3).fill(AudioConsoleColor.chipFill))
                        .overlay(RoundedRectangle(cornerRadius: 3)
                            .stroke(isSelected ? AudioConsoleColor.title : AudioConsoleColor.chipStroke,
                                    lineWidth: 1))
                        .contentShape(Rectangle())
                    }
                    .buttonStyle(.plain)
                    .help(stripLabel(strip))
                    .accessibilityAddTraits(isSelected ? .isSelected : [])
                }
                if model.strips.isEmpty {
                    Text("Channels appear once sources carry PCM.")
                        .font(.grotesk(12)).foregroundStyle(Studio.secondary)
                }
            }
        }
    }

    func stripLabel(_ strip: AudioStrip) -> String { model.audioSourceLabel(strip.id) }
}

// Fader strip, XAML:1326-1424: 120 wide; PAN with its readout, MUTE / S, the
// signed gain label, the fader and meter stretching to the bank's height, and
// the name chip (a button that selects the channel).
struct ConsoleStrip: View {
    @EnvironmentObject var model: AppModel
    let strip: AudioStrip
    let label: String
    var selected = false

    var body: some View {
        VStack(spacing: 6) {
            VStack(spacing: 0) {
                Text("PAN").font(.plexMono(8)).tracking(0.6).foregroundStyle(Studio.secondary)
                Slider(value: Binding(
                    get: { strip.pan },
                    set: { value in model.editStrip(strip.id) { $0.pan = value } }).stepped(0.05, in: -1...1),
                    in: -1...1)
                    .controlSize(.mini)
                    .accessibilityLabel("\(label) pan")
                Text(ConsoleStripLabels.pan(strip.pan)).font(.plexMono(8)).foregroundStyle(Studio.secondary)
            }
            HStack(spacing: 3) {
                Button { model.editStrip(strip.id) { $0.muted.toggle() } } label: {
                    Text("MUTE").font(.plexMono(9, .bold)).frame(maxWidth: .infinity).padding(.vertical, 5)
                        .background(RoundedRectangle(cornerRadius: 4)
                            .fill(strip.muted ? Studio.red : Studio.surface))
                        .foregroundStyle(strip.muted ? Studio.textPrimary : Studio.secondary)
                        .contentShape(Rectangle())
                }
                .buttonStyle(.plain)
                .help("Mute in mix")
                Button { model.editStrip(strip.id) { $0.solo.toggle() } } label: {
                    Text("S").font(.plexMono(9, .bold)).frame(maxWidth: .infinity).padding(.vertical, 5)
                        .background(RoundedRectangle(cornerRadius: 4)
                            .fill(strip.solo ? Studio.amber : Studio.surface))
                        .foregroundStyle(strip.solo ? Studio.onAccent : Studio.secondary)
                        .contentShape(Rectangle())
                }
                .buttonStyle(.plain)
                .help("Solo in mix")
            }
            VStack(spacing: 1) {
                Text(ConsoleStripLabels.gain(strip.manualGainDb))
                    .font(.plexMono(10, .semibold)).foregroundStyle(Studio.textPrimary)
                if strip.sourceMuted {
                    // Display only (#481): the guest muted themselves in Zoom.
                    // XAML:1388-1392: FontSize 10, bold (the owner asked for 10-11 px).
                    Text("ZOOM MUTED").font(.plexMono(10, .bold)).foregroundStyle(Studio.amber)
                        .accessibilityLabel("\(label) is muted in Zoom")
                }
                if AudioConsolePolicy.isMeterOnly(strip.id, mode: model.zoomAudioMode) {
                    // A1: in ProgramMix the guest is inside zoom-mix, so this strip's
                    // mute and fader do not change program — say so (Windows status copy).
                    Text("METER ONLY").font(.plexMono(9, .semibold)).foregroundStyle(Studio.secondary)
                        .help("Zoom program mix is on: this guest reaches program through Zoom's combined mix")
                }
            }
            // XAML:1394-1411: fader (*) beside a 42-wide meter with its dBFS
            // scale, both stretching to the bank's height.
            HStack(spacing: 4) {
                GeometryReader { geo in
                    Slider(value: Binding(
                        get: { strip.manualGainDb },
                        set: { value in model.editStrip(strip.id) { $0.manualGainDb = value } })
                            .stepped(0.5, in: -24...24),
                        in: -24...24)
                        .frame(width: geo.size.height)
                        .rotationEffect(.degrees(-90))
                        .frame(width: geo.size.width, height: geo.size.height)
                        .accessibilityLabel("\(label) gain")
                }
                let reading = AudioConsolePolicy.meterReading(strip)
                HStack(spacing: 3) {
                    SegmentedMeter(level: reading.level, dimmed: reading.dimmed).frame(width: 14)
                    MeterTickScale().frame(width: 22)
                }
                .frame(width: 42)
                .help(ConsoleStripLabels.meterTooltip(muted: strip.muted, sourceMuted: strip.sourceMuted))
                .accessibilityLabel(label)
            }
            .frame(maxHeight: .infinity)
            Button { model.selectAudioStrip(strip.id) } label: {
                Text(label).font(.grotesk(10, .semibold))
                    .foregroundStyle(AudioConsoleColor.nameText)
                    .lineLimit(1).truncationMode(.tail)
                    .frame(maxWidth: .infinity)
                    .padding(.horizontal, 2).padding(.vertical, 4)
                    .background(RoundedRectangle(cornerRadius: 3).fill(AudioConsoleColor.nameFill))
                    .overlay(RoundedRectangle(cornerRadius: 3)
                        .stroke(selected ? AudioConsoleColor.title : AudioConsoleColor.chipStroke, lineWidth: 1))
                    .contentShape(Rectangle())
            }
            .buttonStyle(.plain)
            .help(label)
        }
        .padding(.horizontal, 8).padding(.top, 8).padding(.bottom, 6)
        .frame(width: AudioLayout.stripWidth)
        .frame(maxHeight: .infinity)
        .background(RoundedRectangle(cornerRadius: 3).fill(AudioConsoleColor.stripFill))   // XAML:1326
        .overlay(RoundedRectangle(cornerRadius: 3).stroke(Studio.border, lineWidth: 1))
    }
}

// Master rail, XAML:1434-1488: 150 wide (the caller pins the width), meters
// stretching to the bank's height, Monitor and its volume, L / R.
struct MasterLufsRail: View {
    @EnvironmentObject var model: AppModel

    /// Windows MasterLufsLabel: the core's loudnessLufs (short-term, else momentary).
    var lufsLabel: String {
        MasterMeterReadout.railLufsLabel(loudnessLufs: model.loudnessLufs, mixedFrames: model.mixedFrameCount)
    }

    var body: some View {
        VStack(spacing: 6) {
            Text("MASTER").font(.plexMono(11, .heavy)).tracking(0.6).foregroundStyle(AudioConsoleColor.title)
            Text(lufsLabel).font(.grotesk(22, .bold))
                .lineLimit(1).minimumScaleFactor(0.6)
                .accessibilityLabel("Master loudness \(lufsLabel)")
            // #870: the clock re-evaluates the observation age between snapshots,
            // so a stalled audio worker cannot leave held meters looking live.
            TimelineView(.periodic(from: .now, by: 0.25)) { context in
                if let warning = model.meterWarning(now: context.date) {
                    Text(warning).font(.grotesk(10, .medium)).foregroundStyle(Studio.amber)
                        .multilineTextAlignment(.center).lineLimit(2)
                } else {
                    Text(model.masterLevel > 0 ? "Live" : "Standing by")
                        .font(.grotesk(10)).foregroundStyle(Studio.secondary)
                }
            }
            // L and R from the pgm-l / pgm-r bus taps (StudioFooterReadout.masterLevels,
            // Windows ResolveMasterMeterReadout) — no longer the mono level twice.
            HStack(spacing: 6) {
                MeterTickScale().frame(width: 20)
                SegmentedMeter(level: model.masterLevelLeft).frame(width: 16)
                    .accessibilityLabel("Master left")
                SegmentedMeter(level: model.masterLevelRight).frame(width: 16)
                    .accessibilityLabel("Master right")
            }
            .frame(maxHeight: .infinity)
            VStack(spacing: 2) {
                Toggle("Monitor", isOn: Binding(
                    get: { model.monitorEnabled },
                    set: { model.setMonitor(enabled: $0, volume: model.monitorVolume) }))
                    .toggleStyle(.switch)
                    .controlSize(.small)
                    .font(.grotesk(11))
                Slider(value: Binding(
                    get: { model.monitorVolume },
                    set: { model.setMonitor(enabled: model.monitorEnabled, volume: $0) }),
                    in: 0...1)
                    .controlSize(.mini)
                    .accessibilityLabel("Monitor volume")
                Text("\(Int(model.monitorVolume * 100))%")
                    .font(.plexMono(9)).foregroundStyle(Studio.secondary)
            }
            Text("L / R").font(.plexMono(10, .semibold)).foregroundStyle(AudioConsoleColor.nameText)
                .frame(maxWidth: .infinity)
                .padding(.vertical, 4)
                .background(RoundedRectangle(cornerRadius: 3).fill(AudioConsoleColor.nameFill))
        }
        .padding(.horizontal, 10).padding(.top, 8).padding(.bottom, 6)
        .frame(maxWidth: .infinity, maxHeight: .infinity)
        .background(RoundedRectangle(cornerRadius: 3).fill(AudioConsoleColor.railFill))   // XAML:1443
        .overlay(RoundedRectangle(cornerRadius: 3).stroke(AudioConsoleColor.chipStroke, lineWidth: 1))
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

/// SETUP guide meters (Windows AudioPage.xaml:349-380, MasteringGuideMeter):
/// integrated loudness against the target, true peak against the ceiling.
/// Measured post-master values only; no evidence reads a dash.
struct MasteringGuideMeters: View {
    @EnvironmentObject var model: AppModel

    var body: some View {
        VStack(alignment: .leading, spacing: 4) {
            // #870: a stalled core or audio worker cannot leave held values
            // looking live — the Windows MeterWarning words, re-evaluated on a clock.
            TimelineView(.periodic(from: .now, by: 0.25)) { context in
                if let warning = model.meterWarning(now: context.date) {
                    Text(warning).font(.grotesk(11, .medium)).foregroundStyle(Studio.amber)
                }
            }
            meters
        }
    }

    var meters: some View {
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

    static let cellWidth: CGFloat = 58
    static let sourceWidth: CGFloat = 150

    var body: some View {
        VStack(alignment: .leading, spacing: 8) {
            Text("Every crosspoint carries a level. Click to route or select; "
                 + "gain edits apply to the selected send. ISO buses are metered routing taps; "
                 + "ISO recordings take each guest's own audio.")
                .font(.grotesk(12)).foregroundStyle(Studio.secondary)
            if !model.audioRouteControlNotice.isEmpty {
                Text(model.audioRouteControlNotice)
                    .font(.grotesk(12)).foregroundStyle(Studio.amber)
            }
            AudioBusStrip()
            if sources.isEmpty {
                Text("Sources appear here once channels carry PCM.")
                    .font(.grotesk(12)).foregroundStyle(Studio.textDim)
            }
            // Source column pinned; the bus columns scroll (15+ columns do not fit).
            HStack(alignment: .top, spacing: 2) {
                VStack(alignment: .leading, spacing: 2) {
                    Color.clear.frame(width: Self.sourceWidth, height: 28)
                    ForEach(sources, id: \.id) { source in
                        HStack(spacing: 4) {
                            Text(source.label).font(.grotesk(12)).lineLimit(1)
                            Spacer(minLength: 0)
                            Button("Clear") { model.clearRoutes(source: source.id) }
                                .buttonStyle(.plain).font(.plexMono(8)).foregroundStyle(Studio.textDim)
                                .help("Unroute this source from every bus")
                        }
                        .frame(width: Self.sourceWidth - 6, height: 24, alignment: .leading)
                        .padding(.leading, 6)
                        .background(Rectangle().fill(Studio.surface))
                    }
                }
                ScrollView(.horizontal) {
                    Grid(horizontalSpacing: 2, verticalSpacing: 2) {
                        GridRow {
                            ForEach(model.buses) { bus in
                                VStack(spacing: 0) {
                                    MonoLabel(bus.label).lineLimit(1)
                                    Text("\(model.routedCount(bus: bus.id))")
                                        .font(.plexMono(8)).foregroundStyle(Studio.textDim)
                                }
                                .frame(width: Self.cellWidth, height: 28)
                                .help("\(model.routedCount(bus: bus.id)) source(s) routed to \(bus.label)")
                            }
                        }
                        ForEach(sources, id: \.id) { source in
                            GridRow {
                                ForEach(model.buses) { bus in
                                    cell(source: source.id, bus: bus.id)
                                }
                            }
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

    func cell(source: String, bus: String) -> some View {
        let gain = model.sendGain(source: source, bus: bus)
        let selected = model.selectedSend?.source == source && model.selectedSend?.bus == bus
        return Button {
            model.selectOrRouteSend(source: source, bus: bus)
        } label: {
            Text(gain.map { String(format: "%.1f", $0) } ?? "·")
                .font(.plexMono(9, gain != nil ? .semibold : .regular))
                .foregroundStyle(gain != nil ? Studio.accent : Studio.secondary)
                .frame(maxWidth: .infinity).frame(height: 24)
                .background(Rectangle().fill(gain != nil ? Studio.accent.opacity(0.16) : Studio.field))
                .overlay(Rectangle().stroke(
                    selected ? Studio.textPrimary : gain != nil ? Studio.accent.opacity(0.7) : Studio.border,
                    lineWidth: selected ? 2 : 1))
        }
        .buttonStyle(.plain)
        .frame(width: Self.cellWidth)
    }
}

/// One chip per bus (Windows AudioPage.xaml:846-906): label (editable for aux and
/// custom buses), routed count, LISTEN, and for aux/custom buses the MASTER /
/// MON / STREAM sends with their shared gain and a remove button; "+ Bus" adds a
/// custom bus.
struct AudioBusStrip: View {
    @EnvironmentObject var model: AppModel

    var body: some View {
        VStack(alignment: .leading, spacing: 4) {
            ScrollView(.horizontal) {
                HStack(spacing: 6) {
                    ForEach(model.buses) { bus in chip(bus) }
                    Button("+ Bus") { model.addCustomBus() }
                        .buttonStyle(GhostButtonStyle())
                        .help("Add a custom bus")
                }
            }
            Text("LISTEN plays one bus on the monitor output while Monitor is on.")
                .font(.grotesk(11)).foregroundStyle(Studio.textDim)
        }
    }

    func chip(_ bus: AudioBus) -> some View {
        let listening = model.listenBusId == bus.id
        return VStack(alignment: .leading, spacing: 4) {
            HStack(spacing: 4) {
                if bus.isRemovable {
                    TextField("Bus", text: Binding(get: { bus.label },
                                                   set: { model.renameBus(bus.id, to: $0) }))
                        .textFieldStyle(.plain).font(.plexMono(9, .semibold)).frame(width: 64)
                } else {
                    MonoLabel(bus.label)
                }
                Text("\(model.routedCount(bus: bus.id))").font(.plexMono(8)).foregroundStyle(Studio.textDim)
                Button("LISTEN") { model.setListening(bus.id, on: !listening) }
                    .buttonStyle(.plain).font(.plexMono(8, .semibold))
                    .padding(.horizontal, 4).padding(.vertical, 1)
                    .background(RoundedRectangle(cornerRadius: 3).fill(listening ? Studio.amber : Studio.surface))
                    .foregroundStyle(listening ? Color.black : Studio.secondary)
                    .accessibilityLabel("Listen to \(bus.label)")
            }
            if bus.isRemovable {
                HStack(spacing: 3) {
                    Text("→").font(.plexMono(9)).foregroundStyle(Studio.textDim)
                    sendToggle(bus, "master", "MASTER", bus.sendsToMaster)
                    sendToggle(bus, "mon", "MON", bus.sendsToMonitor)
                    sendToggle(bus, "stream", "STREAM", bus.sendsToStream)
                    Button("×") { model.removeBus(bus.id) }
                        .buttonStyle(.plain).font(.plexMono(10)).foregroundStyle(Studio.red)
                        .accessibilityLabel("Remove \(bus.label)")
                }
                HStack(spacing: 4) {
                    Slider(value: Binding(get: { bus.outputGainDb },
                                          set: { model.setBusOutputGain(bus.id, gainDb: $0) }), in: -60...10)
                        .frame(width: 110)
                    Text(String(format: "%.1f dB", bus.outputGainDb)).font(.plexMono(8)).foregroundStyle(Studio.secondary)
                }
            }
        }
        .padding(6)
        .background(RoundedRectangle(cornerRadius: 6).fill(Studio.field))
        .overlay(RoundedRectangle(cornerRadius: 6).stroke(listening ? Studio.amber : Studio.border, lineWidth: 1))
    }

    func sendToggle(_ bus: AudioBus, _ target: String, _ title: String, _ on: Bool) -> some View {
        Button(title) { model.setBusSend(bus.id, target: target, enabled: !on) }
            .buttonStyle(.plain).font(.plexMono(8, .semibold))
            .padding(.horizontal, 3).padding(.vertical, 1)
            .background(RoundedRectangle(cornerRadius: 3).fill(on ? Studio.accent.opacity(0.25) : Studio.surface))
            .foregroundStyle(on ? Studio.accent : Studio.textDim)
            .accessibilityLabel("\(bus.label) sends to \(title)")
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
            // Reflows (Task 5 review): the stages take as many equal columns as
            // fit (MasteringRackLayout.stageColumns), so nothing clips at 900.
            StageGridLayout {
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
        WrapRowLayout {
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
                .fixedSize(horizontal: false, vertical: true)
        }
    }

    var presetsRow: some View {
        WrapRowLayout {
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
        .frame(maxWidth: .infinity, maxHeight: .infinity, alignment: .topLeading)
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
