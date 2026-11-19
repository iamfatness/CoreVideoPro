// ShowInputPersistence — show-input slot assignments survive a relaunch
// (Tier 0.8; Windows ShowInputRosterStore + ShowInputsCoordinator
// LoadShowInputRoster/SaveShowInputRoster).
//
// Before this, slots started empty every launch, while the per-slot chroma key,
// mic pairing and dropout policy were persisted BY SLOT NUMBER — so after a
// relaunch they attached to whatever source auto-assign happened to drop into
// that slot. Now the assignment itself is restored, and those per-source
// settings are reattached by the source's identity (`sourceKey`), never by
// position.

import Foundation

extension ShowInputSlot {
    /// The scheme-qualified id the wire, the ISO selection and the routing
    /// matrix all use: "zoom:<pid>" / "capture:<deviceId>"; nil when unassigned.
    var sourceKey: String? {
        guard !sourceId.isEmpty else { return nil }
        switch kind {
        case "zoom": return "zoom:" + sourceId
        case "capture": return "capture:" + sourceId
        default: return nil
        }
    }
}

/// #1213 (owner ruling 2026-11-16): a hand-typed slot name lasts one session
/// and one source. A slot keeps the source's own name (`sourceName`) beside the
/// displayed `name`; "renamed by hand" is simply `name != sourceName`.
enum SlotNamePolicy {
    /// Assigning the SAME source again keeps a rename; a different source
    /// (kind or id) shows its own name.
    static func afterAssign(_ slot: ShowInputSlot, kind: String, sourceId: String,
                            sourceName: String) -> (name: String, sourceName: String) {
        let sameSource = slot.kind == kind && slot.sourceId == sourceId
        let keepRename = sameSource && !slot.name.isEmpty
        return (keepRename ? slot.name : sourceName, sourceName)
    }

    /// What is written to disk: the source's own name, never the rename.
    static func persisted(_ slot: ShowInputSlot) -> String {
        slot.sourceName.isEmpty ? slot.name : slot.sourceName
    }

    /// The source is present and reports `liveName`. A slot that was not
    /// renamed this session follows it (this is also what retires a manual name
    /// saved by a build before #1213); a renamed slot keeps its rename and only
    /// learns the source's name. An unknown live name changes nothing.
    static func withLiveName(name: String, sourceName: String,
                             liveName: String) -> (name: String, sourceName: String) {
        guard !liveName.isEmpty else { return (name, sourceName) }
        return (name == sourceName ? liveName : name, liveName)
    }
}

extension AppModel {
    /// Restores operator intent: kind, source, name, in-show. A restored source
    /// reads offline until the roster / device list produces it (Windows marks
    /// unresolved sources unavailable rather than dropping the assignment).
    /// A duplicate of an earlier slot's source is dropped (Windows L7 GC).
    func restoreSlotAssignments(_ saved: [PersistedSlot]) {
        var seen: Set<String> = []
        for record in saved.sorted(by: { $0.slotId < $1.slotId }) {
            // Capture only (plan D2): a Zoom record in an older file is skipped —
            // Zoom reuses user ids across meetings.
            guard record.kind == "capture",
                  !record.sourceId.isEmpty,
                  let index = slots.firstIndex(where: { $0.id == record.slotId }),
                  slots[index].kind == "unassigned" else { continue }
            var slot = ShowInputSlot(id: record.slotId)
            slot.kind = record.kind
            slot.sourceId = record.sourceId
            slot.name = record.name
            slot.sourceName = record.name
            slot.inShow = record.inShow
            slot.offline = true
            guard let key = slot.sourceKey, seen.insert(key).inserted else { continue }
            ShowInputWriteScope.run("restore") { slots[index] = slot }
            if record.kind == "capture" { captureReconnectPending.insert(record.sourceId) }
        }
        assignedIds = Set(slots.filter { $0.kind == "zoom" && $0.inShow }.map(\.sourceId))
    }

    /// Where a persisted per-source setting belongs: the slot now holding that
    /// SOURCE. A legacy record (no sourceKey) falls back to its slot number.
    func restoredSlotIndex(sourceKey: String?, legacySlotId: Int) -> Int? {
        if let sourceKey {
            return slots.firstIndex { $0.sourceKey == sourceKey }
        }
        return slots.firstIndex { $0.id == legacySlotId }
    }

    /// A restored capture slot (or every capture slot after a core respawn)
    /// reconnects its device once the core lists it — the same connect the
    /// Sources menu sends when the operator picks the device.
    func reconnectRestoredCaptures() {
        guard !captureReconnectPending.isEmpty else { return }
        for device in captureDevices where captureReconnectPending.contains(device.id) {
            captureReconnectPending.remove(device.id)
            guard slots.contains(where: { $0.kind == "capture" && $0.sourceId == device.id }),
                  device.connectionState != "connected" else { continue }
            connectCaptureDevice(device)
        }
    }
}
