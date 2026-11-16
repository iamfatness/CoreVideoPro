// ShowInputAssignmentLaw — THE LAW for show-input slots on the Mac
// (docs/reference/the-law-slots.md; Windows ShowInputsCoordinator.cs:366-448
// and ShowInputRosterService.cs:340-416).
//
// Sources appear in slots by OPERATOR action or NEWCOMER auto-assign only.
// The Mac used to keep a per-person tombstone written only by Unassign, so a
// guest replaced through the row picker had no tombstone and the next roster
// tick (~1/s) put them back into the first empty slot. This ports the Windows
// model instead: a seen-this-meeting set (auto-assign places only ids it has
// never seen) plus a set of slot NUMBERS the operator emptied (reserved
// against auto-assign until the meeting roster empties).

import Foundation

struct ShowInputAssignmentLaw: Equatable {
    static let slotCount = 10

    /// Roster ids seen this meeting; REPLACED by every accepted roster (a leaver is forgotten,
    /// so a real rejoin is a newcomer again). Windows `_autoAssignSeenParticipantIds`.
    private(set) var seenThisMeeting: Set<String> = []
    /// Slot NUMBERS the operator emptied. Windows `_operatorClearedSlotNumbers`.
    /// Reservations bind auto-assign only: the operator may still place anything there.
    private(set) var operatorClearedSlots: Set<Int> = []
    var autoAssignEnabled = true

    /// A slot freed because its guest left. `name` is the slot's display name at the time,
    /// for the Sources status line.
    struct Freed: Equatable { let slotId: Int; let name: String }

    /// One accepted roster barrier. `roster` is in roster order and is already EMPTY when the
    /// meeting is not `in_meeting`. Frees departed Zoom slots, updates memory, fills newcomers.
    @discardableResult
    mutating func syncFromMeeting(_ slots: inout [ShowInputSlot],
                                  roster: [RosterParticipant]) -> [Freed] {
        let present = Set(roster.map(\.id))
        // "Auto-assign only places ids it has NEVER seen" (ShowInputsCoordinator.cs:366-372).
        let newcomers = roster.filter { !seenThisMeeting.contains($0.id) }
        seenThisMeeting = present
        // "Sticky until the MEETING ROSTER EMPTIES" (owner ruling 2026-09-12, :384-385).
        if roster.isEmpty { operatorClearedSlots.removeAll() }
        let freed = Self.freeDeparted(&slots, present: present)
        if autoAssignEnabled { fill(&slots, from: newcomers) }
        return freed
    }

    /// The operator emptied `slotId` (any kind). "The operator emptying a slot is a
    /// decision, not a vacancy." (:515-517).
    mutating func operatorUnassigned(slotId: Int) {
        operatorClearedSlots.insert(slotId)
    }

    /// The toggle flipped (either direction, D7): forget both memories, then free departed
    /// slots and, when enabled, fill from the WHOLE roster ("an explicit 'assign everyone
    /// now'", :425-438). Unlike Windows, `seenThisMeeting` is set to the roster afterwards,
    /// so an operator unassign between this reapply and the next roster tick cannot make
    /// the removed guest look like a newcomer on that tick.
    @discardableResult
    mutating func reapply(_ slots: inout [ShowInputSlot], roster: [RosterParticipant]) -> [Freed] {
        seenThisMeeting = []
        operatorClearedSlots = []
        let freed = Self.freeDeparted(&slots, present: Set(roster.map(\.id)))
        if autoAssignEnabled { fill(&slots, from: roster) }
        seenThisMeeting = Set(roster.map(\.id))
        return freed
    }

    /// "Free slots whose Zoom participant is no longer in the meeting."
    /// (ShowInputRosterService.cs:365-376). The slot is reset whole, as Unassign does (D10),
    /// and is NOT reserved.
    private static func freeDeparted(_ slots: inout [ShowInputSlot], present: Set<String>) -> [Freed] {
        var freed: [Freed] = []
        for index in slots.indices
        where slots[index].kind == "zoom" && !present.contains(slots[index].sourceId) {
            freed.append(Freed(slotId: slots[index].id, name: slots[index].name))
            slots[index] = ShowInputSlot(id: slots[index].id)
        }
        return freed
    }

    /// Candidates in roster order take the lowest-numbered unassigned slot that the
    /// operator has not reserved (ShowInputRosterService.cs:389-405). Skips the
    /// CoreVideo participant itself (D6) and anyone already in a Zoom slot.
    private func fill(_ slots: inout [ShowInputSlot], from candidates: [RosterParticipant]) {
        var slotted = Set(slots.filter { $0.kind == "zoom" }.map(\.sourceId))
        for participant in candidates where !participant.isMe && !slotted.contains(participant.id) {
            let free = slots.indices
                .filter { slots[$0].kind == "unassigned" && !operatorClearedSlots.contains(slots[$0].id) }
                .min { slots[$0].id < slots[$1].id }
            guard let index = free else { break }
            slots[index].kind = "zoom"
            slots[index].sourceId = participant.id
            slots[index].name = participant.name
            slots[index].sourceName = participant.name
            slots[index].inShow = true
            slots[index].offline = false
            slotted.insert(participant.id)
        }
    }

    /// True when two slot arrays differ in any assignment field the wire reads.
    static func assignmentsDiffer(_ a: [ShowInputSlot], _ b: [ShowInputSlot]) -> Bool {
        a.count != b.count || zip(a, b).contains {
            $0.kind != $1.kind || $0.sourceId != $1.sourceId || $0.inShow != $1.inShow
        }
    }
}
