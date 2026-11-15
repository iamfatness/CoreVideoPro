import Foundation

/// Retains an active take's selected identities across passive source loss.
/// Availability admits a writer; it does not revoke one already in the take.
/// Port of native-shell RecordingIsoSelection.cs (#1188: 1d6649c8, 0203e310).
struct RecordingIsoSelection {
    private var active: [String] = []
    private var selected: Set<String>?
    private var assigned: Set<String>?
    private var refreshIntent = false

    mutating func reset() {
        active = []
        selected = nil
        assigned = nil
        refreshIntent = false
    }

    /// An explicit operator ISO edit: the next resolve re-derives the set.
    mutating func noteOperatorEdit() { refreshIntent = true }

    private static func resolveFresh(enabled: Bool, selected: Set<String>,
                                     candidates: [String]) -> [String] {
        guard enabled else { return [] }
        let present = Set(candidates)
        return selected.sorted().filter(present.contains).prefix(8).map { $0 }
    }

    mutating func resolve(recordingRequested: Bool, enabled: Bool,
                          selected selectedIds: Set<String>,
                          eligible: [String], assigned assignedIds: Set<String>) -> [String] {
        if !recordingRequested || !enabled {
            reset()
            return Self.resolveFresh(enabled: enabled, selected: selectedIds, candidates: eligible)
        }
        // Assigning an unrelated, unselected source is not an ISO edit.
        let assignedSet = assignedIds.intersection(selectedIds)
        if !refreshIntent, selected == selectedIds, assigned == assignedSet { return active }
        let candidates = active.filter(assignedSet.contains) + eligible
        active = Self.resolveFresh(enabled: enabled, selected: selectedIds, candidates: candidates)
        selected = selectedIds
        assigned = assignedSet
        refreshIntent = false
        return active
    }
}
