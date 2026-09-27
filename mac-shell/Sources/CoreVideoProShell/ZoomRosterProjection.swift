import Foundation

// A Zoom snapshot is a complete roster barrier. Keep the current UI roster
// when a delayed poll from an older meeting or revision arrives.
struct ZoomRosterProjection {
    private(set) var epoch = ""
    private(set) var revision: Int64 = 0
    private(set) var gaps: Int64 = 0
    private(set) var stale: Int64 = 0
    private(set) var notice = ""
    private var retiredEpochs: Set<String> = []

    mutating func resetForProcess() {
        if !epoch.isEmpty { retiredEpochs.insert(epoch) }
        epoch = ""
        revision = 0
        notice = "Zoom roster reconciling with restarted core"
    }

    func isRetiredOrOlder(_ snapshot: JSONObject) -> Bool {
        guard let candidate = snapshot["rosterEpoch"] as? String, !candidate.isEmpty,
              let number = snapshot["rosterRevision"] as? NSNumber else { return !epoch.isEmpty }
        if retiredEpochs.contains(candidate) { return true }
        if candidate == epoch { return number.int64Value < revision }
        guard epoch.isEmpty else {
            guard let currentOrder = Self.order(epoch),
                  let candidateOrder = Self.order(candidate) else { return true }
            return candidateOrder <= currentOrder
        }
        return false
    }

    mutating func observe(_ snapshot: JSONObject) -> Bool {
        guard let candidate = snapshot["rosterEpoch"] as? String, !candidate.isEmpty,
              let number = snapshot["rosterRevision"] as? NSNumber,
              number.int64Value > 0 else {
            if !epoch.isEmpty { stale += 1; return false }
            // An older core has no revisioned roster contract.
            return true
        }
        let next = number.int64Value
        if retiredEpochs.contains(candidate) { stale += 1; return false }
        if candidate == epoch {
            if next <= revision { stale += 1; return false }
            if next > revision + 1 { gaps += next - revision - 1 }
        } else if !epoch.isEmpty {
            guard let currentOrder = Self.order(epoch),
                  let candidateOrder = Self.order(candidate),
                  candidateOrder > currentOrder else { stale += 1; return false }
            retiredEpochs.insert(epoch)
        }
        epoch = candidate
        revision = next
        notice = ""
        return true
    }

    private static func order(_ epoch: String) -> (Int64, Int64)? {
        let parts = epoch.split(separator: ":", maxSplits: 2)
        guard parts.count == 3, let process = Int64(parts[0]),
              let meeting = Int64(parts[1]) else { return nil }
        return (process, meeting)
    }
}
