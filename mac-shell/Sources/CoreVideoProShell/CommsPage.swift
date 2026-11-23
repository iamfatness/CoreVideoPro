// The Comms (Zoom talkback) page, in the STRUCTURE of Windows
// native-shell/CoreVideoPro.WinUI/Views/CommsPage.xaml:51-369: five rows,
// Auto / * / Auto / Auto / Auto.
//   1. rail (fixed)        title, rail text, four lamps
//   2. desk (fills)        the only part that scrolls
//   3. settings drawer     fixed, when open
//   4. strip               fixed, when the desk is visible
//   5. ops strip           fixed
// The page owns its scrolling (TabPageLayout.fillsHeight), so the strip and
// the ops strip stay pinned at the bottom however many cells there are.
// Colours come only from Studio tokens (CommsColors, the CommsBrushConverter
// mapping). Copy is Windows'.

import SwiftUI

/// CommsBrushConverter.cs:35-79 onto Studio tokens. Pure, so the harness pins it.
enum CommsColors {
    struct Triple: Equatable { let background: Color; let border: Color; let text: Color }

    static func cell(_ tone: CommsTone) -> Triple {
        switch tone {
        case .hot: return Triple(background: Studio.red, border: Studio.red, text: Studio.background)
        case .armed: return Triple(background: Studio.surface, border: Studio.amber, text: Studio.amber)
        case .ready: return Triple(background: Studio.surface, border: Studio.border, text: Studio.textPrimary)
        case .idle: return Triple(background: Studio.surface, border: Studio.border, text: Studio.secondary)
        case .off: return Triple(background: Studio.field, border: Studio.line2, text: Studio.textDim)
        }
    }

    static func meter(_ tone: CommsTone) -> Color {
        switch tone {
        case .hot: return Studio.red
        case .armed: return Studio.amber
        default: return Studio.accent
        }
    }

    static func lamp(_ on: Bool) -> Color { on ? Studio.accent : Studio.textDim }
}

struct CommsPage: View {
    @ObservedObject var model: CommsModel
    var onLeave: () -> Void = {}

    var body: some View {
        VStack(spacing: 0) {
            CommsRail(model: model)
            ScrollView(.vertical) {
                CommsDesk(model: model)
                    .padding(EdgeInsets(top: 0, leading: 16, bottom: 8, trailing: 16))
                    .frame(maxWidth: .infinity, alignment: .leading)
            }
            .frame(maxHeight: .infinity)
            .background(GeometryReader { Color.clear.preference(key: CommsDeskFrameKey.self, value: $0.frame(in: .named("comms"))) })
            if model.settingsOpen && model.deskVisible { CommsSettingsDrawer(model: model) }
            if model.deskVisible { CommsStrip(model: model, onLeave: onLeave) }
            CommsOpsStrip(model: model)
                .background(GeometryReader { Color.clear.preference(key: CommsOpsFrameKey.self, value: $0.frame(in: .named("comms"))) })
        }
        .frame(maxWidth: .infinity, maxHeight: .infinity)
        .coordinateSpace(name: "comms")
    }
}

// Frames the layout test reads (comms/page-owns-its-scrolling).
struct CommsOpsFrameKey: PreferenceKey {
    static let defaultValue: CGRect = .zero
    // Siblings that never set the key contribute .zero; keep the measured one.
    static func reduce(value: inout CGRect, nextValue: () -> CGRect) {
        let next = nextValue()
        if next != .zero { value = next }
    }
}

struct CommsCellFramesKey: PreferenceKey {
    static let defaultValue: [Int: CGRect] = [:]
    static func reduce(value: inout [Int: CGRect], nextValue: () -> [Int: CGRect]) {
        value.merge(nextValue(), uniquingKeysWith: { $1 })
    }
}

struct CommsDeskFrameKey: PreferenceKey {
    static let defaultValue: CGRect = .zero
    // Siblings that never set the key contribute .zero; keep the measured one.
    static func reduce(value: inout CGRect, nextValue: () -> CGRect) {
        let next = nextValue()
        if next != .zero { value = next }
    }
}

// ── 1. rail ──────────────────────────────────────────────────────────────

struct CommsRail: View {
    @ObservedObject var model: CommsModel

    var body: some View {
        VStack(spacing: 0) {
            HStack(spacing: 12) {
                Text("Comms").font(.grotesk(16, .semibold)).foregroundStyle(Studio.textPrimary)
                Text(model.railText).font(.plexMono(11)).foregroundStyle(Studio.secondary)
                    .lineLimit(1).truncationMode(.tail)
                Spacer(minLength: 8)
                HStack(spacing: 16) {
                    CommsLamp(tag: "LINK", on: model.lampLink, help: "In the meeting")
                    CommsLamp(tag: "MTG MIC", on: model.lampMic,
                              help: "This participant's meeting audio is open. Zoom delivers talkback only while it is.")
                    CommsLamp(tag: "CHANNEL", on: model.lampChannel, help: "At least one talkback channel is ready")
                    CommsLamp(tag: "TX", on: model.lampTx, help: "Sending to a channel someone is in")
                }
            }
            .padding(EdgeInsets(top: 4, leading: 16, bottom: 10, trailing: 16))
            Rectangle().fill(Studio.border).frame(height: 1)
        }
    }
}

struct CommsLamp: View {
    let tag: String
    let on: Bool
    let help: String

    var body: some View {
        HStack(spacing: 6) {
            Circle().fill(CommsColors.lamp(on)).frame(width: 8, height: 8)
            Text(tag).font(.plexMono(10, .medium)).foregroundStyle(on ? Studio.textPrimary : Studio.textDim)
        }
        .help(help)
        .accessibilityElement(children: .ignore)
        .accessibilityLabel("\(tag) \(on ? "on" : "off")")
    }
}

// ── 2. desk ──────────────────────────────────────────────────────────────

/// The three non-desk states, Windows' copy. Nothing for .desk.
struct CommsStateBlock: View {
    let state: CommsPageState

    static let notInMeetingTitle = "Not in a meeting"
    static let notInMeetingBody = "Join from the Zoom page. Talkback comes up once this account is in the meeting as host or co-host."
    static let waitingText = "Waiting for the Zoom engine's talkback report…"
    static let unavailableTitle = "Talkback is not up"
    static let hostHint = "Ask the meeting host to make this account host or co-host. Video keeps working meanwhile."

    var body: some View {
        switch state {
        case .notInMeeting:
            VStack(spacing: 6) {
                Text(Self.notInMeetingTitle).font(.grotesk(15, .semibold)).foregroundStyle(Studio.textPrimary)
                Text(Self.notInMeetingBody).font(.grotesk(12)).foregroundStyle(Studio.secondary)
                    .multilineTextAlignment(.center)
            }
            .frame(maxWidth: .infinity).padding(.top, 40)
        case .waiting:
            Text(Self.waitingText).font(.grotesk(12)).foregroundStyle(Studio.secondary)
                .frame(maxWidth: .infinity).padding(.top, 40)
        case .unavailable(let text, let hostHint):
            VStack(spacing: 6) {
                Text(Self.unavailableTitle).font(.grotesk(15, .semibold)).foregroundStyle(Studio.textPrimary)
                Text(text).font(.grotesk(12)).foregroundStyle(Studio.textPrimary).multilineTextAlignment(.center)
                if hostHint {
                    Text(Self.hostHint).font(.grotesk(12)).foregroundStyle(Studio.secondary).multilineTextAlignment(.center)
                }
            }
            .frame(maxWidth: .infinity).padding(.top, 40)
        case .desk:
            EmptyView()
        }
    }
}

struct CommsDesk: View {
    @ObservedObject var model: CommsModel
    static let columns = [GridItem(.adaptive(minimum: 200), spacing: 8)]
    static let hint = "Hold a cell to talk · 1–9 key the first nine · Space = ALL CALL · LATCH makes presses stick"

    var body: some View {
        VStack(alignment: .leading, spacing: 12) {
            CommsStateBlock(state: model.pageState)
            if model.deskVisible {
                let tone = CommsColors.cell(model.allTone)
                TalkKey(enabled: true,
                        onPress: { Task { await model.allPress() } },
                        onRelease: { Task { await model.allRelease() } }) {
                    Text(model.allText).font(.plexMono(13, .medium)).foregroundStyle(tone.text)
                        .frame(maxWidth: .infinity, minHeight: 56)
                        .background(RoundedRectangle(cornerRadius: 8).fill(tone.background))
                        .overlay(RoundedRectangle(cornerRadius: 8).stroke(tone.border, lineWidth: 2))
                }
                .help("Hold to talk to everyone on a channel (Space)")
                .accessibilityLabel(model.allText)
                if let empty = model.emptyDeskText {
                    Text(empty).font(.grotesk(12)).foregroundStyle(Studio.secondary)
                        .frame(maxWidth: .infinity).padding(.vertical, 16)
                }
                if model.gridVisible {
                    LazyVGrid(columns: Self.columns, spacing: 8) {
                        ForEach(model.cells) { cell in CommsCellKey(model: model, cell: cell) }
                    }
                }
                if model.editVisible { CommsTalentEditor(model: model) }
                Text(Self.hint).font(.grotesk(11)).foregroundStyle(Studio.textDim)
            }
        }
        .padding(.top, 16)
    }
}

struct CommsCellKey: View {
    @ObservedObject var model: CommsModel
    let cell: CommsCell

    var body: some View {
        let tone = CommsColors.cell(cell.tone)
        TalkKey(enabled: !cell.off,
                onPress: { Task { await model.press(slot: cell.slot) } },
                onRelease: { Task { await model.release(slot: cell.slot) } }) {
            VStack(alignment: .leading, spacing: 6) {
                HStack(alignment: .firstTextBaseline) {
                    Text(cell.name).font(.grotesk(14, .semibold)).lineLimit(1).truncationMode(.tail).help(cell.name)
                    Spacer(minLength: 4)
                    if let hint = cell.keyHint {
                        Text(hint).font(.plexMono(10, .medium)).foregroundStyle(tone.text.opacity(0.8)).help("Digit key")
                    }
                }
                Text(cell.stateText).font(.plexMono(11))
            }
            .foregroundStyle(tone.text)
            .padding(EdgeInsets(top: 12, leading: 14, bottom: 12, trailing: 14))
            .frame(maxWidth: .infinity, minHeight: 72, alignment: .topLeading)
            .background(RoundedRectangle(cornerRadius: 8).fill(tone.background))
            .overlay(RoundedRectangle(cornerRadius: 8).stroke(tone.border, lineWidth: 2))
        }
        .accessibilityElement(children: .ignore)
        .accessibilityLabel(cell.automationName)
        .background(GeometryReader {
            Color.clear.preference(key: CommsCellFramesKey.self, value: [cell.slot: $0.frame(in: .named("comms"))])
        })
    }
}

struct CommsTalentEditor: View {
    @ObservedObject var model: CommsModel

    var body: some View {
        VStack(alignment: .leading, spacing: 10) {
            HStack {
                MonoLabel("TALENT")
                Spacer()
                MonoLabel("CHANNEL: CLICK A NUMBER TO PUT SOMEONE ON IT")
            }
            ForEach(model.people) { person in
                HStack(alignment: .top, spacing: 12) {
                    VStack(alignment: .leading, spacing: 4) {
                        Text(person.name).font(.grotesk(13, .semibold)).foregroundStyle(Studio.textPrimary)
                            .lineLimit(1).truncationMode(.tail).help(person.name)
                        if !person.talkback {
                            Text(CommsModel.webClientWhy).font(.grotesk(11)).foregroundStyle(Studio.secondary)
                        }
                    }
                    Spacer(minLength: 8)
                    if person.talkback {
                        LazyVGrid(columns: [GridItem(.adaptive(minimum: 32, maximum: 32), spacing: 4)], spacing: 4) {
                            ForEach(person.chips, id: \.slot) { chip in
                                Button(chip.text) {
                                    Task { await model.assign(slot: chip.slot, participantId: person.participantId, on: !chip.on) }
                                }
                                .buttonStyle(CommsChipStyle(on: chip.on, busy: chip.busy))
                                .help(chip.tip)
                                .accessibilityLabel(chip.tip)
                            }
                        }
                        .frame(width: 560)
                    } else {
                        Text("NO TALKBACK").font(.plexMono(10, .medium)).foregroundStyle(Studio.textDim)
                    }
                }
                .padding(.vertical, 4)
            }
        }
        .padding(14)
        .overlay(RoundedRectangle(cornerRadius: 8).stroke(Studio.border, lineWidth: 1))
    }
}

struct CommsChipStyle: ButtonStyle {
    let on: Bool
    let busy: Bool

    func makeBody(configuration: Configuration) -> some View {
        configuration.label
            .font(.plexMono(11, .medium))
            .foregroundStyle(on ? Studio.background : Studio.textPrimary)
            .frame(width: 32, height: 32)
            .background(RoundedRectangle(cornerRadius: 6).fill(on ? Studio.accent : Studio.surface))
            .overlay(RoundedRectangle(cornerRadius: 6).stroke(busy ? Studio.amber : Studio.border, lineWidth: 1))
            .contentShape(RoundedRectangle(cornerRadius: 6))
    }
}

/// Windows CommsToggleButton: accent border and raised fill when on.
struct CommsToggleStyle: ButtonStyle {
    let on: Bool

    func makeBody(configuration: Configuration) -> some View {
        configuration.label
            .font(.grotesk(12, .semibold))
            .foregroundStyle(Studio.textPrimary)
            .padding(.horizontal, 12).padding(.vertical, 7)
            .background(RoundedRectangle(cornerRadius: 8).fill(on || configuration.isPressed ? Studio.surfaceRaised : .clear))
            .overlay(RoundedRectangle(cornerRadius: 8).stroke(on ? Studio.accent : Studio.border, lineWidth: 1))
            .contentShape(RoundedRectangle(cornerRadius: 8))
    }
}

// ── 3. settings drawer ───────────────────────────────────────────────────

struct CommsSettingsDrawer: View {
    @ObservedObject var model: CommsModel

    var body: some View {
        VStack(spacing: 0) {
            Rectangle().fill(Studio.border).frame(height: 1)
            HStack(alignment: .top, spacing: 20) {
                VStack(alignment: .leading, spacing: 6) {
                    MonoLabel("MICROPHONE")
                    devicePicker(model.mics, selected: model.selectedMic) { d in Task { await model.selectMic(d) } }
                    if let error = model.micError {
                        Text(error).font(.grotesk(11)).foregroundStyle(Studio.amber).fixedSize(horizontal: false, vertical: true)
                    }
                }
                .frame(maxWidth: .infinity, alignment: .leading)
                VStack(alignment: .leading, spacing: 6) {
                    MonoLabel("SIDETONE OUTPUT")
                    devicePicker(model.outputs, selected: model.selectedOutput) { d in Task { await model.selectOutput(d) } }
                }
                .frame(maxWidth: .infinity, alignment: .leading)
                VStack(alignment: .leading, spacing: 6) {
                    MonoLabel("PROCESSING")
                    HStack(spacing: 6) {
                        Button("Sidetone") { Task { await model.toggleSidetone() } }
                            .buttonStyle(CommsToggleStyle(on: model.sidetoneOn))
                            .help("Hear your own voice in the sidetone output")
                        Button("Tone") { Task { await model.toggleTone() } }
                            .buttonStyle(CommsToggleStyle(on: model.toneOn))
                            .help("Replace the microphone with a 700 Hz tone through the whole live chain")
                    }
                    HStack(spacing: 6) {
                        Button("−") { Task { await model.gainDown() } }.buttonStyle(GhostButtonStyle())
                        Text(model.gainText).font(.plexMono(12)).foregroundStyle(Studio.textPrimary).frame(minWidth: 56)
                        Button("+") { Task { await model.gainUp() } }.buttonStyle(GhostButtonStyle())
                    }
                }
                .fixedSize()
                VStack(alignment: .leading, spacing: 4) {
                    MonoLabel("STATION")
                    stat("Frames sent", model.framesSent)
                    stat("Channel sends", model.channelSends)
                    stat("Underruns", model.underruns)
                    stat("Send errors", model.sendErrors, warn: model.sendErrorsWarn)
                }
                .frame(minWidth: 200)
                .fixedSize()
            }
            .padding(EdgeInsets(top: 12, leading: 16, bottom: 12, trailing: 16))
        }
        .background(Studio.panel)
    }

    private func stat(_ label: String, _ value: String, warn: Bool = false) -> some View {
        HStack {
            Text(label).font(.grotesk(12)).foregroundStyle(Studio.secondary)
            Spacer(minLength: 12)
            Text(value).font(.plexMono(12)).foregroundStyle(warn ? Studio.amber : Studio.textPrimary)
        }
    }

    // The menu's items are the model's device list, which changes only when the
    // engine's list does; choosing sends the verb, and the label stays the
    // engine's device until it reports the switch.
    private func devicePicker(_ devices: [String], selected: String?, choose: @escaping (String) -> Void) -> some View {
        Menu {
            ForEach(devices, id: \.self) { d in Button(d) { choose(d) } }
        } label: {
            Text(selected?.isEmpty == false ? selected! : "System default")
                .font(.grotesk(12)).foregroundStyle(Studio.textPrimary).lineLimit(1).truncationMode(.tail)
                .frame(maxWidth: .infinity, alignment: .leading)
        }
        .menuStyle(.borderlessButton)
        .padding(.horizontal, 10).padding(.vertical, 6)
        .background(RoundedRectangle(cornerRadius: 6).fill(Studio.field))
        .overlay(RoundedRectangle(cornerRadius: 6).stroke(Studio.border, lineWidth: 1))
    }
}

// ── 4. strip ─────────────────────────────────────────────────────────────

struct CommsStrip: View {
    @ObservedObject var model: CommsModel
    var onLeave: () -> Void

    var body: some View {
        VStack(spacing: 0) {
            Rectangle().fill(Studio.border).frame(height: 1)
            HStack(spacing: 8) {
                Button("Latch") { model.latchMode.toggle() }.buttonStyle(CommsToggleStyle(on: model.latchMode))
                    .help("Presses toggle a channel on and off instead of talking while held")
                Button("Edit talent") { model.editMode.toggle() }.buttonStyle(CommsToggleStyle(on: model.editMode))
                Button("Settings") { model.settingsOpen.toggle() }.buttonStyle(CommsToggleStyle(on: model.settingsOpen))
                Button("Private comms") { Task { await model.togglePrivate() } }
                    .buttonStyle(CommsToggleStyle(on: model.privatePreference))
                    .help(model.privateHint)
                Button("Leave", action: onLeave).buttonStyle(CommsToggleStyle(on: false))
                    .help("Leave the meeting (video too)")
                Spacer(minLength: 12)
                MonoLabel("MIC")
                HStack(spacing: 2) {
                    ForEach(0..<CommsModel.meterSegments, id: \.self) { i in
                        let tone: CommsTone = i == CommsModel.meterSegments - 1 ? .hot : i >= 8 ? .armed : .ready
                        RoundedRectangle(cornerRadius: 1)
                            .fill(CommsColors.meter(tone).opacity(i < model.litSegments ? 1 : 0.18))
                            .frame(width: 6, height: 16)
                    }
                }
                .help(model.micLevelName)
                .accessibilityElement(children: .ignore)
                .accessibilityLabel(model.micLevelName)
            }
            .padding(EdgeInsets(top: 10, leading: 16, bottom: 10, trailing: 16))
        }
    }
}

// ── 5. ops strip ─────────────────────────────────────────────────────────

struct CommsOpsStrip: View {
    @ObservedObject var model: CommsModel

    var body: some View {
        VStack(spacing: 0) {
            Rectangle().fill(Studio.border).frame(height: 1)
            Button { model.opsOpen.toggle() } label: {
                VStack(alignment: .leading, spacing: 2) {
                    if let error = model.error {
                        Text(error).font(.grotesk(12, .semibold)).foregroundStyle(Studio.red)
                    }
                    if model.opsLineVisible {
                        Text(model.opsLine).font(.plexMono(11)).foregroundStyle(Studio.secondary)
                            .lineLimit(1).truncationMode(.tail)
                    }
                }
                .frame(maxWidth: .infinity, minHeight: 32, alignment: .leading)
                .padding(.horizontal, 16)
                .contentShape(Rectangle())
            }
            .buttonStyle(.plain)
            .help("Click for the event history")
            .accessibilityLabel("Comms event log")
            if model.opsOpen {
                ScrollView {
                    VStack(alignment: .leading, spacing: 2) {
                        ForEach(model.opsLines) { entry in
                            Text(entry.text).font(.plexMono(11)).foregroundStyle(Studio.secondary)
                                .frame(maxWidth: .infinity, alignment: .leading)
                        }
                    }
                    .padding(.horizontal, 16).padding(.bottom, 8)
                }
                .frame(maxHeight: 180)
            }
        }
    }
}
