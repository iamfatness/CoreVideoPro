// The Comms (Zoom talkback) tab's model: a one-for-one port of
// native-shell/CoreVideoPro.WinUI/ViewModels/Comms/CommsViewModel.cs. Every
// rule and every string is Windows'; the line numbers in comments point at
// that file. Pure (no AppKit, no bridge): it sends through an injected
// closure and reads an injected clock, so ShellTestsComms ports
// CommsViewModelTests one-for-one.
//
// Two shapes differ from Windows only because Swift is not WinUI:
//   - Sends are serialised: each one starts after the previous one finished,
//     so a press and its release can never reach the core out of order.
//     `drain()` awaits the tail (the C# tests' `LastSend`).
//   - Cells and people are value rows in arrays keyed by slot / participant
//     id. A row whose key survives keeps its position and its `held` flag
//     (ObservableCollectionSync semantics); SwiftUI identity is the key, so a
//     press is never rebuilt under the operator's finger.

import Foundation

enum CommsTone: Equatable { case idle, ready, armed, hot, off }

struct CommsCell: Identifiable, Equatable {
    let slot: Int
    var held = false
    var name = ""
    var stateText = ""
    var tone: CommsTone = .idle
    var off = false
    var keyHint: String?
    var id: Int { slot }
    var automationName: String { "Talk to \(name): \(stateText)" }
}

struct CommsChip: Equatable {
    let slot: Int
    var text: String
    var on: Bool
    var busy: Bool
    var tip: String
}

struct CommsPersonRow: Identifiable, Equatable {
    let participantId: UInt32
    var name = ""
    var talkback = false
    var chips: [CommsChip] = []
    var id: UInt32 { participantId }
}

struct CommsOpsEntry: Identifiable, Equatable {
    let key: String
    let text: String
    var id: String { key }
}

enum CommsPageState: Equatable {
    case notInMeeting, waiting, unavailable(text: String, hostHint: Bool), desk
}

@MainActor
final class CommsModel: ObservableObject {
    /// nil = ok, otherwise the core's refusal message verbatim.
    typealias Send = (_ verb: String, _ payload: JSONObject) async -> String?

    static let leaseMs = 1500
    static let renewEveryMs = 500
    static let privateResendMs: Int64 = 2000
    static let meterSegments = 12
    static let webClientWhy = "On the Zoom web client. Ask them to rejoin in the Zoom desktop app."

    @Published private(set) var state: CommsState?
    @Published private(set) var cells: [CommsCell] = []
    @Published private(set) var people: [CommsPersonRow] = []
    @Published private(set) var mics: [String] = []
    @Published private(set) var outputs: [String] = []
    @Published private(set) var opsLines: [CommsOpsEntry] = []
    @Published var latchMode = false
    @Published var editMode = false
    @Published var settingsOpen = false
    @Published var opsOpen = false
    @Published private(set) var error: String?
    @Published private(set) var inMeeting = false
    @Published private(set) var meetingLabel: String?
    @Published private(set) var privatePreference = false

    /// Bumped only when the cell set or a label changes (tests pin the gate).
    private(set) var cellStructureVersion = 0
    /// Bumped only when the talent rows change (tests pin the gate).
    private(set) var peopleStructureVersion = 0
    /// Bumped only when the device lists change (pickers are never rebuilt per report).
    private(set) var deviceListVersion = 0

    /// The operator changed the preference; the shell persists it.
    var onPrivatePreferenceChanged: ((Bool) -> Void)?

    private let send: Send
    private let now: () -> Int64
    private var privateSentAtMs = Int64.min / 2
    private var allHeld = false
    /// Held slots whose cell left the desk while held. Windows still releases
    /// through the orphaned cell instance on pointer-up; this keeps that
    /// release (only release(slot:) reads it, as only the instance would).
    private var orphanHeld: Set<Int> = []
    private var cellSig = ""
    private var peopleSig = ""
    private var micsSig = ""
    private var outsSig = ""
    private var opsSig = ""
    private var tail: Task<Void, Never>?

    init(send: @escaping Send = { _, _ in nil }, now: @escaping () -> Int64 = CommsModel.monotonicMs) {
        self.send = send
        self.now = now
    }

    nonisolated static func monotonicMs() -> Int64 {
        Int64(DispatchTime.now().uptimeNanoseconds / 1_000_000)
    }

    // ── screen states (:132-155) ─────────────────────────────────────────

    var pageState: CommsPageState {
        guard inMeeting else { return .notInMeeting }
        guard let s = state else { return .waiting }
        if !s.available {
            let text = s.reason ?? "Talkback is not available."
            return .unavailable(text: text, hostHint: (s.reason ?? "").range(of: "host", options: .caseInsensitive) != nil)
        }
        return .desk
    }

    var deskVisible: Bool { inMeeting && state?.available == true }
    var gridVisible: Bool { deskVisible && !editMode && !cells.isEmpty }
    var editVisible: Bool { deskVisible && editMode }
    var emptyDeskVisible: Bool { deskVisible && !editMode && cells.isEmpty }
    var emptyDeskText: String? {
        guard emptyDeskVisible else { return nil }
        return (state?.roster.count ?? 0) > 0 ? "In the meeting. Channels forming…" : "In the meeting. Waiting for participants…"
    }

    var railText: String {
        guard inMeeting else { return "Not in a meeting" }
        let room = (state?.room.map { " · Room \($0)" } ?? "") + (privateReported ? " · private comms" : "")
        return ((meetingLabel?.isEmpty == false) ? meetingLabel! : "In meeting") + room
    }

    // Lamps are machine truth even when talkback is refused (:157-163).
    private var reported: Bool { inMeeting && state != nil }
    var lampLink: Bool { reported && state!.lampLink }
    var lampMic: Bool { reported && state!.lampMeetingMic }
    var lampChannel: Bool { reported && state!.lampChannel }
    var lampTx: Bool { reported && state!.lampTx }

    // ── ALL CALL (:167-171) ──────────────────────────────────────────────

    private var allKeyed: Bool { deskVisible && !state!.channels.isEmpty && state!.channels.allSatisfy(\.keyed) }
    var allHot: Bool { allKeyed && state!.channels.contains { $0.listeners > 0 } }
    var allArmed: Bool { allKeyed && !state!.channels.contains { $0.listeners > 0 } }
    var allTone: CommsTone { allHot ? .hot : allArmed ? .armed : .ready }
    var allText: String {
        allHot ? (state!.channels.allSatisfy(\.latched) ? "ALL CALL · ON AIR · LATCHED" : "ALL CALL · ON AIR") : "ALL CALL"
    }

    // ── settings ─────────────────────────────────────────────────────────

    var selectedMic: String? { state?.mic.device }
    var selectedOutput: String? { state?.mic.sidetoneDevice }
    var sidetoneOn: Bool { state?.mic.sidetone == true }
    var toneOn: Bool { state?.mic.tone == true }
    var gainText: String {
        guard let g = state?.mic.gainDb else { return "" }
        return "\(g > 0 ? "+" : "")\(g) dB"
    }
    var framesSent: String { state.map { Self.grouped($0.framesSent) } ?? "" }
    var channelSends: String { state.map { Self.grouped($0.channelSends) } ?? "" }
    var underruns: String { state.map { Self.grouped($0.underruns) } ?? "" }
    var sendErrors: String { state.map { Self.grouped($0.sendErrors) } ?? "" }
    var sendErrorsWarn: Bool { (state?.sendErrors ?? 0) > 0 }
    /// The engine's mic failure verbatim (macOS: the permission text, N1).
    var micError: String? { state?.mic.error }
    var micDb: Double? { deskVisible ? state!.mic.peakDbfs : nil }
    var micLevelName: String {
        if let db = micDb, db.isFinite, abs(db) < 1e6 { return "Microphone level \(Int(db.rounded())) dBFS" }
        return "Microphone level not measured"
    }
    // 12 segments over -48..0 dBFS; unmeasured = dark (:497-501). C#
    // Math.Round is round-half-to-even, and the engine's integer dBFS hits
    // exact halves (-30 -> 4.5, -6 -> 10.5).
    var litSegments: Int {
        guard let db = micDb, db.isFinite else { return 0 }
        return Int((min(max((db + 48) / 48, 0), 1) * Double(Self.meterSegments)).rounded(.toNearestOrEven))
    }

    private static func grouped(_ n: Int64) -> String {
        let f = NumberFormatter()
        f.numberStyle = .decimal
        f.locale = Locale(identifier: "en_US")
        return f.string(from: NSNumber(value: n)) ?? "\(n)"
    }

    func selectMic(_ device: String) async {
        guard device != state?.mic.device else { return }
        await sendAndWait("comms-mic", ["device": device])
    }

    func selectOutput(_ device: String) async {
        guard device != state?.mic.sidetoneDevice else { return }
        await sendAndWait("comms-sidetone-out", ["device": device])
    }

    func toggleSidetone() async { await sendAndWait("comms-sidetone", ["on": !sidetoneOn]) }
    func toggleTone() async { await sendAndWait("comms-tone", ["on": !toneOn]) }
    func gainDown() async { await sendGain(-1) }
    func gainUp() async { await sendGain(+1) }

    private func sendGain(_ delta: Int) async {
        guard let current = state?.mic.gainDb else { return }
        let db = min(max(current + delta, -20), 20)
        guard db != current else { return }
        await sendAndWait("comms-gain", ["db": db])
    }

    // ── private comms (:226-281) ─────────────────────────────────────────

    var privateReported: Bool { state?.isPrivate == true }

    var privateHint: String {
        privatePreference
            ? "Private comms ON: the room never hears you; you speak only through talkback. Turn off to use your own meeting mic (you stay muted until you unmute)."
            : "Private comms OFF: talkback uses your own meeting mic, which must be open (the room hears it too). Turn on so only talent hears you."
    }

    /// Startup: the saved preference, without sending anything yet.
    func restorePrivatePreference(_ on: Bool) {
        privatePreference = on
    }

    func togglePrivate() async {
        privatePreference.toggle()
        onPrivatePreferenceChanged?(privatePreference)
        if inMeeting { await sendPrivate().value }
    }

    @discardableResult
    private func sendPrivate() -> Task<Void, Never> {
        privateSentAtMs = now()
        return enqueue("comms-private", ["on": privatePreference])
    }

    // A new engine (rejoin, core restart) starts OFF: re-assert the preference
    // whenever a report disagrees, at most once per privateResendMs.
    private func reconcilePrivate() {
        guard let s = state, inMeeting, s.isPrivate != privatePreference else { return }
        guard now() - privateSentAtMs >= Self.privateResendMs else { return }
        sendPrivate()
    }

    // ── ops strip (:285-307, :478-494) ───────────────────────────────────

    var opsLine: String { state?.log.last ?? "No comms events yet." }
    var opsLineVisible: Bool { !opsOpen }

    /// "Meeting 123 4567 8901" from a join URL or bare id; nil when there is no id.
    nonisolated static func meetingLabel(for joinText: String) -> String? {
        let trimmed = joinText.trimmingCharacters(in: .whitespacesAndNewlines)
        guard !trimmed.isEmpty,
              let regex = try? NSRegularExpression(pattern: #"(?:/j/|/w/|^)\s*(\d[\d\s-]{7,})"#),
              let match = regex.firstMatch(in: joinText, range: NSRange(joinText.startIndex..., in: joinText)),
              let range = Range(match.range(at: 1), in: joinText)
        else { return nil }
        let digits = joinText[range].filter(\.isNumber).map(String.init).joined()
        guard digits.count >= 9 else { return nil }
        let d = Array(digits)
        let grouped: String
        switch d.count {
        case 10: grouped = "\(String(d[0..<3])) \(String(d[3..<6])) \(String(d[6...]))"
        case 11: grouped = "\(String(d[0..<3])) \(String(d[3..<7])) \(String(d[7...]))"
        default: grouped = digits
        }
        return "Meeting \(grouped)"
    }

    // ── apply (:311-344) ─────────────────────────────────────────────────

    /// The shell's meeting phase. Leaving forgets every local key and sends
    /// nothing (Windows code, :318): the engine lease and the core's
    /// comms:null do the real release.
    func applyMeeting(_ inMeeting: Bool, label: String?) {
        guard self.inMeeting != inMeeting || meetingLabel != label else { return }
        let wasIn = self.inMeeting
        self.inMeeting = inMeeting
        meetingLabel = label
        if wasIn && !inMeeting { releaseAllLocal() }
        refresh()
    }

    /// One core comms fact. `.some(nil)` = the core dropped the report; nil =
    /// not a fact (ignored).
    func apply(_ fact: CommsState??) {
        guard let fact else { return }
        state = fact
        if fact == nil { releaseAllLocal() }
        refresh()
        reconcilePrivate()
    }

    /// A new core generation (restart): every local hold and the last report
    /// are forgotten. Private is re-sent only by the report-disagrees rule.
    func coreGenerationChanged() {
        releaseAllLocal()
        state = nil
        refresh()
    }

    private func refresh() {
        applyCells()
        applyPeople()
        applyDevices()
        applyOps()
    }

    // A channel is a cell when someone is on it, it is named, or it is
    // keyed/latched (:375-415). The set changes only when that set (or a
    // label) changes, and then in place.
    private func applyCells() {
        let chans = deskVisible ? state!.channels : []
        let rows = chans.filter { $0.listeners > 0 || $0.label != nil || $0.keyed || $0.latched }
        let sig = rows.map { "\($0.slot):\($0.label ?? "")" }.joined(separator: "|")
        var next = cells
        if sig != cellSig {
            cellSig = sig
            cellStructureVersion += 1
            let held = Dictionary(cells.map { ($0.slot, $0.held) }, uniquingKeysWith: { a, _ in a })
            let surviving = Set(rows.map(\.slot))
            for c in cells where c.held && !surviving.contains(c.slot) { orphanHeld.insert(c.slot) }
            next = rows.map { CommsCell(slot: $0.slot, held: held[$0.slot] ?? false) }
        }
        var live = 0
        for i in next.indices {
            guard let ch = chans.first(where: { $0.slot == next[i].slot }) else { continue }
            next[i].name = ch.label ?? "CH \(ch.slot + 1)"
            let unreachable = ch.reachOk == 0 && ch.reachDark > 0
            if ch.room != nil || unreachable {
                next[i].off = true
                next[i].tone = .off
                next[i].stateText = ch.room.map { "in \($0)" } ?? "all in other rooms"
                next[i].keyHint = nil
                continue
            }
            next[i].off = false
            let hearing = ch.listeners > 0
            next[i].tone = ch.keyed && hearing ? .hot : ch.keyed ? .armed : hearing ? .ready : .idle
            let partial = ch.reachDark > 0 ? " · \(ch.reachDark) elsewhere" : ""
            let text: String
            if ch.keyed && hearing { text = ch.latched ? "on air · latched" : "on air" }
            else if ch.keyed { text = "nobody in channel" }
            else if !ch.ready { text = "forming…" }
            else if ch.label == nil { text = "\(ch.listeners) listening" }
            else { text = hearing ? "ready" : "invite in flight" }
            next[i].stateText = text + partial
            live += 1
            next[i].keyHint = live <= 9 ? String(live) : nil
        }
        if next != cells { cells = next }
    }

    private func applyPeople() {
        let c = deskVisible ? state : nil
        let roster = c?.roster ?? []
        let chans = c?.channels ?? []
        let sig = roster.map { "\($0.participantId):\($0.name):\($0.talkback):\($0.room ?? ""):\($0.channels.map(String.init).joined(separator: ","))" }
            .joined(separator: "~") + "#" + chans.map { $0.label ?? "" }.joined(separator: "~")
        guard sig != peopleSig else { return }
        peopleSig = sig
        peopleStructureVersion += 1
        people = roster.map { p in
            CommsPersonRow(participantId: p.participantId,
                           name: p.name + (p.room.map { "  · in \($0)" } ?? ""),
                           talkback: p.talkback,
                           chips: p.talkback ? buildChips(p, roster: roster, chans: chans) : [])
        }
    }

    private func buildChips(_ p: CommsPerson, roster: [CommsPerson], chans: [CommsChannel]) -> [CommsChip] {
        chans.map { ch in
            let on = p.channels.contains(ch.slot)
            let others = roster.filter { $0.participantId != p.participantId && $0.channels.contains(ch.slot) }.map(\.name)
            let label = ch.label ?? "CH \(ch.slot + 1)"
            let tip = (on ? "Take off \(label)" : "Put on \(label)")
                + (!others.isEmpty ? ", with \(others.joined(separator: ", "))" : on ? "" : " (empty)")
            return CommsChip(slot: ch.slot, text: String(ch.slot + 1), on: on, busy: !on && !others.isEmpty, tip: tip)
        }
    }

    // Device lists change rarely; never rebuilt per report (:462-474).
    private func applyDevices() {
        let nextMics = state?.mic.devices ?? []
        let nextOuts = state?.mic.sidetoneDevices ?? []
        let ms = nextMics.joined(separator: "\u{1f}"), os = nextOuts.joined(separator: "\u{1f}")
        if ms != micsSig { micsSig = ms; mics = nextMics; deviceListVersion += 1 }
        if os != outsSig { outsSig = os; outputs = nextOuts; deviceListVersion += 1 }
    }

    private func applyOps() {
        let log = state?.log ?? []
        let sig = log.joined(separator: "\u{1f}")
        guard sig != opsSig else { return }
        opsSig = sig
        var seen: [String: Int] = [:]
        var entries: [CommsOpsEntry] = []
        for line in log.reversed() {
            let n = seen[line].map { $0 + 1 } ?? 0
            seen[line] = n
            entries.append(CommsOpsEntry(key: "\(n)\u{1f}\(line)", text: line))
        }
        opsLines = entries
    }

    // ── keys (:505-582) ──────────────────────────────────────────────────

    func press(slot: Int) async {
        guard deskVisible, let i = cells.firstIndex(where: { $0.slot == slot }), !cells[i].off else { return }
        let ch = state!.channels.first { $0.slot == slot }
        if latchMode {
            await sendAndWait("comms-latch", ["slot": slot, "on": !(ch?.latched ?? false)])
            return
        }
        guard !cells[i].held else { return }
        cells[i].held = true
        await sendAndWait("comms-key", ["slot": slot, "on": true, "leaseMs": Self.leaseMs])
    }

    func release(slot: Int) async {
        if orphanHeld.remove(slot) != nil {
            // The press went to a cell that has since left the desk.
        } else if let i = cells.firstIndex(where: { $0.slot == slot }), cells[i].held {
            cells[i].held = false
        } else {
            return
        }
        await sendAndWait("comms-key", ["slot": slot, "on": false])
    }

    func allPress() async {
        guard deskVisible else { return }
        if latchMode {
            let allOn = !state!.channels.isEmpty && state!.channels.allSatisfy(\.latched)
            await sendAndWait("comms-latch", ["all": true, "on": !allOn])
            return
        }
        guard !allHeld else { return }
        allHeld = true
        await sendAndWait("comms-key", ["all": true, "on": true, "leaseMs": Self.leaseMs])
    }

    func allRelease() async {
        guard allHeld else { return }
        allHeld = false
        await sendAndWait("comms-key", ["all": true, "on": false])
    }

    var allCallHeld: Bool { allHeld }

    /// True while any key (or ALL CALL) is held by this operator.
    var anyHeld: Bool { allHeld || !orphanHeld.isEmpty || cells.contains { $0.held } }

    /// Re-sends the lease on every held key (the shell's 500 ms tick).
    func renewLeases() async {
        var last: Task<Void, Never>?
        for c in cells where c.held {
            last = enqueue("comms-key", ["slot": c.slot, "on": true, "leaseMs": Self.leaseMs])
        }
        if allHeld { last = enqueue("comms-key", ["all": true, "on": true, "leaseMs": Self.leaseMs]) }
        await last?.value
    }

    /// The shell is going away: every held key up in one line, whatever this
    /// model believes is held (latches stay; they are deliberate).
    func sendReleaseAll() async {
        releaseAllLocal()
        await sendAndWait("comms-release-all", [:])
    }

    /// Digits 1-9 key the first nine reachable cells (panel-scoped).
    func cell(forDigit digit: Int) -> CommsCell? {
        let live = cells.filter { !$0.off }
        return digit >= 1 && digit <= live.count ? live[digit - 1] : nil
    }

    /// Focus left the page or the window, or the operator navigated away:
    /// every held key goes up.
    func releaseAll() async {
        for slot in orphanHeld.sorted() { await release(slot: slot) }
        for c in cells where c.held { await release(slot: c.slot) }
        await allRelease()
    }

    func assign(slot: Int, participantId: UInt32, on: Bool) async {
        await sendAndWait("comms-assign", ["slot": slot, "participantId": participantId, "on": on])
    }

    private func releaseAllLocal() {
        orphanHeld.removeAll()
        for i in cells.indices where cells[i].held { cells[i].held = false }
        allHeld = false
    }

    // ── sends (:586-613) ─────────────────────────────────────────────────

    /// Awaits every send queued so far (the C# tests' `LastSend`).
    func drain() async { await tail?.value }

    private func sendAndWait(_ verb: String, _ payload: JSONObject) async {
        await enqueue(verb, payload).value
    }

    @discardableResult
    private func enqueue(_ verb: String, _ payload: JSONObject) -> Task<Void, Never> {
        let previous = tail
        let send = self.send
        let task = Task { @MainActor [weak self] in
            await previous?.value
            let refusal = await send(verb, payload)
            self?.error = refusal
        }
        tail = task
        return task
    }
}
