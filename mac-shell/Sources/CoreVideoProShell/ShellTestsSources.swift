// macOS Sources rebuild, Phase 1 (docs/superpowers/plans/2026-11-16-mac-sources-phase1.md).
// Kept in its own file so the parallel Mac branches that also extend
// ShellTests.swift do not collide line-for-line.

import AppKit
import Combine
import Foundation
import SwiftUI

extension ShellTests {

    /// Appended to the main case list by runAndExit().
    @MainActor
    static var sourcesCases: [(String, () -> Void)] {
        [
            // Task 1: the slot law, through the whole roster merge.
            ("sources-law/picker-replace-never-re-adds", testReplacingAGuestByThePickerNeverReAddsThem),
            ("sources-law/guest-for-guest-replace", testReplacingAGuestWithAnotherGuestNeverReAddsTheFirst),
            ("sources-law/cleared-slot-reserved", testARosterSyncNeverRefillsASlotTheOperatorUnassigned),
            ("sources-law/empty-roster-releases", testAnEmptyRosterReleasesTheOperatorsClearedSlots),
            ("sources-law/leaver-freed-rejoin-is-newcomer", testALeaverIsFreedAndARejoinIsANewcomer),
            ("sources-law/out-of-meeting-roster-is-empty", testARosterOutsideTheMeetingFreesEveryZoomSlot),
            ("sources-law/rejected-roster-frees-nothing", testARejectedRosterFreesNothing),
            ("sources-law/toggle-assigns-everyone", testTheAutoAssignToggleMeansAssignEveryoneNow),
            ("sources-law/toggle-off-places-no-newcomer", testAutoAssignOffPlacesNoNewcomerButStillFrees),
            ("sources-law/camera-off-newcomer-and-self", testCameraOffNewcomersArePlacedButNeverOurself),
            ("sources-law/property-newcomers-only", testAutoAssignPlacesOnlyNewcomersProperty),
        ]
    }

    /// A revisioned roster barrier. FIXTURE INVARIANTS: `revision` > 0 and strictly
    /// increasing per model (ZoomRosterProjection.observe rejects a repeated or older
    /// revision as stale: a repeated revision would make every test below vacuous);
    /// the epoch has three ':'-separated parts with numeric process/meeting fields
    /// (ZoomRosterProjection.order); every model starts fresh (epoch empty).
    static func lawRoster(_ ids: [String], revision: Int64, epoch: String = "1:1:law",
                          videoOff: Set<String> = [], isMe: Set<String> = [],
                          meetingState: String = "in_meeting") -> JSONObject {
        ["meetingState": meetingState, "rosterEpoch": epoch,
         "rosterRevision": NSNumber(value: revision),
         "participants": ids.map { id -> JSONObject in
             ["userId": id, "displayName": "Guest \(id)", "videoOn": !videoOff.contains(id),
              "isMe": isMe.contains(id), "sourceGeneration": NSNumber(value: 1)]
         }]
    }

    @MainActor
    static func zoomSlot(_ model: AppModel, holding id: String) -> Int? {
        model.slots.first { $0.kind == "zoom" && $0.sourceId == id }?.id
    }

    // ── Task 1: the slot law ────────────────────────────────────────────────

    /// THE live defect (spec §3.2 item 1). Replacing guest A through the row picker left A
    /// with no tombstone, so the next roster tick put A into the first empty slot.
    /// Driven through the WHOLE merge (applyZoom → ZoomRosterProjection → law → slots):
    /// live-meeting-qa-2026-08-09.md:78-95. Windows: ShowInputsCoordinator.cs:366-374.
    /// Fails on e98436fc: tick 2 puts A into slot 2.
    @MainActor
    static func testReplacingAGuestByThePickerNeverReAddsThem() {
        let model = AppModel()
        model.applyZoomForTesting(lawRoster(["A"], revision: 1))
        expectEqual(zoomSlot(model, holding: "A"), 1, "precondition: newcomer A is auto-assigned to slot 1")
        model.assignSlot(1, kind: "capture", sourceId: "cam1", name: "Wide cam")   // the row picker
        for revision in Int64(2)...4 {                                              // three roster ticks
            model.applyZoomForTesting(lawRoster(["A"], revision: revision))
        }
        expect(model.roster.contains { $0.id == "A" }, "A is still in the meeting (this is not a departure)")
        expectEqual(zoomSlot(model, holding: "A"), nil, "roster sync never puts A back")
        expect(model.slots.filter { $0.id != 1 }.allSatisfy { $0.kind == "unassigned" },
               "every other slot stays empty: nothing is invented")
        expectEqual(model.slots[0].sourceId, "cam1", "the operator's choice stands")
    }

    /// Same defect, guest for guest: remove B, then put B where A was. Fails on e98436fc
    /// (A has no tombstone and lands in slot 2).
    @MainActor
    static func testReplacingAGuestWithAnotherGuestNeverReAddsTheFirst() {
        let model = AppModel()
        model.applyZoomForTesting(lawRoster(["A", "B"], revision: 1))        // A→1, B→2
        model.unassignSlot(2)
        model.assignSlot(1, kind: "zoom", sourceId: "B", name: "Guest B")
        for revision in Int64(2)...4 { model.applyZoomForTesting(lawRoster(["A", "B"], revision: revision)) }
        expectEqual(zoomSlot(model, holding: "B"), 1, "B stays where the operator put it")
        expectEqual(zoomSlot(model, holding: "A"), nil, "A is not re-added")
        expectEqual(model.slots[1].kind, "unassigned", "the slot the operator emptied stays empty")
    }

    /// Port of ShowInputsCoordinatorTests.ARosterSyncNeverRefillsASlotTheOperatorUnassigned (:839).
    /// FIXTURE INVARIANT: exactly ShowInputAssignmentLaw.slotCount (10) guests, so the cleared slot
    /// is the ONLY free slot (the-law-slots.md:41-44 — with a spare earlier free slot the test
    /// passes with and without the fix). Fails on e98436fc (p-new lands in slot 4).
    @MainActor
    static func testARosterSyncNeverRefillsASlotTheOperatorUnassigned() {
        let model = AppModel()
        let guests = (0..<10).map { "p\($0)" }
        model.applyZoomForTesting(lawRoster(guests, revision: 1))
        expectEqual(model.slots.map(\.sourceId), guests, "precondition: p0..p9 fill slots 1..10 in roster order")
        model.unassignSlot(4)                                                    // evicts p3
        model.applyZoomForTesting(lawRoster(guests.filter { $0 != "p3" } + ["p-new"], revision: 2))
        expectEqual(model.slots[3].kind, "unassigned", "slot 4 stays empty")
        expectEqual(zoomSlot(model, holding: "p-new"), nil, "the newcomer is not given the reserved slot")
        model.assignSlot(4, kind: "zoom", sourceId: "p-new", name: "Guest p-new")   // reserved against AUTO only
        model.applyZoomForTesting(lawRoster(guests.filter { $0 != "p3" } + ["p-new"], revision: 3))
        expectEqual(zoomSlot(model, holding: "p-new"), 4, "the operator may still place anyone there, and it sticks")
    }

    /// Port of AnEmptyRosterReleasesTheOperatorsClearedSlots (:875). Fails on e98436fc (old
    /// slots stay, offline, after the empty roster).
    @MainActor
    static func testAnEmptyRosterReleasesTheOperatorsClearedSlots() {
        let model = AppModel()
        model.applyZoomForTesting(lawRoster((0..<10).map { "p\($0)" }, revision: 1))
        model.unassignSlot(4)
        model.applyZoomForTesting(lawRoster([], revision: 2))                    // the meeting ends
        expect(model.slots.allSatisfy { $0.kind == "unassigned" }, "every Zoom slot frees on the empty roster")
        let next = (0..<10).map { "q\($0)" }
        model.applyZoomForTesting(lawRoster(next, revision: 3))
        expectEqual(model.slots.map(\.sourceId), next,
                    "a new meeting fills all ten slots in order: slot 4 is ordinary again")
    }

    /// Rule 3 + "a leaver is forgotten" (ShowInputsCoordinator.cs:398-399). Fails on e98436fc.
    @MainActor
    static func testALeaverIsFreedAndARejoinIsANewcomer() {
        let model = AppModel()
        model.applyZoomForTesting(lawRoster(["A", "B"], revision: 1))
        model.applyZoomForTesting(lawRoster(["B"], revision: 2))
        expectEqual(model.slots[0].kind, "unassigned", "A's slot is freed when A leaves")
        expectEqual(model.slots[0].inShow, false, "and leaves the show")
        expectEqual(model.sourcesStatus, "Guest A left the meeting — input 1 is free.", "the status line names it")
        model.applyZoomForTesting(lawRoster(["A", "B"], revision: 3))
        expectEqual(zoomSlot(model, holding: "A"), 1, "A rejoining is a newcomer and takes the first free slot")
    }

    /// Spec correction 3 (LiveProductionSync.cs:119-123): a roster outside in_meeting is empty.
    @MainActor
    static func testARosterOutsideTheMeetingFreesEveryZoomSlot() {
        let model = AppModel()
        model.applyZoomForTesting(lawRoster(["A"], revision: 1))
        model.applyZoomForTesting(lawRoster(["A"], revision: 2, meetingState: "leaving"))
        expectEqual(zoomSlot(model, holding: "A"), nil, "a non-meeting roster frees the slot")
    }

    /// Rule 4. The first half fails nothing on e98436fc (the old code never frees); it guards
    /// the gate mutation. CONFIRMATORY against the old code, INDEPENDENT against mutation M5.
    @MainActor
    static func testARejectedRosterFreesNothing() {
        let model = AppModel()
        model.applyZoomForTesting(lawRoster(["A", "B"], revision: 2))
        model.applyZoomForTesting(lawRoster([], revision: 1))                    // stale: rejected by the projection
        expectEqual(model.slots.prefix(2).map(\.sourceId), ["A", "B"], "a stale roster frees nothing")
        let legacy = AppModel()                                                  // an older core: no revision contract
        legacy.applyZoomForTesting(["meetingState": "in-meeting",
                                    "participants": [["userId": "A", "displayName": "A", "videoOn": true]]])
        expectEqual(zoomSlot(legacy, holding: "A"), 1, "precondition: unrevisioned rosters are accepted")
        legacy.applyZoomForTesting(["meetingState": "in-meeting", "participants": [JSONObject]()])
        expectEqual(zoomSlot(legacy, holding: "A"), 1,
                    "an unrevisioned EMPTY roster is no roster fact (StudioViewModel.cs:11128) and frees nothing")
    }

    /// Ports of ReapplyAutoAssign_* (:785, :800) through the model, with reservations in play.
    @MainActor
    static func testTheAutoAssignToggleMeansAssignEveryoneNow() {
        let model = AppModel()
        model.applyZoomForTesting(lawRoster(["A", "B"], revision: 1))            // A→1, B→2
        model.unassignSlot(1); model.unassignSlot(2)                             // reserved and seen
        model.applyZoomForTesting(lawRoster(["A", "B", "C"], revision: 2))
        expectEqual(zoomSlot(model, holding: "C"), 3, "precondition: newcomer C skips the reserved slots")
        model.setAutoAssignInputs(false)
        expectEqual(model.slots.filter { $0.kind == "zoom" }.map(\.sourceId), ["C"], "switching off places nobody")
        model.setAutoAssignInputs(true)
        expectEqual(zoomSlot(model, holding: "A"), 1, "switching on assigns everyone, into released slot 1")
        expectEqual(zoomSlot(model, holding: "B"), 2, "and slot 2")
        expectEqual(zoomSlot(model, holding: "C"), 3, "C keeps its slot: no reshuffle")
    }

    @MainActor
    static func testAutoAssignOffPlacesNoNewcomerButStillFrees() {
        let model = AppModel()
        model.setAutoAssignInputs(false)
        model.applyZoomForTesting(lawRoster(["p1"], revision: 1))
        expect(!model.slots.contains { $0.kind == "zoom" }, "off: a newcomer is not placed")
        model.assignSlot(5, kind: "zoom", sourceId: "p1", name: "Guest p1")
        model.applyZoomForTesting(lawRoster([], revision: 2))
        expectEqual(model.slots[4].kind, "unassigned", "off still frees a departed guest's slot")
    }

    /// D3 and D6. The isMe assertion passes on e98436fc too (the bot is camera-off):
    /// CONFIRMATORY there, INDEPENDENT once camera-off newcomers are placed.
    @MainActor
    static func testCameraOffNewcomersArePlacedButNeverOurself() {
        let model = AppModel()
        model.applyZoomForTesting(lawRoster(["me", "A"], revision: 1, videoOff: ["me", "A"], isMe: ["me"]))
        expectEqual(zoomSlot(model, holding: "A"), 1, "a camera-off newcomer is placed (Windows passes the whole roster)")
        expectEqual(zoomSlot(model, holding: "me"), nil, "the CoreVideo participant itself is never placed")
    }

    /// Quantification (rule 3): 120 seeds × 8 ticks over a pool of 14 ids; roster size 0…12
    /// (11+ forces overflow, 0 is a real empty barrier); order shuffled each tick; camera
    /// state random per id per tick; the operator unassigns a random Zoom slot one tick in
    /// three; auto-assign off for one seed in three. Every "expected" operand comes from the
    /// TEST's own record of the previous roster and of the slots it unassigned (rule 2).
    @MainActor
    static func testAutoAssignPlacesOnlyNewcomersProperty() {
        let pool = (0..<14).map { "u\($0)" }
        for seed in 0..<120 {
            var rng = SplitMix64(seed: UInt64(seed))
            let model = AppModel()
            if seed % 3 == 2 { model.setAutoAssignInputs(false) }
            let autoOn = model.autoAssignInputsEnabled
            var previous: Set<String> = []          // the test's own memory of the last accepted roster
            var reserved: Set<Int> = []             // slots the test emptied since the last empty roster
            var violations: [String] = []
            for tick in 1...8 {
                if tick > 1, rng.next() % 3 == 0,
                   let target = model.slots.filter({ $0.kind == "zoom" }).randomElement(using: &rng) {
                    model.unassignSlot(target.id); reserved.insert(target.id)
                }
                let present = Array(pool.shuffled(using: &rng).prefix(Int(rng.next() % 13)))
                let off = Set(present.filter { _ in rng.next() % 2 == 0 })
                let presentSet = Set(present)
                let before = model.slots
                // Expected fill, computed independently: departed Zoom slots free first; then
                // newcomers (present − previous − already slotted), in roster order, take the
                // lowest-numbered unassigned slots outside `reserved`.
                let freed = before.filter { $0.kind == "zoom" && !presentSet.contains($0.sourceId) }.map(\.id)
                if present.isEmpty { reserved.removeAll() }
                let slotted = Set(before.filter { $0.kind == "zoom" && presentSet.contains($0.sourceId) }.map(\.sourceId))
                let free = before.filter { ($0.kind == "unassigned" || freed.contains($0.id)) && !reserved.contains($0.id) }
                    .map(\.id).sorted()
                let newcomers = present.filter { !previous.contains($0) && !slotted.contains($0) }
                var expected: [Int: String] = [:]
                if autoOn { for (slot, id) in zip(free, newcomers) { expected[slot] = id } }

                model.applyZoomForTesting(lawRoster(present, revision: Int64(tick), videoOff: off))

                for slot in model.slots {
                    let was = before.first { $0.id == slot.id }!
                    let placed = slot.kind == "zoom" && !(was.kind == "zoom" && was.sourceId == slot.sourceId)
                    if placed, expected[slot.id] != slot.sourceId {
                        violations.append("tick \(tick): slot \(slot.id) got \(slot.sourceId), expected \(expected[slot.id] ?? "nothing")")
                    }
                    if !placed, let want = expected[slot.id] {
                        violations.append("tick \(tick): slot \(slot.id) should have taken newcomer \(want)")
                    }
                    if slot.kind == "zoom", !presentSet.contains(slot.sourceId) {
                        violations.append("tick \(tick): departed \(slot.sourceId) still holds slot \(slot.id)")
                    }
                }
                let ids = model.slots.filter { $0.kind == "zoom" }.map(\.sourceId)
                if Set(ids).count != ids.count { violations.append("tick \(tick): a guest holds two slots \(ids)") }
                previous = presentSet
            }
            expect(violations.isEmpty, "seed \(seed): \(violations.first ?? "")")
        }
    }
}

struct SplitMix64: RandomNumberGenerator {
    var state: UInt64
    init(seed: UInt64) { state = seed }
    mutating func next() -> UInt64 {
        state &+= 0x9E37_79B9_7F4A_7C15
        var z = state
        z = (z ^ (z >> 30)) &* 0xBF58_476D_1CE4_E5B9
        z = (z ^ (z >> 27)) &* 0x94D0_49BB_1331_11EB
        return z ^ (z >> 31)
    }
}
