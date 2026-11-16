import Foundation

// Main-actor projection for one core-owned control. The UI keeps a draft while
// the accepted monitor state and revision come only from the core snapshot.
struct MonitorControlProjection {
    struct Draft: Equatable {
        var enabled: Bool
        var volume: Double
        /// CoreAudio device UID; "" = the system default output (Tier 0.12).
        var deviceId = ""
        var deviceName = MonitorDevicePolicy.systemDefaultName

        /// The name is display text the core echoes back in its own words;
        /// only the device id is the operator's choice.
        static func == (lhs: Draft, rhs: Draft) -> Bool {
            lhs.enabled == rhs.enabled && lhs.volume == rhs.volume && lhs.deviceId == rhs.deviceId
        }
    }

    private(set) var authorityEpoch = ""
    private(set) var revision: Int64 = 0
    private(set) var applied = Draft(enabled: false, volume: 0.7)
    private(set) var draft: Draft?
    private(set) var pendingOperationId: String?
    private(set) var notice = ""
    private(set) var legacyCoreConfirmed = false
    private var draftExpectedEpoch = ""
    private var draftExpectedRevision: Int64 = 0
    private var retiredEpochs: Set<String> = []
    /// A remote client's operation id (remoteEdit), sent by the next command.
    private var pinnedOperationId: String?
    /// Result status per operation id the core answered (bounded), for outcome(of:).
    private var outcomes: [String: String] = [:]
    private var outcomeOrder: [String] = []
    /// The last result status observed for any operation ("" until one is).
    private(set) var lastResultStatus = ""

    mutating func resetForProcess() {
        if !authorityEpoch.isEmpty { retiredEpochs.insert(authorityEpoch) }
        authorityEpoch = ""
        revision = 0
        pendingOperationId = nil
        legacyCoreConfirmed = false
        draftExpectedEpoch = ""
        draftExpectedRevision = 0
        notice = "Monitor edit reconciling with restarted core"
    }

    mutating func edit(_ next: Draft) {
        draft = next
        pinnedOperationId = nil   // a UI edit never carries a remote client's id
        // Capture before the async command path; a concurrent client may apply
        // while this editor is waiting to send.
        draftExpectedEpoch = authorityEpoch
        draftExpectedRevision = revision
        notice = "Monitor edit pending core application"
    }

    /// A remote edit (audio.monitor.set / .volume): like `edit`, but pinned to
    /// the caller's epoch and revision when given (else the current applied
    /// ones), and to the caller's operation id, so the core's answer can be
    /// matched to this request.
    mutating func remoteEdit(_ next: Draft, expectedEpoch: String?, expectedRevision: Int64?, operationId: String?) {
        draft = next
        draftExpectedEpoch = expectedEpoch ?? authorityEpoch
        draftExpectedRevision = expectedRevision ?? revision
        pinnedOperationId = operationId ?? UUID().uuidString
        notice = "Monitor edit pending core application"
    }

    /// nil while the core has not answered `operationId`.
    func outcome(of operationId: String) -> MonitorControlOutcome? {
        switch outcomes[operationId] {
        case nil: return nil
        case "applied": return .applied
        case "conflict": return .conflict(revision: revision)
        default: return .rejected
        }
    }

    private mutating func record(_ operation: String, _ status: String) {
        if outcomes[operation] == nil { outcomeOrder.append(operation) }
        outcomes[operation] = status
        lastResultStatus = status
        if outcomeOrder.count > 32 { outcomes[outcomeOrder.removeFirst()] = nil }
    }

    @discardableResult mutating func observe(_ mix: JSONObject) -> Bool {
        legacyCoreConfirmed = mix["monitorControl"] == nil
        guard let control = mix["monitorControl"] as? JSONObject,
              let epoch = control["authorityEpoch"] as? String, !epoch.isEmpty,
              let number = control["revision"] as? NSNumber else { return authorityEpoch.isEmpty }
        let nextRevision = number.int64Value
        if retiredEpochs.contains(epoch) ||
            (epoch == authorityEpoch && nextRevision < revision) { return false }
        if epoch != authorityEpoch {
            if !authorityEpoch.isEmpty { retiredEpochs.insert(authorityEpoch) }
            pendingOperationId = nil
        }
        authorityEpoch = epoch
        revision = nextRevision
        applied = Draft(enabled: mix["monitorEnabled"] as? Bool ?? applied.enabled,
                        volume: (mix["monitorVolume"] as? NSNumber)?.doubleValue ?? applied.volume,
                        deviceId: mix["monitorDeviceId"] as? String ?? applied.deviceId,
                        deviceName: mix["monitorDeviceName"] as? String ?? applied.deviceName)
        if pendingOperationId == nil, draft == applied {
            draft = nil
            notice = ""
            return true
        }
        guard let operation = pendingOperationId else { return true }
        let results = (control["recentResults"] as? [JSONObject] ?? [])
        let result = results.first { $0["operationId"] as? String == operation }
            ?? ((control["lastResult"] as? JSONObject).flatMap {
                $0["operationId"] as? String == operation ? $0 : nil
            })
        guard let status = result?["status"] as? String else { return true }
        pendingOperationId = nil
        record(operation, status)
        switch status {
        case "applied":
            if draft == applied { draft = nil; notice = "" }
        case "conflict": notice = "Monitor edit conflicted; draft retained"
        default: notice = "Monitor edit was rejected; draft retained"
        }
        return true
    }

    mutating func nextCommand() -> JSONObject? {
        guard pendingOperationId == nil, let draft, !authorityEpoch.isEmpty,
              draft != applied else { return nil }
        let operation = pinnedOperationId ?? UUID().uuidString
        pinnedOperationId = nil
        pendingOperationId = operation
        return ["type": "set-audio-monitor-control", "operationId": operation,
                "authorityEpoch": draftExpectedEpoch.isEmpty ? authorityEpoch : draftExpectedEpoch,
                "expectedRevision": draftExpectedEpoch.isEmpty ? revision : draftExpectedRevision,
                "enabled": draft.enabled, "deviceId": draft.deviceId,
                "deviceName": draft.deviceName, "volume": draft.volume]
    }

    mutating func markUnconfirmed() {
        if pendingOperationId != nil { notice = "Monitor edit reconciling with core" }
    }
}

/// What a remote monitor edit came to (Windows ApplyAudioMonitorControlAsync kinds).
enum MonitorControlOutcome: Equatable { case applied, conflict(revision: Int64), rejected, unconfirmed }
