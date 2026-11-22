// SourceFactsStore — the observed row facts, one ObservableObject per slot
// (sources plan Task 5). A @Published write on AppModel re-evaluates EVERY
// view that observes AppModel, and `zoom-source-format` facts arrive up to
// five times a second per guest; so the facts live in per-slot boxes, and a
// box is written only when ITS facts changed (spec correction 7).

import Combine
import Foundation

@MainActor
final class SlotFactsBox: ObservableObject {
    @Published fileprivate(set) var facts = SourceRowFacts()
}

@MainActor
final class SourceFactsStore {
    private(set) var projection = SourceRowFactsProjection()
    private var boxes: [Int: SlotFactsBox] = [:]

    init() {
        for slotId in 1...ShowInputAssignmentLaw.slotCount { boxes[slotId] = SlotFactsBox() }
    }

    /// Stable instance per slot id 1...10.
    func box(_ slotId: Int) -> SlotFactsBox {
        if let box = boxes[slotId] { return box }
        let box = SlotFactsBox()
        boxes[slotId] = box
        return box
    }

    /// Applies `change`, then writes each box ONLY if its facts changed.
    func mutate(_ change: (inout SourceRowFactsProjection) -> Void) {
        change(&projection)
        for (slotId, box) in boxes {
            let next = projection.facts(slotId: slotId)
            if box.facts != next { box.facts = next }
        }
    }
}

extension AppModel {
    /// Capture device rows → the projection's observed signal facts.
    static func captureSignals(_ devices: [CaptureDeviceRow]) -> [String: CaptureSignal] {
        Dictionary(devices.map { ($0.id, CaptureSignal(connectionState: $0.connectionState,
                                                       signalPresent: $0.signalPresent)) },
                   uniquingKeysWith: { first, _ in first })
    }

    /// Roster participants → the projection's roster fact.
    static func zoomRosterFact(epoch: String, meetingState: String,
                               roster: [RosterParticipant]) -> ZoomRosterFact {
        ZoomRosterFact(epoch: epoch, meetingState: meetingState,
                       participants: roster.map {
                           ZoomParticipantFact(id: $0.id, videoOn: $0.hasVideo, talking: $0.talking,
                                               sourceGeneration: $0.sourceGeneration)
                       })
    }

    /// The Preview scene's source keys, from the routes it would send.
    func previewSourceKeys() -> Set<String> {
        guard !previewSceneId.isEmpty else { return [] }
        var keys: Set<String> = []
        for route in buildRoutes(for: previewSceneId) {
            if let pid = route["participantId"] as? String, !pid.isEmpty { keys.insert("zoom:" + pid) }
            if let cam = route["captureDeviceId"] as? String, !cam.isEmpty { keys.insert("capture:" + cam) }
        }
        return keys
    }
}

extension AppModel {
    /// The snapshot's observed facts: `sources[]` rows, rendered Program, the
    /// Preview scene, and ISO writer lifecycle.
    func applySourceFacts(_ snapshot: JSONObject) {
        let sources = snapshot["sources"] as? [JSONObject]
        let program = (snapshot["programFrame"] as? JSONObject).map { frame in
            Set((frame["videoSources"] as? [JSONObject] ?? []).compactMap { $0["sourceId"] as? String })
        }
        let recording = snapshot["recording"] as? JSONObject
        guard sources != nil || program != nil || recording != nil else { return }
        let now = sourceFactsNowMs
        let captures = Self.captureSignals(captureDevices)
        let preview = program != nil ? previewSourceKeys() : []
        sourceFacts.mutate { projection in
            if let sources {
                projection.applyBus(BusSourceRow.parse(sources), captures: captures, atMs: now)
            }
            if let program { projection.applyTally(program: program, preview: preview) }
            if let recording { projection.applyWriters(IsoWriterChip.observe(recording)) }
        }
    }
}

import SwiftUI

/// The row's observed signal chip, format and FPS. Observes ONE slot's box, so
/// a fact on another row does not re-evaluate this one.
struct SlotSignalFacts: View {
    @ObservedObject var box: SlotFactsBox

    var body: some View {
        HStack(spacing: 6) {
            Text(box.facts.chip)
                .font(.plexMono(9, .semibold))
                .foregroundStyle(Self.signalColor(box.facts.chip))
            Text(box.facts.format).font(.plexMono(10)).foregroundStyle(Studio.secondary)
            Text(box.facts.fps).font(.plexMono(10)).foregroundStyle(Studio.secondary)
        }
        .lineLimit(1)
        .fixedSize()
        .accessibilityElement(children: .combine)
    }

    /// Windows MultiviewInputRow.cs:58-71, mapped to Studio tokens.
    static func signalColor(_ chip: String) -> Color {
        switch chip {
        case "LIVE": return Studio.accent
        case "PGM": return Studio.red
        case "TALKING", "STALLED": return Studio.amber
        default: return Studio.secondary
        }
    }
}

/// The ISO writer chip (observed writer state, separate from the ISO selection).
struct SlotWriterChip: View {
    @ObservedObject var box: SlotFactsBox

    var body: some View {
        Text(box.facts.writer)
            .font(.plexMono(9, .semibold))
            .foregroundStyle(box.facts.writer == "REC" ? Studio.red
                             : box.facts.writer == "ARMED" || box.facts.writer == "ERROR" ? Studio.amber
                             : Studio.secondary)
            .fixedSize()
    }
}
