import Foundation

// Main-actor projection for one core-owned control. The UI keeps a draft while
// the accepted monitor state and revision come only from the core snapshot.
struct MonitorControlProjection {
    struct Draft: Equatable {
        var enabled: Bool
        var volume: Double
    }

    private(set) var authorityEpoch = ""
    private(set) var revision: Int64 = 0
    private(set) var applied = Draft(enabled: false, volume: 0.7)
    private(set) var draft: Draft?
    private(set) var pendingOperationId: String?
    private(set) var notice = ""

    mutating func edit(_ next: Draft) {
        draft = next
        notice = "Monitor edit pending core application"
    }

    mutating func observe(_ mix: JSONObject) {
        guard let control = mix["monitorControl"] as? JSONObject,
              let epoch = control["authorityEpoch"] as? String, !epoch.isEmpty,
              let number = control["revision"] as? NSNumber else { return }
        let nextRevision = number.int64Value
        if epoch == authorityEpoch && nextRevision < revision { return }
        if epoch != authorityEpoch { pendingOperationId = nil }
        authorityEpoch = epoch
        revision = nextRevision
        applied = Draft(enabled: mix["monitorEnabled"] as? Bool ?? applied.enabled,
                        volume: (mix["monitorVolume"] as? NSNumber)?.doubleValue ?? applied.volume)
        guard let operation = pendingOperationId else { return }
        let results = (control["recentResults"] as? [JSONObject] ?? [])
        let result = results.first { $0["operationId"] as? String == operation }
            ?? ((control["lastResult"] as? JSONObject).flatMap {
                $0["operationId"] as? String == operation ? $0 : nil
            })
        guard let status = result?["status"] as? String else { return }
        pendingOperationId = nil
        switch status {
        case "applied":
            if draft == applied { draft = nil; notice = "" }
        case "conflict": notice = "Monitor edit conflicted; draft retained"
        default: notice = "Monitor edit was rejected; draft retained"
        }
    }

    mutating func nextCommand() -> JSONObject? {
        guard pendingOperationId == nil, let draft, !authorityEpoch.isEmpty,
              draft != applied else { return nil }
        let operation = UUID().uuidString
        pendingOperationId = operation
        return ["type": "set-audio-monitor-control", "operationId": operation,
                "authorityEpoch": authorityEpoch, "expectedRevision": revision,
                "enabled": draft.enabled, "deviceId": "",
                "deviceName": "System default output", "volume": draft.volume]
    }

    mutating func markUnconfirmed() {
        if pendingOperationId != nil { notice = "Monitor edit reconciling with core" }
    }
}
