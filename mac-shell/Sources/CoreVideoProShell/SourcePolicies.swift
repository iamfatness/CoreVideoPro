// SourcePolicies — capture dropout status and meeting-scoped production roles
// (sources plan Task 9). Windows Services/CaptureDropoutPresentation.cs and
// Services/ProductionRoleService.cs + StudioViewModel.SetParticipantProductionRole.

import Foundation

enum CaptureDropoutStatus {
    static let unavailableNotice = "This source does not report signal loss yet, so it always shows its last picture."
    static let policies = [("hold", "Hold last frame (up to 5 s, then slate)"), ("black", "Black (then slate)")]

    /// Port of CaptureDropoutPresentation.Status (:18-37).
    static func line(_ row: BusSourceRow?) -> String {
        guard let row, applies(row) else { return "" }
        let reason = (row.lossReason ?? "").trimmingCharacters(in: .whitespaces).isEmpty
            ? "signal loss" : (row.lossReason ?? "").replacingOccurrences(of: "-", with: " ")
        let black = row.dropoutPolicy == "black"
        switch (row.liveness, row.onAir) {
        case ("lost", "failed"):
            return "Lost (\(reason)) — slate on Program"
        case ("lost", _):
            return "Lost (\(reason)) — " + (black ? "Program black" : "holding")
                + ", slate in \(seconds(row.failInMs))"
        case ("stalled", _):
            return "No new frames for \(seconds(row.lastSampleAgeMs)) — "
                + (black ? "Program black" : "holding last frame")
        case ("live", _):
            return "Live"
        default:
            return ""
        }
    }

    /// The core applies a dropout policy to this capture source (it reports signal loss).
    static func applies(_ row: BusSourceRow?) -> Bool {
        row?.kind == "capture" && row?.dropoutPolicyApplies == true
    }

    private static func seconds(_ ms: Double?) -> String {
        guard let ms else { return "unknown" }
        return String(format: "%.1f s", locale: Locale(identifier: "en_US_POSIX"), ms / 1000)
    }
}

struct ProductionRoles: Equatable {
    /// Windows ProductionRoleService.Roles (ProductionRoleService.cs:17-28), values and labels verbatim.
    static let roles: [(value: String, label: String)] = [
        ("host", "Host"), ("co-host", "Co-host"), ("reader", "Reader"), ("panelist", "Panelist"),
        ("guest-1", "Guest 1"), ("guest-2", "Guest 2"), ("guest-3", "Guest 3"), ("guest-4", "Guest 4"),
    ]

    private(set) var byParticipant: [String: String] = [:]

    static func label(_ role: String) -> String {
        roles.first { $0.value.caseInsensitiveCompare(role) == .orderedSame }?.label ?? role
    }

    static func == (a: ProductionRoles, b: ProductionRoles) -> Bool { a.byParticipant == b.byParticipant }

    /// One holder per role (StudioViewModel.cs:5041-5051). Refused when the participant is absent
    /// from `present`. nil/blank clears. Returns the status line Windows writes, or nil when unchanged.
    mutating func set(_ participantId: String, role: String?, present: [RosterParticipant]) -> String? {
        guard let participant = present.first(where: { $0.id == participantId }) else { return nil }
        let trimmed = role?.trimmingCharacters(in: .whitespacesAndNewlines) ?? ""
        let normalized: String? = trimmed.isEmpty ? nil : trimmed
        let current = byParticipant[participantId]
        if let current, let normalized, current.caseInsensitiveCompare(normalized) == .orderedSame { return nil }
        if current == nil && normalized == nil { return nil }
        guard let normalized else {
            byParticipant[participantId] = nil
            return "\(participant.name) role cleared"
        }
        if let previous = byParticipant.first(where: {
            $0.value.caseInsensitiveCompare(normalized) == .orderedSame })?.key {
            byParticipant[previous] = nil
        }
        byParticipant[participantId] = normalized
        return "\(participant.name) is now \(Self.label(normalized))"
    }

    /// Meeting-scoped: called on an empty accepted roster.
    mutating func clear() { byParticipant.removeAll() }
}
