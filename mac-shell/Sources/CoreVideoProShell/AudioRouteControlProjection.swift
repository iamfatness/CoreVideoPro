import Foundation

struct AudioRouteKey: Hashable, Comparable {
    let source: String
    let bus: String

    static func < (lhs: Self, rhs: Self) -> Bool {
        lhs.source == rhs.source ? lhs.bus < rhs.bus : lhs.source < rhs.source
    }
}

// Main-actor-owned editor projection. The native snapshot supplies applied
// routes; local cells remain drafts until their operation appears in a result.
struct AudioRouteControlProjection {
    struct Draft: Equatable {
        let enabled: Bool
        let gainDb: Double
    }

    private(set) var authorityEpoch = ""
    private(set) var revision: Int64 = 0
    private(set) var applied: [AudioRouteKey: Draft] = [:]
    private(set) var drafts: [AudioRouteKey: Draft] = [:]
    private(set) var pendingOperationId: String?
    private(set) var notice = ""
    private var pendingKey: AudioRouteKey?
    private var expected: [AudioRouteKey: (epoch: String, revision: Int64)] = [:]
    private var blocked: Set<AudioRouteKey> = []

    mutating func resetForProcess() {
        authorityEpoch = ""
        revision = 0
        pendingOperationId = nil
        pendingKey = nil
        blocked.formUnion(drafts.keys)
        notice = "Audio route reconciling with restarted core"
    }

    mutating func edit(source: String, bus: String, enabled: Bool, gainDb: Double) {
        let key = AudioRouteKey(source: source, bus: bus)
        let draft = Draft(enabled: enabled, gainDb: gainDb)
        drafts[key] = draft
        expected[key] = (authorityEpoch, revision)
        blocked.remove(key)
        notice = "Audio route edit pending core application"
    }

    mutating func observe(_ matrix: JSONObject) {
        guard let control = matrix["control"] as? JSONObject,
              let epoch = control["authorityEpoch"] as? String, !epoch.isEmpty,
              let number = control["revision"] as? NSNumber else { return }
        let nextRevision = number.int64Value
        if epoch == authorityEpoch && nextRevision < revision { return }
        if epoch != authorityEpoch {
            pendingOperationId = nil
            pendingKey = nil
            if !authorityEpoch.isEmpty && !drafts.isEmpty {
                blocked.formUnion(drafts.keys)
                notice = "Audio route core restarted; draft retained for review"
            }
        }
        authorityEpoch = epoch
        revision = nextRevision
        if let sends = matrix["sends"] as? [JSONObject] {
            applied = Dictionary(uniqueKeysWithValues: sends.compactMap { send in
                guard let source = send["sourceId"] as? String,
                      let bus = send["busId"] as? String else { return nil }
                return (AudioRouteKey(source: source, bus: bus),
                        Draft(enabled: true, gainDb: (send["gainDb"] as? NSNumber)?.doubleValue ?? 0))
            })
        }
        guard let operation = pendingOperationId, let key = pendingKey else { return }
        let results = control["recentResults"] as? [JSONObject] ?? []
        let result = results.first { $0["operationId"] as? String == operation }
            ?? ((control["lastResult"] as? JSONObject).flatMap {
                $0["operationId"] as? String == operation ? $0 : nil
            })
        guard let status = result?["status"] as? String else { return }
        pendingOperationId = nil
        pendingKey = nil
        if status == "applied" {
            if drafts[key] == applied[key] ||
               (drafts[key]?.enabled == false && applied[key] == nil) {
                drafts[key] = nil
                expected[key] = nil
                blocked.remove(key)
            }
            // Other cells from the same local gesture may follow our result.
            for other in drafts.keys where expected[other]?.epoch == epoch &&
                expected[other]?.revision == nextRevision - 1 {
                expected[other] = (epoch, nextRevision)
            }
            notice = drafts.isEmpty ? "" : "Audio route edits pending core application"
        } else {
            blocked.insert(key)
            notice = status == "conflict" ? "Audio route conflicted; draft retained" :
                "Audio route rejected; draft retained"
        }
    }

    mutating func nextCommand() -> JSONObject? {
        guard pendingOperationId == nil, !authorityEpoch.isEmpty else { return nil }
        for key in drafts.keys.sorted() {
            if blocked.contains(key) { continue }
            guard let draft = drafts[key], let base = expected[key], base.epoch == authorityEpoch else { continue }
            if draft == applied[key] || (!draft.enabled && applied[key] == nil) {
                drafts[key] = nil
                expected[key] = nil
                continue
            }
            let operation = UUID().uuidString
            pendingOperationId = operation
            pendingKey = key
            return ["type": "set-audio-route-control", "operationId": operation,
                    "authorityEpoch": base.epoch, "expectedRevision": base.revision,
                    "sourceId": key.source, "busId": key.bus,
                    "enabled": draft.enabled, "gainDb": max(-60, min(10, draft.gainDb))]
        }
        return nil
    }

    func overlayDrafts(on applied: [String: [String: Double]]) -> [String: [String: Double]] {
        var merged = applied
        for (key, draft) in drafts {
            if draft.enabled { merged[key.source, default: [:]][key.bus] = draft.gainDb }
            else { merged[key.source]?[key.bus] = nil }
        }
        return merged
    }

    mutating func markUnconfirmed() {
        if pendingOperationId != nil { notice = "Audio route edit reconciling with core" }
    }
}
