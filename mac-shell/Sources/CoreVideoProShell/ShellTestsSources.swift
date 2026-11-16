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
            // Task 2: every slot write logged with its writer; saved at once; one restore.
            ("sources-log/write-scope-chains-restores-throws", testWriteScopeChainsRestoresAndSurvivesAThrow),
            ("sources-log/every-writer-names-itself", testEverySlotWriterNamesItself),
            ("sources-save/slot-changes-reach-disk-now", testSlotChangesReachDiskWithoutTheAutosave),
            ("sources-save/second-restore-refused", testASecondRestoreIsRefused),
            // Task 3: Zoom slots are meeting-scoped; the toggle is persisted and drawn.
            ("sources-law/zoom-slots-meeting-scoped", testZoomSlotsAreNeverRestoredOrSaved),
            ("sources-law/toggle-persisted-default-on", testTheAutoAssignToggleIsPersistedAndDefaultsOn),
            // Task 4: SourceRowFactsProjection — observed facts, never requests.
            ("sources-facts/zoom-state-table", testZoomRowStateTable),
            ("sources-facts/capture-reads-observations-only", testCaptureRowReadsObservationsNeverRequests),
            ("sources-facts/reused-zoom-id-fenced", testReusedZoomIdClearsOldFormatAndRejectsOldFacts),
            ("sources-facts/epoch-and-frame-clock-fence", testANewEpochAndAnOlderFrameAreRejected),
            ("sources-facts/tally-precedence-all-inputs", testTallyPrecedenceOverEveryInputCombination),
            ("sources-facts/iso-chip-is-writer-lifecycle", testIsoChipUsesObservedWriterLifecycle),
            ("sources-facts/reassignment-resets-row", testReassigningASlotResetsItsRowBeforeTheNewSourceShows),
            // Task 5: facts wired from the core; picker guard.
            ("sources-facts/wired-zoom-format-one-row-no-sync", testFormatEventsPatchOneRowWithoutASync),
            ("sources-facts/wired-capture-bus-not-device", testCaptureRowIsWiredFromTheBus),
            ("sources-facts/wired-tally-and-writer", testTallyAndWriterAreWiredFromTheSnapshot),
            ("sources-facts/picker-guard", testAStalePickerChoiceIsRefused),
            // Task 6: types, picker groups, column layout, workspace state.
            ("sources-table/column-widths-windows-breakpoints", testColumnWidthsMatchTheWindowsBreakpoints),
            ("sources-table/types-and-categories", testSourceTypesLabelsAndCategories),
            ("sources-table/picker-groups", testPickerGroupsInWindowsOrderWithTheSavedUnavailableEntry),
            ("sources-table/filters-never-write", testInspectionFiltersAndColumnsNeverChangeAssignmentsOrLiveSelections),
            ("sources-table/column-prefs-round-trip", testColumnPreferencesRoundTripAndMissingFieldsDefaultVisible),
            ("sources-table/summary-copy", testSummaryUsesTheWindowsCopy),
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

    // ── Task 2: slot-write log, immediate save, one restore ─────────────────

    /// Ports ShowInputAssignmentLawTests.WriteScope_DefaultsToUntracked_ChainsOnNesting_AndRestores (:21)
    /// and WriteScope_SurvivesAThrowingBody (:43).
    @MainActor
    static func testWriteScopeChainsRestoresAndSurvivesAThrow() {
        expectEqual(ShowInputWriteScope.reason, "UNTRACKED", "outside any scope")
        ShowInputWriteScope.run("outer") {
            expectEqual(ShowInputWriteScope.reason, "outer", "entered")
            ShowInputWriteScope.run("inner") {
                expectEqual(ShowInputWriteScope.reason, "outer>inner", "nested scopes chain")
            }
            expectEqual(ShowInputWriteScope.reason, "outer", "inner restored")
        }
        expectEqual(ShowInputWriteScope.reason, "UNTRACKED", "outer restored")
        struct Boom: Error {}
        _ = try? ShowInputWriteScope.run("throwing") { throw Boom() }
        expectEqual(ShowInputWriteScope.reason, "UNTRACKED", "a throwing body restores the reason")
    }

    /// Rule 10: drives every production writer through its real entry point and asserts the exact
    /// line, so deleting one scope (or one field from the diff) turns a specific assertion red.
    @MainActor
    static func testEverySlotWriterNamesItself() {
        let model = AppModel()
        var lines: [String] = []
        model.slotWriteObserverForTesting = { lines.append($0) }

        model.assignSlot(3, kind: "capture", sourceId: "cam1", name: "Cam")
        expectEqual(lines, ["slot-write: slot3 kind 'unassigned'->'capture' by=operator-assign",
                            "slot-write: slot3 sourceId ''->'cam1' by=operator-assign",
                            "slot-write: slot3 inShow 'false'->'true' by=operator-assign"], "operator assign")
        lines = []; model.toggleSlotInShow(3)
        expectEqual(lines, ["slot-write: slot3 inShow 'true'->'false' by=operator-in-show"], "in-show toggle")
        lines = []; model.unassignSlot(3)
        expectEqual(lines, ["slot-write: slot3 kind 'capture'->'unassigned' by=operator-unassign",
                            "slot-write: slot3 sourceId 'cam1'->'' by=operator-unassign"],
                    "unassign (inShow was already false)")
        lines = []; model.applyZoomForTesting(lawRoster(["A"], revision: 1))     // slot 3 reserved → slot 1
        expectEqual(lines, ["slot-write: slot1 kind 'unassigned'->'zoom' by=roster-sync",
                            "slot-write: slot1 sourceId ''->'A' by=roster-sync",
                            "slot-write: slot1 inShow 'false'->'true' by=roster-sync"], "auto-assign")
        lines = []; model.applyZoomForTesting(lawRoster([], revision: 2))
        expectEqual(lines, ["slot-write: slot1 kind 'zoom'->'unassigned' by=roster-sync",
                            "slot-write: slot1 sourceId 'A'->'' by=roster-sync",
                            "slot-write: slot1 inShow 'true'->'false' by=roster-sync"], "departure")
        model.setAutoAssignInputs(false)
        model.applyZoomForTesting(lawRoster(["B"], revision: 3))
        lines = []; model.setAutoAssignInputs(true)
        expectEqual(lines, ["slot-write: slot1 kind 'unassigned'->'zoom' by=auto-assign-toggle",
                            "slot-write: slot1 sourceId ''->'B' by=auto-assign-toggle",
                            "slot-write: slot1 inShow 'false'->'true' by=auto-assign-toggle"], "toggle")
        model.assignSlot(2, kind: "capture", sourceId: "cam2", name: "Cam 2")
        lines = []; model.swapMultiviewSlots(1, 2)
        expectEqual(lines, ["slot-write: slot1 kind 'zoom'->'capture' by=drag-swap",
                            "slot-write: slot1 sourceId 'B'->'cam2' by=drag-swap",
                            "slot-write: slot2 kind 'capture'->'zoom' by=drag-swap",
                            "slot-write: slot2 sourceId 'cam2'->'B' by=drag-swap"], "drag-swap")
        lines = []; model.slots[4].inShow = true                                  // nobody wrapped this
        expectEqual(lines, ["slot-write: slot5 inShow 'false'->'true' by=UNTRACKED"],
                    "an unwrapped write is visible as UNTRACKED (D9)")

        var prefs = ShellPrefs()
        prefs.showInputSlots = [PersistedSlot(slotId: 6, kind: "capture", sourceId: "cam6", name: "C6", inShow: true)]
        let restored = AppModel()
        var restoreLines: [String] = []
        restored.slotWriteObserverForTesting = { restoreLines.append($0) }
        restored.applyRestoredPrefs(prefs)
        expectEqual(restoreLines, ["slot-write: slot6 kind 'unassigned'->'capture' by=restore",
                                   "slot-write: slot6 sourceId ''->'cam6' by=restore",
                                   "slot-write: slot6 inShow 'false'->'true' by=restore"], "restore")
    }

    /// Ports OperatorAssignment_PersistsImmediately… (:1017), OperatorUnassign_PersistsImmediately
    /// (:1039) and AutoAssign_PersistsImmediately (:1055). FIXTURE: the disk file is overwritten
    /// with a "stale-file" marker before each action, and the model's joinMeetingId is the fixture
    /// constant "meeting-T2". Setting joinMeetingId has no save side effect (a plain @Published).
    /// So reading "meeting-T2" back proves the ACTION saved. Fails on e98436fc (disk keeps
    /// "stale-file" until the 5 s autosave).
    @MainActor
    static func testSlotChangesReachDiskWithoutTheAutosave() {
        let model = AppModel()
        model.applyRestoredPrefs(ShellPrefs())            // the operator's file is restored → saving is allowed
        model.joinMeetingId = "meeting-T2"
        var stale = ShellPrefs(); stale.joinMeetingId = "stale-file"

        stale.save(); model.assignSlot(4, kind: "capture", sourceId: "cam4", name: "Cam 4")
        var disk = ShellPrefs.load()
        expectEqual(disk.joinMeetingId, "meeting-T2", "an operator assignment saves now")
        expectEqual(disk.showInputSlots?.first { $0.slotId == 4 }?.sourceId, "cam4", "with the new slot")

        stale.save(); model.unassignSlot(4); disk = ShellPrefs.load()
        expectEqual(disk.joinMeetingId, "meeting-T2", "an unassign saves now")
        expect(disk.showInputSlots?.contains { $0.slotId == 4 } != true, "and the slot is gone on disk")

        stale.save(); model.applyZoomForTesting(lawRoster(["A"], revision: 1)); disk = ShellPrefs.load()
        expect(disk.showInputSlots?.contains { $0.kind == "zoom" } != true,
               "an auto-assigned guest is never written to disk (D2)")   // Task 3 amendment

        stale.save(); model.setAutoAssignInputs(false); disk = ShellPrefs.load()
        expectEqual(disk.autoAssignInputs, false, "the toggle saves now")
        expectEqual(disk.joinMeetingId, "meeting-T2", "the whole file")
    }

    /// Port of SecondLoad_IsRefused_AndNeverOverwritesLiveOperatorChanges (:1074). FIXTURE: the
    /// second file targets the slot the operator EMPTIED (restoreSlotAssignments only fills
    /// unassigned slots, so targeting a still-assigned slot would pass vacuously). Fails on e98436fc.
    @MainActor
    static func testASecondRestoreIsRefused() {
        var first = ShellPrefs(); first.joinMeetingId = "first"
        first.showInputSlots = [PersistedSlot(slotId: 2, kind: "capture", sourceId: "camA", name: "A", inShow: true)]
        let model = AppModel()
        model.applyRestoredPrefs(first)
        model.unassignSlot(2)
        var second = ShellPrefs(); second.joinMeetingId = "second"
        second.showInputSlots = [PersistedSlot(slotId: 2, kind: "capture", sourceId: "camC", name: "C", inShow: true)]
        model.applyRestoredPrefs(second)
        expectEqual(model.slots[1].kind, "unassigned", "a stale file never refills the slot the operator emptied")
        expectEqual(model.joinMeetingId, "first", "and overwrites nothing else")
    }

    // ── Task 3: Zoom slots are meeting-scoped; the toggle persists ──────────

    @MainActor
    static func testZoomSlotsAreNeverRestoredOrSaved() {
        var legacy = ShellPrefs()                  // a file written before D2
        legacy.showInputSlots = [PersistedSlot(slotId: 1, kind: "zoom", sourceId: "42", name: "Old guest", inShow: true),
                                 PersistedSlot(slotId: 3, kind: "capture", sourceId: "cam1", name: "Cam", inShow: true)]
        legacy.slotDropoutPolicies = [PersistedSlotPolicy(slotId: 1, dropoutPolicy: "black", sourceKey: "zoom:42")]
        let model = AppModel()
        model.applyRestoredPrefs(legacy)
        expectEqual(model.slots[0].kind, "unassigned", "a saved Zoom slot is not restored")
        expectEqual(model.slots[2].sourceId, "cam1", "a capture slot is")
        expect(model.slots.allSatisfy { $0.dropoutPolicy.isEmpty }, "the old guest's policy attaches to nobody")
        model.assignSlot(1, kind: "zoom", sourceId: "77", name: "Live guest")
        expectEqual(model.currentPrefs().showInputSlots?.map(\.kind), ["capture"], "a live Zoom slot is not saved")
    }

    @MainActor
    static func testTheAutoAssignToggleIsPersistedAndDefaultsOn() {
        let fresh = AppModel()
        fresh.applyRestoredPrefs((try? JSONDecoder().decode(ShellPrefs.self, from: Data("{}".utf8))) ?? ShellPrefs())
        expect(fresh.autoAssignInputsEnabled, "a file without the key keeps the Windows default: on")
        fresh.setAutoAssignInputs(false)
        guard let after = relaunched(fresh) else { expect(false, "relaunch"); return }
        expect(!after.autoAssignInputsEnabled, "off survives a relaunch")
        after.resetAutomationDefaults()
        expect(after.autoAssignInputsEnabled, "Reset defaults turns it back on (MagicSceneCoordinator.cs:458)")
        expect(ShellPrefs.roundTripSelfCheck() == nil, "autoAssignInputs survives encode→decode")
    }

    // ── Task 4: SourceRowFactsProjection ────────────────────────────────────

    private static func zref(_ slot: Int = 1, _ pid: String = "42", inShow: Bool = true) -> SourceSlotRef {
        SourceSlotRef(slotId: slot, kind: "zoom", sourceId: pid, inShow: inShow)
    }
    private static func cref(_ slot: Int = 2, _ id: String = "cam1") -> SourceSlotRef {
        SourceSlotRef(slotId: slot, kind: "capture", sourceId: id, inShow: true)
    }
    private static func zp(_ id: String = "42", video: Bool = true, talking: Bool = false, gen: Int64 = 1) -> ZoomParticipantFact {
        ZoomParticipantFact(id: id, videoOn: video, talking: talking, sourceGeneration: gen)
    }
    private static func zr(_ ps: [ZoomParticipantFact], epoch: String = "1:1:t", state: String = "in_meeting") -> ZoomRosterFact {
        ZoomRosterFact(epoch: epoch, meetingState: state, participants: ps)
    }
    private static func zf(_ w: Int, _ h: Int, fps: Int = 30, at: Double, gen: Int64 = 1,
                           epoch: String = "1:1:t", pid: String = "42") -> ZoomFormatFact {
        ZoomFormatFact(participantId: pid, rosterEpoch: epoch, sourceGeneration: gen,
                       width: w, height: h, fps: fps, frameId: Int(at), frameAtMs: at)
    }
    private static func bus(_ id: String, w: Int = 0, h: Int = 0, frames: Double = 0, age: Double?) -> BusSourceRow {
        BusSourceRow(sourceId: id, kind: id.hasPrefix("capture:") ? "capture" : "zoom", width: w, height: h,
                     framesIngested: frames, sinceLastNewFrameMs: age, liveness: nil, onAir: nil, lossReason: nil,
                     failInMs: nil, lastSampleAgeMs: nil, dropoutPolicy: "hold", dropoutPolicyApplies: nil)
    }

    /// MultiviewInputRowsProjection.cs:362-486, state by state. Windows proof:
    /// ShowInputsCoordinatorTests.DeliveredFormatAndAgePatch… (:314), VersionedBridgeVideoOff… (:203).
    static func testZoomRowStateTable() {
        var p = SourceRowFactsProjection()
        p.setSlots([zref()])
        expectEqual(p.facts(slotId: 1).signal, "IDLE", "no roster fact yet")
        p.applyRoster(zr([], state: "idle"))
        expectEqual(p.facts(slotId: 1).signal, "NO INCOMING", "not in a meeting")
        p.applyRoster(zr([]))
        expectEqual(p.facts(slotId: 1).signal, "NO INCOMING", "guest absent")
        p.applyRoster(zr([zp(video: false)]))
        expectEqual(p.facts(slotId: 1).signal, "VIDEO OFF", "camera off")
        p.applyRoster(zr([zp()]))
        expectEqual(p.facts(slotId: 1).signal, "CONNECTING", "no delivered frame yet")
        expectEqual(p.facts(slotId: 1).format, "—", "and no format")
        p.applyFormat(zf(1920, 1080, at: 100))
        expectEqual(p.facts(slotId: 1), SourceRowFacts(signal: "LIVE", format: "1920×1080", fps: "30"), "a delivered frame")
        p.applyBus(["42": bus("42", age: 1500)], captures: [:], atMs: 1000)
        expectEqual(p.facts(slotId: 1).signal, "LIVE", "exactly 1500 ms is not stale (> 1500)")
        p.applyBus(["42": bus("42", age: 1501)], captures: [:], atMs: 1500)
        let stalled = p.facts(slotId: 1)
        expectEqual(stalled.signal, "STALLED", "a frame older than 1500 ms")
        expectEqual(stalled.fps, "—", "a stale rate is never shown as live FPS")
        expectEqual(stalled.format, "1920×1080", "the last format stays, marked stale")
        expect(stalled.formatStale && stalled.frameAgeMs == 1501, "with its age")
        p.applyRoster(zr([zp(talking: true)]))
        expectEqual(p.facts(slotId: 1).signal, "STALLED", "stale outranks talking")
        p.applyBus(["42": bus("42", age: 10)], captures: [:], atMs: 2000)
        expectEqual(p.facts(slotId: 1).signal, "TALKING", "fresh and talking")
        p.engineStopped()
        expectEqual(p.facts(slotId: 1), SourceRowFacts(), "engine off: IDLE, nothing kept")
        var parked = SourceRowFactsProjection()
        parked.setSlots([zref(inShow: false)]); parked.applyRoster(zr([zp()])); parked.applyFormat(zf(1280, 720, at: 1))
        expectEqual(parked.facts(slotId: 1).signal, "IDLE", "assigned but out of the show")
    }

    /// Spec correction 5. Windows proof: CaptureSignalFactPatchesHostUvcFormatWithoutRowClick (:704).
    /// FIXTURE: the bus says 1280×720 and the device row would say 1920×1080 @ 30 if it were read.
    /// The two never agree, so reading the wrong field is visible.
    static func testCaptureRowReadsObservationsNeverRequests() {
        var p = SourceRowFactsProjection()
        p.setSlots([cref()])
        p.applyBus([:], captures: [:], atMs: 0)
        expectEqual(p.facts(slotId: 2).signal, "NO INCOMING", "no such device")
        p.applyBus([:], captures: ["cam1": CaptureSignal(connectionState: "detected", signalPresent: false)], atMs: 0)
        expectEqual(p.facts(slotId: 2).signal, "CONNECTING", "detected, not connected")
        let live = ["cam1": CaptureSignal(connectionState: "connected", signalPresent: true)]
        p.applyBus([:], captures: live, atMs: 0)
        expectEqual(p.facts(slotId: 2), SourceRowFacts(signal: "LIVE", format: "—", fps: "—"),
                    "live with no bus row: no format, and never the device's default 1920×1080@30")
        p.applyBus(["capture:cam1": bus("capture:cam1", w: 1280, h: 720, frames: 0, age: 10)], captures: live, atMs: 0)
        expectEqual(p.facts(slotId: 2).format, "1280×720", "format is the delivered frame size")
        expectEqual(p.facts(slotId: 2).fps, "—", "one sample is not a rate")
        p.applyBus(["capture:cam1": bus("capture:cam1", w: 1280, h: 720, frames: 15, age: 10)], captures: live, atMs: 500)
        expectEqual(p.facts(slotId: 2).fps, "—", "500 ms is under the 900 ms window")
        p.applyBus(["capture:cam1": bus("capture:cam1", w: 1280, h: 720, frames: 30, age: 10)], captures: live, atMs: 1000)
        expectEqual(p.facts(slotId: 2).fps, "30", "30 frames over 1000 ms")
        p.applyBus(["capture:cam1": bus("capture:cam1", w: 1280, h: 720, frames: 30, age: 1600)], captures: live, atMs: 2000)
        expectEqual(p.facts(slotId: 2).fps, "—", "a stale capture shows no rate")
    }

    /// Port of ReusedZoomIdClearsOldFormatAndRejectsOldSourceFactWithoutTake (:133).
    static func testReusedZoomIdClearsOldFormatAndRejectsOldFacts() {
        var p = SourceRowFactsProjection()
        p.setSlots([zref()]); p.applyRoster(zr([zp(gen: 1)]))
        p.applyFormat(zf(1920, 1080, at: 100, gen: 1))
        expectEqual(p.facts(slotId: 1).format, "1920×1080", "first person's format")
        p.applyRoster(zr([zp(gen: 2)]))                                // same id, a different source
        expectEqual(p.facts(slotId: 1).signal, "CONNECTING", "the new person has no frame yet")
        expectEqual(p.facts(slotId: 1).format, "—", "the old format is cleared")
        p.applyFormat(zf(1920, 1080, at: 200, gen: 1))
        expectEqual(p.facts(slotId: 1).format, "—", "a late fact from the old source is rejected")
        p.applyFormat(zf(1280, 720, at: 201, gen: 2))
        expectEqual(p.facts(slotId: 1).format, "1280×720", "the new source's fact applies")
    }

    static func testANewEpochAndAnOlderFrameAreRejected() {
        var p = SourceRowFactsProjection()
        p.setSlots([zref()]); p.applyRoster(zr([zp()]))
        p.applyFormat(zf(1920, 1080, at: 100))
        p.applyFormat(zf(640, 360, at: 90))
        expectEqual(p.facts(slotId: 1).format, "1920×1080", "an older frame cannot repaint the row")
        p.applyRoster(zr([zp()], epoch: "1:2:t"))
        expectEqual(p.facts(slotId: 1).format, "—", "a new meeting epoch clears every Zoom format")
        p.applyFormat(zf(1920, 1080, at: 300, epoch: "1:1:t"))
        expectEqual(p.facts(slotId: 1).format, "—", "a fact from the retired epoch is rejected")
        p.applyFormat(zf(1280, 720, at: 301, epoch: "1:2:t"))
        expectEqual(p.facts(slotId: 1).format, "1280×720", "the current epoch's fact applies")
    }

    /// Rule 3: quantifies over EVERY combination of on-program, on-preview, talking, stale and
    /// camera-on (32). The oracle is written here from MultiviewTileCropPolicy.cs:31-37 and
    /// MultiviewInputRowsProjection.cs:298, independently of the implementation.
    /// Windows proof: PreviewTallyHonorsProgramPreviewTalkingAndStall (:664),
    /// ProgramTallyChangesStatusChipWithoutReplacingSourceHealth (:694).
    static func testTallyPrecedenceOverEveryInputCombination() {
        for bits in 0..<32 {
            let onProgram = bits & 1 != 0, onPreview = bits & 2 != 0, talking = bits & 4 != 0
            let stale = bits & 8 != 0, videoOn = bits & 16 != 0
            let oracle = !videoOn ? "none" : stale ? "hold" : onProgram ? "pgm" : onPreview ? "pvw"
                : talking ? "talking" : "none"
            var p = SourceRowFactsProjection()
            p.setSlots([zref()]); p.applyRoster(zr([zp(video: videoOn, talking: talking)]))
            p.applyFormat(zf(1920, 1080, at: 100))
            p.applyBus(["42": bus("42", age: stale ? 2000 : 10)], captures: [:], atMs: 0)
            p.applyTally(program: onProgram ? ["zoom:42"] : [], preview: onPreview ? ["zoom:42"] : [])
            let facts = p.facts(slotId: 1)
            expectEqual(facts.tally, oracle, "tally for bits \(bits)")
            let oracleSignal = !videoOn ? "VIDEO OFF" : stale ? "STALLED" : talking ? "TALKING" : "LIVE"
            expectEqual(facts.signal, oracleSignal, "a tally never replaces source health (bits \(bits))")
            expectEqual(facts.chip, oracleSignal == "LIVE" && oracle == "pgm" ? "PGM" : oracleSignal,
                        "PGM only for a LIVE row on Program (bits \(bits))")
        }
    }

    /// Port of IsoChipUsesObservedWriterLifecycle (:407-432) plus the Mac "unreported" rule.
    static func testIsoChipUsesObservedWriterLifecycle() {
        func recording(_ state: String?, _ health: String?, stream: String,
                       active: Bool = true, status: String = "recording") -> JSONObject {
            var r: JSONObject = ["sessionId": "s", "status": status, "active": active,
                                 "streams": [["kind": "iso", "sourceId": "zoom:guest", "status": stream]]]
            if let state { r["lifecycle"] = ["state": state, "health": health ?? "healthy"] }
            return r
        }
        let rows: [(String?, String?, String, String?)] = [
            ("preparing", "healthy", "writing", "armed"),
            ("producing", "healthy", "writing", "recording"),
            ("producing", "healthy", "warning", "error"),
            ("interrupted", "failed", "writing", "error"),
            ("completed", "healthy", "writing", nil),            // off is omitted
        ]
        for (state, health, stream, expected) in rows {
            expectEqual(IsoWriterChip.observe(recording(state, health, stream: stream))["zoom:guest"], expected,
                        "\(state ?? "-")/\(health ?? "-")/\(stream)")
        }
        expectEqual(IsoWriterChip.observe(recording(nil, nil, stream: "writing"))["zoom:guest"], "recording",
                    "no lifecycle: an active recording reads as recording")
        let unreported: JSONObject = ["status": "recording", "active": true,
                                      "streams": [["kind": "iso", "participantId": "zoom:guest", "status": "unreported"]]]
        expectEqual(IsoWriterChip.observe(unreported)["zoom:guest"], "unknown", "unreported is unknown, never off")
        var p = SourceRowFactsProjection(); p.setSlots([zref(1, "guest")])
        p.applyWriters(["zoom:guest": "unknown"])
        expectEqual(p.facts(slotId: 1).writer, "—", "the row shows — for an unreported writer")
        p.applyWriters([:])
        expectEqual(p.facts(slotId: 1).writer, "OFF", "no writer node: OFF")
    }

    /// Windows MultiviewInputRowsProjection.cs:334-344: reassignment clears before another identity shows.
    static func testReassigningASlotResetsItsRowBeforeTheNewSourceShows() {
        var p = SourceRowFactsProjection()
        p.setSlots([zref(1, "42")]); p.applyRoster(zr([zp("42"), zp("43")]))
        p.applyFormat(zf(1920, 1080, at: 100)); p.applyWriters(["zoom:42": "recording", "zoom:43": "armed"])
        expectEqual(p.facts(slotId: 1).writer, "REC", "precondition")
        p.setSlots([zref(1, "43")])
        let row = p.facts(slotId: 1)
        expectEqual(row.format, "—", "the old guest's format is gone")
        expectEqual(row.signal, "CONNECTING", "the new guest has not delivered")
        expectEqual(row.writer, "ARMED", "the writer chip is the new guest's")
    }

    // ── Task 5: facts wired from the core ───────────────────────────────────


    @MainActor
    static func testFormatEventsPatchOneRowWithoutASync() {
        let model = AppModel()
        model.assignSlot(1, kind: "zoom", sourceId: "42", name: "Guest 42")
        model.applyZoomForTesting(lawRoster(["42"], revision: 1))           // sourceGeneration 1, epoch 1:1:law
        let spine = model.spineSyncRequests, scenes = model.sceneSyncRequestCount
        var otherRowChanges = 0
        let sink = model.sourceFacts.box(2).objectWillChange.sink { _ in otherRowChanges += 1 }
        model.handleEventForTesting(["type": "zoom-source-format", "format": [
            "participantId": "42", "rosterEpoch": "1:1:law", "sourceGeneration": NSNumber(value: 1),
            "width": 1280, "height": 720, "fps": 30, "frameId": 5, "frameAtMs": 500.0]])
        let facts = model.sourceFacts.box(1).facts
        expectEqual(facts.format, "1280×720", "the event patches the row")
        expectEqual(facts.signal, "LIVE", "and makes it live")
        expectEqual(model.spineSyncRequests, spine, "no spine push")
        expectEqual(model.sceneSyncRequestCount, scenes, "no scene sync")
        expectEqual(otherRowChanges, 0, "another row is not invalidated")
        sink.cancel()
    }

    @MainActor
    static func testCaptureRowIsWiredFromTheBus() {
        let model = AppModel()
        var now = 0.0
        model.sourceFactsClockForTesting = { now }
        model.assignSlot(2, kind: "capture", sourceId: "cam1", name: "Cam")
        func snapshot(frames: Double) -> JSONObject {
            ["captureDevices": [["id": "cam1", "name": "Cam", "vendor": "avfoundation", "connectionState": "connected",
                                 "signalPresent": true, "resolution": ["width": 1920, "height": 1080], "frameRate": 30]],
             "sources": [["sourceId": "capture:cam1", "kind": "capture", "width": 1280, "height": 720,
                          "framesIngested": frames, "sinceLastNewFrameMs": 10.0, "health": "producing"]]]
        }
        model.applySnapshotForTesting(snapshot(frames: 0))
        expectEqual(model.sourceFacts.box(2).facts.format, "1280×720", "the bus frame size, not the device's 1920×1080")
        now = 1000
        model.applySnapshotForTesting(snapshot(frames: 25))
        expectEqual(model.sourceFacts.box(2).facts.fps, "25", "a measured rate (25 frames / 1 s), not the device's 30")
    }

    @MainActor
    static func testTallyAndWriterAreWiredFromTheSnapshot() {
        let model = AppModel()
        model.assignSlot(1, kind: "zoom", sourceId: "42", name: "Guest 42")
        model.applyZoomForTesting(lawRoster(["42"], revision: 1))
        model.handleEventForTesting(["type": "zoom-source-format", "format": [
            "participantId": "42", "rosterEpoch": "1:1:law", "sourceGeneration": NSNumber(value: 1),
            "width": 1920, "height": 1080, "fps": 30, "frameId": 1, "frameAtMs": 100.0]])
        model.applySnapshotForTesting([
            "programFrame": ["frameNumber": 7, "videoSources": [["sourceId": "zoom:42", "kind": "participant-video",
                                                                 "participantId": "42"]]],
            "recording": ["sessionId": "s", "status": "recording", "active": true,
                          "lifecycle": ["state": "producing", "health": "healthy"],
                          "streams": [["kind": "iso", "sourceId": "zoom:42", "status": "writing"]]]])
        let facts = model.sourceFacts.box(1).facts
        expectEqual(facts.chip, "PGM", "a live source on rendered Program reads PGM")
        expectEqual(facts.writer, "REC", "the writer chip is the observed writer")
    }

    @MainActor
    static func testAStalePickerChoiceIsRefused() {
        let model = AppModel()
        model.assignSlot(1, kind: "zoom", sourceId: "42", name: "Guest 42")     // the menu opens for zoom:42
        model.assignSlot(1, kind: "capture", sourceId: "camA", name: "A")       // meanwhile slot 1 changes
        model.assignSlot(1, kind: "capture", sourceId: "camB", name: "B", openedFor: "zoom:42")
        expectEqual(model.slots[0].sourceId, "camA", "a choice made against an older assignment is refused")
        expectEqual(model.sourcesStatus, "Input 1 changed while its menu was open. Choose again.", "and says so")
        model.assignSlot(1, kind: "capture", sourceId: "camB", name: "B", openedFor: "capture:camA")
        expectEqual(model.slots[0].sourceId, "camB", "a current choice applies")
    }

    // ── Task 6: types, picker, columns, workspace state ─────────────────────

    private static func close(_ a: [CGFloat], _ b: [CGFloat], _ what: String) {
        expect(a.count == b.count && zip(a, b).allSatisfy { abs($0 - $1) < 0.001 }, "\(what): \(a) vs \(b)")
    }

    /// Expected arrays computed by hand from the Windows formula (SourcesTableRowControl.cs:115-119).
    static func testColumnWidthsMatchTheWindowsBreakpoints() {
        let all = SourcesColumnPrefs()
        func w(_ width: CGFloat, _ p: SourcesColumnPrefs = SourcesColumnPrefs()) -> [CGFloat] {
            SourcesColumnLayout.widths(tableWidth: width, prefs: p)
        }
        close(w(700), [26, -1, 0, 91, 42, 0, 0, 36, 60, 140], "700")
        close(w(749), [26, -1, 0, 97.37, 42, 0, 0, 36, 60, 140], "749: Signal still hidden")
        close(w(750), [26, -1, 0, 100, 42, 86, 0, 36, 60, 140], "750: Signal appears, Format floor 100")
        close(w(900), [26, -1, 0, 117, 45, 86, 0, 36, 60, 140], "900")
        close(w(1049), [26, -1, 0, 136.37, 52.45, 94.41, 0, 41.96, 62.94, 140], "1049: Type and Audio still hidden")
        close(w(1050), [26, -1, 84, 136.5, 52.5, 94.5, 126, 42, 63, 140], "1050: Type and Audio appear")
        close(w(1100), [26, -1, 88, 143, 55, 99, 132, 44, 66, 140], "1100")
        close(w(1600), [26, -1, 128, 208, 80, 144, 192, 64, 96, 140], "1600")
        var hidden = all; hidden.showType = false; hidden.showSignal = false; hidden.showAudio = false
        close(w(1600, hidden), [26, -1, 0, 208, 80, 0, 0, 64, 96, 140], "Columns menu hides Type, Signal, Audio")
        expect(!SourcesColumnLayout.showsThumbnail(tableWidth: 749) && SourcesColumnLayout.showsThumbnail(tableWidth: 750),
               "the thumbnail shows from 750")
        expect(!SourcesColumnLayout.inspectorBeside(pageWidth: 1499) && SourcesColumnLayout.inspectorBeside(pageWidth: 1500),
               "the inspector sits beside the table from 1500")
        expectEqual(SourcesColumnLayout.headers, ["#", "Source", "Type", "Format", "FPS", "Signal", "Audio", "MV", "ISO", "Actions"],
                    "the Windows headers (SourcesWorkspaceControl.cs:86)")
    }

    static func testSourceTypesLabelsAndCategories() {
        let cases: [(String, String, String?, SourceType, String, String)] = [
            ("zoom", "42", nil, .zoomGuest, "Zoom", "Zoom · "),
            ("capture", "0x1234", "avfoundation", .camera, "Cameras", "Camera · "),
            ("capture", "dl-1", "Blackmagic", .captureCard, "Cameras", "Capture card · "),
            ("capture", "aja-1", "aja", .captureCard, "Cameras", "Capture card · "),
            ("capture", "screen:0", "ScreenCaptureKit", .screen, "Screens", "Screen · "),
            ("capture", "window:991", "ScreenCaptureKit", .window, "Screens", "Window · "),
            ("unassigned", "", nil, .unassigned, "Unassigned", ""),
        ]
        for (kind, id, vendor, type, category, prefix) in cases {
            let derived = SourceType.of(kind: kind, sourceId: id, vendor: vendor)
            expectEqual(derived, type, "\(kind)/\(id)/\(vendor ?? "-")")
            expectEqual(derived.category, category, "category of \(type.rawValue)")
            expectEqual(derived.pickerPrefix, prefix, "a screen is never labelled Camera (spec §3.3)")
        }
    }

    static func testPickerGroupsInWindowsOrderWithTheSavedUnavailableEntry() {
        let roster = [RosterParticipant(id: "42", name: "Ana", hasVideo: true, muted: false, talking: false, assigned: false)]
        let devices = [
            CaptureDeviceRow(id: "screen:0", name: "Display 1", kind: "video", vendor: "ScreenCaptureKit",
                             connectionState: "detected", signalPresent: false, warning: ""),
            CaptureDeviceRow(id: "cam1", name: "FaceTime", kind: "video", vendor: "avfoundation",
                             connectionState: "connected", signalPresent: true, warning: ""),
        ]
        let groups = SourcePickerCatalog.groups(roster: roster, devices: devices, current: nil)
        expectEqual(groups.map(\.title), ["Zoom", "Camera", "Screen"], "Windows order")
        expectEqual(groups[1].options.map(\.label), ["Camera · FaceTime"], "cameras")
        expectEqual(groups[2].options.map(\.label), ["Screen · Display 1"], "screens are labelled Screen")
        let noScreens = SourcePickerCatalog.groups(roster: [], devices: [devices[1]], current: nil)
        expectEqual(noScreens.map(\.title), ["Camera"], "empty groups are not drawn")
        var gone = ShowInputSlot(id: 3); gone.kind = "capture"; gone.sourceId = "cam9"; gone.name = "Old cam"; gone.offline = true
        let withGone = SourcePickerCatalog.groups(roster: roster, devices: devices, current: gone)
        expectEqual(withGone.first?.options.first?.label, "Saved source unavailable — Old cam",
                    "a missing saved source is named, never substituted")
        expect(withGone.first?.options.first?.unavailable == true, "and marked unavailable")
    }

    /// Port of SourcesWorkspaceTests.InspectionFiltersAndColumnsNeverChangeAssignmentsOrLiveSelections (:12).
    @MainActor
    static func testInspectionFiltersAndColumnsNeverChangeAssignmentsOrLiveSelections() {
        let model = AppModel()
        model.assignSlot(2, kind: "zoom", sourceId: "alice", name: "Alice")
        model.toggleSlotIso(2)
        let slotsBefore = model.slots, isoBefore = model.isoSelectedSourceIds
        var writes: [String] = []
        model.slotWriteObserverForTesting = { writes.append($0) }
        var state = SourcesWorkspaceState()
        state.inspect(2); state.query = "not alice"; state.category = "Cameras"
        state.columns.showAudio = false; state.columns.inspectorOpen = false
        expect(!state.matches(slotId: 2, displayName: "Alice", sourceLabel: "Zoom · Alice", type: .zoomGuest), "filtered out")
        expectEqual(state.selectedSlotId, 2, "a filter never moves the selection")
        expectEqual(model.slots, slotsBefore, "no slot changed")
        expectEqual(model.isoSelectedSourceIds, isoBefore, "no ISO changed")
        expect(writes.isEmpty, "no slot write at all")
        state.query = "02"; state.category = "Zoom"
        expect(state.matches(slotId: 2, displayName: "Alice", sourceLabel: "Zoom · Alice", type: .zoomGuest),
               "the zero-padded slot number matches")
    }

    static func testColumnPreferencesRoundTripAndMissingFieldsDefaultVisible() {
        var value = SourcesColumnPrefs(); value.showAudio = false; value.showType = false
        let data = try? JSONEncoder().encode(value)
        expectEqual(data.flatMap { try? JSONDecoder().decode(SourcesColumnPrefs.self, from: $0) }, value, "round trip")
        let empty = try? JSONDecoder().decode(SourcesColumnPrefs.self, from: Data("{}".utf8))
        expect(empty?.showSignal == true && empty?.showType == true, "missing fields default visible")
        expect(ShellPrefs.roundTripSelfCheck() == nil, "sourcesWorkspace survives encode→decode")
    }

    static func testSummaryUsesTheWindowsCopy() {
        var slots = (1...10).map { ShowInputSlot(id: $0) }
        slots[0].kind = "zoom"; slots[0].sourceId = "a"; slots[0].inShow = true; slots[0].iso = true
        slots[1].kind = "zoom"; slots[1].sourceId = "b"; slots[1].inShow = false
        slots[2].kind = "capture"; slots[2].sourceId = "c"; slots[2].inShow = true
        expectEqual(SourcesSummary.text(slots: slots, matching: 2),
                    "3/10 assigned · 2 in MV · 1 ISO selected · 2 match filter", "the Windows summary line")
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
