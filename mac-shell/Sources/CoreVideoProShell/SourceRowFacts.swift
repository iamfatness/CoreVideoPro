// SourceRowFacts — what each Sources row shows about its signal, decided only
// from facts the core OBSERVED (sources plan Task 4; the Swift twin of Windows
// ViewModels/ShowInputs/MultiviewInputRowsProjection.cs PatchSlot :329-487).
//
// The rule it exists for (MultiviewInputRow.cs:35-38): a request ceiling is
// never shown as a measurement. So capture Format comes from the bus row's
// latest frame size and capture FPS is MEASURED from framesIngested; the
// capture device row's frameRate/resolution are never read (they are the
// struct default 30 and 1920×1080 until the first frame — spec correction 5).
// Zoom Format/FPS come from `zoom-source-format` events, fenced by roster
// epoch, source generation and frame clock (ApplySourceFormatFact :106-125).

import Foundation

struct SourceSlotRef: Equatable {
    let slotId: Int
    let kind: String
    let sourceId: String
    let inShow: Bool

    init(_ slot: ShowInputSlot) {
        self.init(slotId: slot.id, kind: slot.kind, sourceId: slot.sourceId, inShow: slot.inShow)
    }

    init(slotId: Int, kind: String, sourceId: String, inShow: Bool) {
        self.slotId = slotId; self.kind = kind; self.sourceId = sourceId; self.inShow = inShow
    }

    /// "zoom:<pid>" / "capture:<id>", same as ShowInputSlot.sourceKey.
    var sourceKey: String? {
        guard !sourceId.isEmpty else { return nil }
        switch kind {
        case "zoom": return "zoom:" + sourceId
        case "capture": return "capture:" + sourceId
        default: return nil
        }
    }
}

struct ZoomParticipantFact: Equatable {
    let id: String
    let videoOn: Bool
    let talking: Bool
    let sourceGeneration: Int64
}

struct ZoomRosterFact: Equatable {
    let epoch: String
    let meetingState: String
    let participants: [ZoomParticipantFact]
}

struct ZoomFormatFact: Equatable {
    let participantId: String
    let rosterEpoch: String
    let sourceGeneration: Int64
    let width: Int
    let height: Int
    let fps: Int
    let frameId: Int
    let frameAtMs: Double

    /// Parses a `zoom-source-format` event: fields under event["format"]
    /// (ZoomEngineRuntime.cpp:1953-1964). `fps` is the core's measured delivered
    /// rate and may arrive fractional; it is rounded.
    static func parse(event: JSONObject) -> ZoomFormatFact? {
        guard let format = event["format"] as? JSONObject,
              let pidValue = format["participantId"],
              let epoch = format["rosterEpoch"] as? String,
              let width = (format["width"] as? NSNumber)?.intValue,
              let height = (format["height"] as? NSNumber)?.intValue,
              let frameAt = (format["frameAtMs"] as? NSNumber)?.doubleValue else { return nil }
        return ZoomFormatFact(
            participantId: "\(pidValue)", rosterEpoch: epoch,
            sourceGeneration: (format["sourceGeneration"] as? NSNumber)?.int64Value ?? 0,
            width: width, height: height,
            fps: Int(((format["fps"] as? NSNumber)?.doubleValue ?? 0).rounded()),
            frameId: (format["frameId"] as? NSNumber)?.intValue ?? 0,
            frameAtMs: frameAt)
    }
}

struct BusSourceRow: Equatable {
    let sourceId: String
    let kind: String
    let width: Int
    let height: Int
    let framesIngested: Double
    let sinceLastNewFrameMs: Double?   // JSON null → nil
    let liveness: String?
    let onAir: String?
    let lossReason: String?
    let failInMs: Double?
    let lastSampleAgeMs: Double?
    let dropoutPolicy: String
    let dropoutPolicyApplies: Bool?

    /// Parses snapshot `sources[]` (SourceVideoIngress.cpp:91-131), keyed by sourceId.
    /// Zoom rows are keyed by the RAW participant id, capture rows by "capture:<id>".
    static func parse(_ sources: [JSONObject]) -> [String: BusSourceRow] {
        var rows: [String: BusSourceRow] = [:]
        for entry in sources {
            guard let id = entry["sourceId"] as? String, !id.isEmpty else { continue }
            func number(_ key: String) -> Double? { (entry[key] as? NSNumber)?.doubleValue }
            rows[id] = BusSourceRow(
                sourceId: id, kind: entry["kind"] as? String ?? "",
                width: (entry["width"] as? NSNumber)?.intValue ?? 0,
                height: (entry["height"] as? NSNumber)?.intValue ?? 0,
                framesIngested: number("framesIngested") ?? 0,
                sinceLastNewFrameMs: number("sinceLastNewFrameMs"),
                liveness: entry["liveness"] as? String, onAir: entry["onAir"] as? String,
                lossReason: entry["lossReason"] as? String, failInMs: number("failInMs"),
                lastSampleAgeMs: number("lastSampleAgeMs"),
                dropoutPolicy: entry["dropoutPolicy"] as? String ?? "hold",
                dropoutPolicyApplies: entry["dropoutPolicyApplies"] as? Bool)
        }
        return rows
    }
}

struct CaptureSignal: Equatable {
    let connectionState: String
    let signalPresent: Bool
}

struct SourceRowFacts: Equatable {
    var signal = "IDLE"        // IDLE | NO INCOMING | VIDEO OFF | CONNECTING | LIVE | TALKING | STALLED
    var format = "—"           // "W×H" (U+00D7) or "—"
    var fps = "—"              // integer string or "—"
    var formatStale = false
    var frameAgeMs: Double? = nil
    var tally = "none"         // pgm | pvw | talking | hold | none
    var writer = "OFF"         // OFF | ARMED | REC | ERROR | —
    /// MultiviewInputRow.cs:56: PGM only for a LIVE row on Program.
    var chip: String { signal == "LIVE" && tally == "pgm" ? "PGM" : signal }
}

struct SourceRowFactsProjection {
    static let staleAfterMs = 1500.0            // MultiviewInputRowsProjection.cs:484 (> 1500)
    static let fpsWindowMs = 900.0              // capture FPS needs samples spanning ≥ this

    /// Per-row Zoom frame state (the Windows row's LastFrameAtMs/FormatLabel).
    private struct ZoomRowFrame {
        var width = 0, height = 0, fps = 0
        var lastFrameAtMs = -1.0
        var appliedGeneration: Int64 = 0
    }

    private let slotIds: [Int]
    private var refs: [Int: SourceSlotRef] = [:]
    private var zoomFrames: [Int: ZoomRowFrame] = [:]
    private var roster: ZoomRosterFact?
    private var engineIsStopped = false
    private var bus: [String: BusSourceRow] = [:]
    private var captures: [String: CaptureSignal] = [:]
    private var captureSamples: [String: [(atMs: Double, frames: Double)]] = [:]
    private var program: Set<String> = []
    private var preview: Set<String> = []
    private var writers: [String: String] = [:]

    init(slotIds: [Int] = Array(1...10)) {
        self.slotIds = slotIds
    }

    /// A changed sourceKey resets that row (MultiviewInputRowsProjection.cs:334-344).
    mutating func setSlots(_ newRefs: [SourceSlotRef]) {
        for ref in newRefs where slotIds.contains(ref.slotId) {
            if refs[ref.slotId]?.sourceKey != ref.sourceKey {
                zoomFrames[ref.slotId] = nil
            }
            refs[ref.slotId] = ref
        }
        let liveCaptureKeys = Set(refs.values.compactMap { $0.kind == "capture" ? $0.sourceKey : nil })
        captureSamples = captureSamples.filter { liveCaptureKeys.contains($0.key) }
    }

    /// A new epoch clears every Zoom format and frame clock (:86-99).
    mutating func applyRoster(_ next: ZoomRosterFact) {
        if roster?.epoch != next.epoch { zoomFrames.removeAll() }
        engineIsStopped = false
        roster = next
        // A reused id with a different source generation is a different person (:409-443).
        for (slotId, ref) in refs where ref.kind == "zoom" {
            guard let participant = next.participants.first(where: { $0.id == ref.sourceId }),
                  participant.sourceGeneration > 0 else { continue }
            var frame = zoomFrames[slotId] ?? ZoomRowFrame()
            if frame.appliedGeneration != 0, frame.appliedGeneration != participant.sourceGeneration {
                frame = ZoomRowFrame()
            }
            frame.appliedGeneration = participant.sourceGeneration
            zoomFrames[slotId] = frame
        }
    }

    /// The format fence (ApplySourceFormatFact :106-125).
    mutating func applyFormat(_ fact: ZoomFormatFact) {
        guard !engineIsStopped, let roster, Self.inMeeting(roster.meetingState),
              roster.epoch == fact.rosterEpoch,
              let participant = roster.participants.first(where: { $0.id == fact.participantId }),
              participant.videoOn else { return }
        if participant.sourceGeneration > 0 && participant.sourceGeneration != fact.sourceGeneration { return }
        for (slotId, ref) in refs where ref.kind == "zoom" && ref.sourceId == fact.participantId {
            var frame = zoomFrames[slotId] ?? ZoomRowFrame()
            guard fact.frameAtMs > frame.lastFrameAtMs else { continue }
            frame.lastFrameAtMs = fact.frameAtMs
            frame.width = fact.width; frame.height = fact.height; frame.fps = fact.fps
            if participant.sourceGeneration > 0 { frame.appliedGeneration = participant.sourceGeneration }
            zoomFrames[slotId] = frame
        }
    }

    mutating func applyBus(_ rows: [String: BusSourceRow], captures newCaptures: [String: CaptureSignal],
                           atMs: Double) {
        bus = rows
        captures = newCaptures
        for ref in refs.values where ref.kind == "capture" {
            guard let key = ref.sourceKey, let row = rows[key] else { continue }
            var samples = captureSamples[key] ?? []
            if let last = samples.last, row.framesIngested < last.frames || atMs < last.atMs {
                samples.removeAll()   // a counter or clock that went backwards is a new run
            }
            if samples.last?.atMs != atMs { samples.append((atMs, row.framesIngested)) }
            captureSamples[key] = Array(samples.suffix(5))
        }
    }

    /// sourceKeys on rendered Program and on the Preview scene.
    mutating func applyTally(program: Set<String>, preview: Set<String>) {
        self.program = program
        self.preview = preview
    }

    /// sourceKey → recording|armed|error|unknown (IsoWriterChip.observe).
    mutating func applyWriters(_ chips: [String: String]) {
        writers = chips
    }

    /// Operator Capture off, or a core respawn: nothing kept.
    mutating func engineStopped() {
        engineIsStopped = true
        roster = nil
        zoomFrames.removeAll()
        bus.removeAll()
        captureSamples.removeAll()
        program = []; preview = []
    }

    func facts(slotId: Int) -> SourceRowFacts {
        var facts = SourceRowFacts()
        guard let ref = refs[slotId], let key = ref.sourceKey else { return facts }
        facts.writer = Self.writerLabel(writers[key])
        guard !engineIsStopped, ref.inShow else { return facts }
        var talking = false
        if ref.kind == "capture" {
            patchCapture(&facts, key: key, deviceId: ref.sourceId)
        } else {
            talking = patchZoom(&facts, slotId: slotId, pid: ref.sourceId)
        }
        facts.tally = Self.tally(signal: facts.signal, stale: facts.formatStale, key: key,
                                 program: program, preview: preview, talking: talking)
        return facts
    }

    // ── per-kind patches ────────────────────────────────────────────────

    private func patchCapture(_ facts: inout SourceRowFacts, key: String, deviceId: String) {
        guard let device = captures[deviceId] else { facts.signal = "NO INCOMING"; return }
        guard device.connectionState == "connected", device.signalPresent else {
            facts.signal = "CONNECTING"; return
        }
        facts.signal = "LIVE"
        guard let row = bus[key] else { return }
        if row.width > 0 && row.height > 0 { facts.format = "\(row.width)×\(row.height)" }
        if let age = row.sinceLastNewFrameMs, age > Self.staleAfterMs { return }
        if let samples = captureSamples[key], let first = samples.first, let last = samples.last,
           last.atMs - first.atMs >= Self.fpsWindowMs {
            let rate = (last.frames - first.frames) / (last.atMs - first.atMs) * 1000
            facts.fps = String(Int(rate.rounded()))
        }
    }

    /// Returns whether the participant is talking (for the tally).
    private func patchZoom(_ facts: inout SourceRowFacts, slotId: Int, pid: String) -> Bool {
        guard let roster else { return false }                        // IDLE (:386-392)
        guard Self.inMeeting(roster.meetingState) else { facts.signal = "NO INCOMING"; return false }
        guard let participant = roster.participants.first(where: { $0.id == pid }) else {
            facts.signal = "NO INCOMING"; return false
        }
        guard participant.videoOn else { facts.signal = "VIDEO OFF"; return participant.talking }
        guard let frame = zoomFrames[slotId], frame.lastFrameAtMs >= 0 else {
            facts.signal = "CONNECTING"; return participant.talking
        }
        facts.format = "\(frame.width)×\(frame.height)"
        if let age = bus[pid]?.sinceLastNewFrameMs, age > Self.staleAfterMs {
            facts.signal = "STALLED"
            facts.formatStale = true
            facts.frameAgeMs = age
            return participant.talking
        }
        facts.fps = frame.fps > 0 ? String(frame.fps) : "—"
        facts.signal = participant.talking ? "TALKING" : "LIVE"
        return participant.talking
    }

    // ── shared rules ────────────────────────────────────────────────────

    static func inMeeting(_ state: String) -> Bool { state == "in_meeting" || state == "in-meeting" }

    /// MultiviewTileCropPolicy.cs:31-37 + MultiviewInputRowsProjection.cs:298.
    static func tally(signal: String, stale: Bool, key: String, program: Set<String>,
                      preview: Set<String>, talking: Bool) -> String {
        if signal == "VIDEO OFF" || signal == "IDLE" { return "none" }
        if stale { return "hold" }
        if program.contains(key) { return "pgm" }
        if preview.contains(key) { return "pvw" }
        if talking && key.hasPrefix("zoom:") { return "talking" }
        return "none"
    }

    /// MultiviewInputRow.cs:57.
    static func writerLabel(_ chip: String?) -> String {
        switch chip {
        case "recording": return "REC"
        case "armed": return "ARMED"
        case "error": return "ERROR"
        case "unknown": return "—"
        default: return "OFF"
        }
    }
}

enum IsoWriterChip {
    /// Port of CoreVideoPro.MediaCore/Services/IsoOutputLifecyclePolicy.cs `Observe`, plus the
    /// Mac rule that an "unreported" ISO stream is "unknown" (shown "—", never OFF; spec §4.9).
    /// Keyed by the stream's sourceId, else its participantId (the unreported branch,
    /// CoreStateCapture.cpp:2054-2066). A key without ':' is treated as "zoom:<key>".
    static func observe(_ recording: JSONObject?) -> [String: String] {
        guard let recording else { return [:] }
        let lifecycle = recording["lifecycle"] as? JSONObject
        let state = lifecycle?["state"] as? String
        let health = lifecycle?["health"] as? String
        var result: [String: String] = [:]
        for stream in recording["streams"] as? [JSONObject] ?? [] {
            guard stream["kind"] as? String == "iso",
                  let raw = (stream["sourceId"] as? String) ?? (stream["participantId"] as? String),
                  !raw.isEmpty else { continue }
            let key = raw.contains(":") ? raw : "zoom:" + raw
            let status = stream["status"] as? String ?? ""
            if status == "unreported" { result[key] = "unknown"; continue }
            let error = state == "failed" || state == "interrupted" || health == "failed"
                || ["warning", "failed", "error"].contains(status)
            let armed = ["requested", "preparing", "starting"].contains(state ?? "")
            let live: Bool
            if lifecycle != nil {
                live = (state == "producing" || state == "live") && (health == "healthy" || health == "degraded")
            } else {
                let recStatus = recording["status"] as? String ?? ""
                live = recording["active"] as? Bool == true && (recStatus == "recording" || recStatus == "warning")
            }
            let chip = error ? "error" : armed ? "armed" : live ? "recording" : "off"
            if chip != "off" { result[key] = chip }
        }
        return result
    }
}
