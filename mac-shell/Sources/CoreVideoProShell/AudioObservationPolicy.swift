// AudioObservationPolicy — port of native-shell AudioObservationPolicy.cs (#870).
// The core publishes `audioObservation` in every snapshot (ageMs = how old the
// audio worker's last measurement was when the snapshot was assembled). The
// shell adds the time since it RECEIVED that snapshot, so a stalled core or
// worker cannot leave frozen meters looking live. Windows holds the last meter
// values and says so in words; the Mac does the same.
//   age <= 250 ms  fresh | <= 1000 ms aging | beyond stale

import Foundation

struct AudioObservation: Equatable {
    var sequence: Int
    var status: String
    var ageMs: Double

    /// nil when the snapshot has no envelope (legacy core).
    init?(_ snapshot: [String: Any]) {
        guard let o = snapshot["audioObservation"] as? [String: Any] else { return nil }
        self.init(sequence: (o["sequence"] as? NSNumber)?.intValue ?? 0,
                  status: o["status"] as? String ?? "unobserved",
                  ageMs: (o["ageMs"] as? NSNumber)?.doubleValue ?? 0)
    }

    init(sequence: Int, status: String, ageMs: Double) {
        self.sequence = sequence
        self.status = status
        self.ageMs = ageMs
    }
}

enum AudioObservationPolicy {
    static let freshMs = 250.0
    static let agingMs = 1000.0

    static func ageMs(_ observation: AudioObservation, receivedAt: Date, now: Date) -> Double {
        max(0, observation.ageMs) + max(0, now.timeIntervalSince(receivedAt) * 1000)
    }

    static func state(_ observation: AudioObservation?, receivedAt: Date, now: Date) -> String {
        guard let observation, observation.status != "unobserved", observation.sequence != 0 else {
            return "unobserved"
        }
        let age = ageMs(observation, receivedAt: receivedAt, now: now)
        return age <= freshMs ? "fresh" : age <= agingMs ? "aging" : "stale"
    }

    /// Windows MeterWarning wording. nil = healthy, or a legacy core with no envelope.
    static func warning(_ observation: AudioObservation?, receivedAt: Date, now: Date) -> String? {
        guard let observation else { return nil }
        switch state(observation, receivedAt: receivedAt, now: now) {
        case "unobserved": return "awaiting measured audio"
        case "aging": return "meters aging"
        case "stale": return "meters stale \u{2014} last values held"
        default: return nil
        }
    }
}
