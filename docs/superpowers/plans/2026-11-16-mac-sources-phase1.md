# macOS Sources Rebuild, Phase 1 (S1, S2, S3, S5) Implementation Plan

Status: DRAFT for owner review, 2026-11-16.

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** On a Mac, sources appear in slots only by operator action or newcomer auto-assign (the slot law), and every slot change is on disk at once. Each Sources row shows the signal facts the core actually observed. The Sources tab becomes the Windows ten-row table with filters, a capture section and labelled types. An inspector holds per-source names, lower-third text, dropout, role, the Mac-only chroma key, and a reserved Grade section.

**Spec:** `docs/superpowers/specs/2026-11-15-mac-sources-rebuild-design.md` (slices S1, S2, S3, S5). Coordinated with `docs/superpowers/specs/2026-11-15-mac-source-grading-scopes-design.md` §5.8 / slice G4, and with `docs/superpowers/specs/2026-11-15-mac-remote-control-parity-design.md` (RC2 `input.*` actions and `automation.autoAssignInputs.set`).

**Architecture:** Shell only. No `native/` file changes in this phase. Four new pure Swift types carry the decisions: `ShowInputAssignmentLaw` (the slot law), `SourceRowFactsProjection` (observed row facts, the Swift twin of Windows `MultiviewInputRowsProjection`), `SourcesColumnLayout` / `SourcesWorkspaceState` (the table), and the inspector stores (`SourceLowerThirdText`, `ProductionRoles`, `CaptureDropoutStatus`). Each is a value type, driven in tests through the real `AppModel` entry points (`applyZoomForTesting`, `applySnapshotForTesting`, a new `handleEventForTesting`). Views (`SourcesTable.swift`, `SourceInspector.swift`) read them. Slot writes go through one choke point on `AppModel.slots` (a `didSet` diff, the Windows setter shape), which logs every assignment write with its reason and saves the prefs file at once.

**Tech stack:** Swift 5.9 / SwiftUI (macOS 13), the in-binary shell harness (`COREVIDEO_SHELL_TESTS=1`), `COREVIDEO_SHELL_TAB_SNAPSHOT` and `COREVIDEO_SHELL_SNAPSHOT` for layout proofs.

## Global constraints

- Branch: work on a new branch cut from `mac-tier0b` (for example `mac-sources-phase1`) in its own worktree. Commit per task; never push without the owner; never touch `main`. End commit messages with `Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>`.
- Paths: Mac shell paths are relative to `mac-shell/Sources/CoreVideoProShell/` unless written in full; Windows paths are relative to `native-shell/CoreVideoPro.WinUI/` (tests: `native-shell/CoreVideoPro.WinUI.Tests/`). Mac line numbers are from `mac-tier0b` at `e98436fc` and were re-read for this plan.
- Build and test: `cd mac-shell && swift build -c release && COREVIDEO_SHELL_TESTS=1 ./.build/release/CoreVideoProShell`. `bash scripts/lint-mac-design.sh` must stay green (fonts are `.grotesk()` / `.plexMono()`, colours are `Studio.*`).
- New tests go in a NEW file `ShellTestsSources.swift` (`extension ShellTests { @MainActor static var sourcesCases: [(String, () -> Void)] }`), registered in `ShellTests.runAndExit` as `] + tier0Cases + vcamUpdateCases + sourcesCases` (`ShellTests.swift:4082`). Existing tests that this plan changes are named in the task that changes them.
- `ShellPrefs` additions are OPTIONAL properties, decoded field by field in `init(from:)` (`ShellPrefs.swift:184-240`), and added to `roundTripSelfCheck` (`ShellPrefs.swift:243`). A non-optional field or a field missing from `init(from:)` silently resets the operator's whole prefs file (the comment at `ShellPrefs.swift:170-180`).
- `@Published` writes on `AppModel` invalidate EVERY view that observes `AppModel` (it is one `ObservableObject`). Anything updated at frame-fact rate (up to 5 per second per guest) must live in its own `ObservableObject` per slot (Task 5), never in an `AppModel` `@Published` property.
- Copy that matches Windows is verbatim, quoted in each task.
- Rules in `docs/superpowers/plan-authoring-rules.md` are binding. Every task reports each named mutation and whether it went red. A mutation that stays green means the test is broken. Fix the test, not the mutation list.
- Backlog first (master plan §7): Task 1 Step 0 checks for a `backlog` tracking issue for "Sources rebuild (mac, rows 2.4/2.6)". If none exists, ask the owner to approve filing it. Coding can proceed locally meanwhile; reference the issue in every commit body once it exists.

## Defaults taken (owner may override)

The spec's open owner decisions, with the spec's recommendation taken, plus the plan-level choices this plan had to make:

| # | Default | Source | Tasks that change if overridden |
|---|---|---|---|
| D1 | Thumbnails come from the core "source sheet" (slice S6, Phase 2). In Phase 1 the thumbnail column shows the placeholder "Preview unavailable" (spec §4.9), and hides under 750 pt as on Windows. | Spec decision 1 | Task 7 (drop the column until S6) |
| D2 | Zoom slots are not restored across launches and not written to the prefs file. A Zoom slot is freed when its guest leaves an accepted roster. | Spec decision 2 | Tasks 1, 3 |
| D3 | Auto-assign places camera-off newcomers too (Windows passes the whole roster). | Spec decision 3 | Task 1 (filter `hasVideo` again) |
| D4 | The Role control is built, stored and sent, but hidden behind `AppModel.roleControlAvailable == false` until role-targeted routes are ported. | Spec decision 4 | Tasks 9, 10 |
| D5 | Name, microphone, Key and dropout leave the row. In S3 they sit in the row's "…" menu (so S3 can ship alone). In S5 they move into the inspector, and the row keeps a "KEY" chip. | Spec decision 5 | Tasks 7, 10 |
| D6 | The CoreVideo participant itself (`isMe: true`) is never auto-assigned. **This departs from Windows**, which does not filter `isMe` at `CoreVideoPro.MediaCore/Services/LiveProductionSync.cs:128-168`. The engine does include itself in the roster (`native/zoom-engine/engine/main-macos.mm:507`). Under D3 the camera-off bot would otherwise take a slot. Owner: confirm, or say whether Windows places itself. | Plan | Task 1 |
| D7 | Flipping the auto-assign toggle EITHER way clears both memories, then runs the roster pass (fills only when on). Windows reapplies on any change (`ViewModels/MagicScene/MagicSceneCoordinator.cs:509-514`). The spec text says "flipping it on". | Plan, per Windows | Task 1 |
| D8 | "Use source text" is an Overlays-tab toggle, default off. When on, a keyed source's saved secondary line replaces the typed title (spec §4.7). | Spec §4.7 | Task 8 |
| D9 | A slot write outside any reason scope is LOGGED as `by=UNTRACKED`, never asserted. Windows does the same (`Models/ShowInputModels.cs:40-52`). The harness runs a release build, so a debug-only assert would never fire in a test. | Plan, per Windows | Task 2 |
| D10 | A departed guest's slot is reset whole, as Unassign already does (`AppModel.swift:1832`). Their per-slot key, mic and dropout go with it. Windows keeps dropout per source, so a guest who drops and rejoins keeps it there. On a Mac this is a small regression, recorded under Risks. | Plan | Task 1 |

## Spec corrections (the plan follows the code)

1. **Line drift.** The auto-assign fill loop is `AppModel.swift:1752-1769` (the spec says `:1756-1767`). `firstEmptySlotIndex` is `:1879-1881`. The spine `"role": "guest"` is at `:2302`.
2. **The Windows law lines.** `ShowInputsCoordinator.cs`: the seen set is at `:366-374`, reserved slots at `:376-386`, the meeting sync at `:388-421`, reapply at `:423-448`, unassign at `:504-521`. Load refusal is at `:144-173` and the synchronous save at `:206-219`.
3. **The roster gate the spec leaves out.** Windows runs the slot pass only when the roster is non-empty or carries `rosterRevision > 0` (`ViewModels/StudioViewModel.cs:11128-11134`). It maps a non-empty roster outside `in_meeting` to an EMPTY roster (`LiveProductionSync.cs:111-123`). Task 1 ports both. Without the gate, an unrevisioned empty poll would free every Zoom slot.
4. **Windows reserves the slot on every Unassign**, whatever its kind (`ShowInputsCoordinator.cs:517`), not only for Zoom.
5. **Capture Format and FPS cannot come from `captureDevices`.** The AVFoundation adapter never writes `frameRate`, so it is the struct default 30 (`native/src/modules/Interfaces.h:1150`). `width` and `height` read the default 1920×1080 until the first frame (`AvfCaptureDeviceAdapter.mm:323,341`). ScreenCaptureKit writes a REQUESTED 60 (`SckScreenCaptureAdapter.mm:275,296`). Showing any of these breaks the Windows rule "a request ceiling is never shown as a measurement" (`ViewModels/ShowInputs/MultiviewInputRow.cs:35-38`). Task 4 takes capture Format from `sources[].width/height`, which is the latest frame (`native/src/core/CaptureDeviceSource.h:34-38`). It takes FPS from the measured `sources[].framesIngested` rate.
6. **Zoom `deliveredWidth/Fps/lastFrameAgeMs` are not in any polled snapshot.** They exist only in the `zoom-media-spine-sync` RESPONSE (`native/src/modules/ZoomEngineRuntime.cpp:1480-1515`). The Mac discards that response (`AppModel.swift:2268-2285`) and sends it only on triggers. Task 4 uses `zoom-source-format` events (`ZoomEngineRuntime.cpp:1945-1966`) for Format and FPS. It uses `sources[]` rows for staleness: Zoom rows are keyed by the RAW participant id and carry `sinceLastNewFrameMs` (`native/src/core/SourceVideoIngress.cpp:91-131`).
7. **"Facts live in their own `@Published` dictionary … a fact update redraws one row"** is false with `AppModel: ObservableObject`. Any `@Published` write re-evaluates every observing view. Task 5 uses one `ObservableObject` box per slot instead.
8. **Slot writes are not only in `AppModel`.** SwiftUI bindings write `model.slots[...]` directly (`ShellApp.swift:1661,1692,1706,1790`, `AudioInputs.swift:103`), and so does the drag-swap (`StudioSceneEditing.swift:204-205`). "One function every write goes through" cannot be enforced. Task 2 ports the Windows setter-level choke point instead: a `didSet` diff on `slots` that logs the assignment fields. Windows logs exactly `Kind/ParticipantId/CaptureDeviceId/InShow` (`Models/ShowInputModels.cs:174-185`); on a Mac those are `kind`, `sourceId` and `inShow`.
9. **The Mac sends no ISO display name** (grep: no producer). The resolved source name feeds the multiview label, audio labels, the lower third, the control snapshot `slots[].name`, and `set-source-policy.displayName`.
10. **Windows keeps lower-third text in two per-source stores:** display names and secondary lines (`Services/SourceLowerThirdText.cs`). Task 8 ports that as `sourceDisplayNames` + `sourceSecondaryLines`, not one `sourceLowerThirdText` key.
11. **Four existing Mac tests encode restoring Zoom slots**, which D2 removes. Task 3 names and amends each one. One test encodes the stale "dropout is zoom-only" rule (`ShellTests.swift:1906`); Task 9 amends it. One test measures `SlotRow` (`ShellTestsTier0.swift:386`); Task 7 replaces it.
12. **Refresh.** The core answers `list-capture-devices` with `{type: "capture-devices", devices}` (`native/src/rpc/JsonRpcServer.cpp:243-245`). Nothing on the Mac sends it today.

## Before you start

- [ ] `git -C <worktree> log --oneline -1` reads `e98436fc` or a descendant of it on your branch.
- [ ] Baseline, measured 2026-11-16 on `mac-tier0b@e98436fc`: `swift build -c release` completes. `COREVIDEO_SHELL_TESTS=1 ./.build/release/CoreVideoProShell` prints **`3961 shell checks passed, 0 failed.`** across **236** registered cases (count of `  running` lines on stderr). `bash scripts/lint-mac-design.sh` prints **`Design lint passed.`** Re-measure and record your numbers. If they differ, find out why before Task 1.
- [ ] Read: `docs/reference/live-meeting-qa-2026-08-09.md:78-121` (THE LAW, and "test the WHOLE merge, not just the leaf"); `docs/reference/the-law-slots.md` (all 49 lines); Windows `ViewModels/ShowInputs/ShowInputsCoordinator.cs:140-225,366-521`, `Services/ShowInputRosterService.cs:340-416`, `Models/ShowInputModels.cs:40-190`, `ViewModels/ShowInputs/MultiviewInputRowsProjection.cs:78-125,329-487`.
- [ ] Nothing in this plan changes `native/`. If a task seems to need a core change, stop and raise it. The S2 risk "capture liveness or ISO writer state unreported on Mac" resolves to "show only what is observed, file a native follow-up". It never resolves to "patch the core in this plan".

## Task order and lanes

| Task | Slice | Lane | Depends on | Can run in parallel with |
|---|---|---|---|---|
| 1 Slot law through the whole merge | S1 | shell | none | 4 |
| 2 Slot-write log, immediate save, one restore | S1 | shell | 1 | 4, 5 |
| 3 Zoom slots not restored; toggle persisted and drawn | S1 | shell | 2 | 4, 5 |
| 4 `SourceRowFactsProjection` (pure) | S2 | shell | none | 1, 2, 3 |
| 5 Facts wiring, chips on rows, picker guard | S2 | shell | 4 (and 2 for the scope API) | 3 |
| 6 Types, picker groups, column layout, workspace state | S3 | shell | 5 | none |
| 7 The Sources table | S3 | shell | 3, 6 | none |
| 8 Per-source names and lower-third text | S5 | shell | 7 | 9 |
| 9 Capture dropout, role store, policy commands | S5 | shell | 7 | 8 |
| 10 The inspector | S5 | shell | 8, 9 | none |

All ten tasks are in the shell lane. They touch `AppModel.swift` often, so "parallel" means "logically independent; expect merge conflicts in `AppModel.swift`".

---

### Task 1: The slot law, driven through the whole roster merge (S1)

**Lane:** shell. **Slice:** S1 rules 1-3, 7 and the toggle model.

**Files:**
- Create: `ShowInputAssignmentLaw.swift`
- Create: `ShellTestsSources.swift` (and register `sourcesCases` in `ShellTests.swift:4082`)
- Modify: `AppModel.swift`: `RosterParticipant` (`:288-325`), `applyZoom` (`:1725-1785`), `manuallyUnassigned` (`:1787`, deleted), `assignSlot` / `unassignSlot` / `toggleSlotInShow` (`:1816-1848`), `toggleAssigned` (`:2239-2249`)

**Interfaces:**

```swift
// ShowInputAssignmentLaw.swift
struct ShowInputAssignmentLaw: Equatable {
    static let slotCount = 10
    /// Roster ids seen this meeting; REPLACED by every accepted roster (a leaver is forgotten,
    /// so a real rejoin is a newcomer again). Windows `_autoAssignSeenParticipantIds`.
    private(set) var seenThisMeeting: Set<String> = []
    /// Slot NUMBERS the operator emptied. Windows `_operatorClearedSlotNumbers`.
    private(set) var operatorClearedSlots: Set<Int> = []
    var autoAssignEnabled = true

    /// One accepted roster barrier. `roster` is in roster order and is already EMPTY when the
    /// meeting is not `in_meeting`. Frees departed Zoom slots, updates memory, fills newcomers.
    /// Returns the ids freed this pass, with their slot ids, for the status line.
    @discardableResult
    mutating func syncFromMeeting(_ slots: inout [ShowInputSlot],
                                  roster: [RosterParticipant]) -> [(slotId: Int, participant: String)]
    /// The operator emptied `slotId` (any kind).
    mutating func operatorUnassigned(slotId: Int)
    /// The toggle flipped (either direction, D7): forget both memories, then free departed
    /// slots and, when enabled, fill from the WHOLE roster.
    mutating func reapply(_ slots: inout [ShowInputSlot], roster: [RosterParticipant])
}

// AppModel.swift
struct RosterParticipant { ...; var isMe = false; var sourceGeneration: Int64 = 0 }   // parse "isMe", "sourceGeneration"
@Published private(set) var autoAssignInputsEnabled = true      // mirrors assignmentLaw.autoAssignEnabled
@Published var sourcesStatus = ""                                // the Sources status line
func setAutoAssignInputs(_ on: Bool)                             // RC `automation.autoAssignInputs.set` binds here
```

**Behaviour.** Each rule quotes the Windows ruling it ports.

1. **Newcomers only.** "Auto-assign only places ids it has NEVER seen — i.e. real newcomers. Without this memory it refilled every 'not currently assigned' id on every sync tick (~1/s), so an operator's manual unassign/replace on the Sources screen was reverted within a second." (`ShowInputsCoordinator.cs:366-372`). On each accepted roster: `newcomers = rosterIds − seenThisMeeting`, then `seenThisMeeting = Set(rosterIds)` (`:396-401`). Fill from the newcomers in roster order. Skip `isMe` (D6) and ids already in a Zoom slot ("already in a slot — keep it stable", `ShowInputRosterService.cs:389-397`). Proven on Windows by `ShowInputsCoordinatorTests.ARosterSyncNeverRefillsASlotTheOperatorUnassigned` (`:839`).
2. **Reserved slots.** "Owner ruling (2026-09-12): sticky until the MEETING ROSTER EMPTIES" (`ShowInputsCoordinator.cs:384-385`). "The operator emptying a slot is a decision, not a vacancy." (`:515-517`). The fill takes the lowest-numbered slot with `kind == "unassigned"` whose id is not in `operatorClearedSlots` (`ShowInputRosterService.cs:402-405`). An empty accepted roster clears `operatorClearedSlots` (`ShowInputsCoordinator.cs:403-412`). Reservations bind auto-assign only: "the operator may still place anything there, and a roster refresh never undoes that" (`docs/reference/the-law-slots.md:35-37`). Proven on Windows by `AnEmptyRosterReleasesTheOperatorsClearedSlots` (`:875`).
3. **Departed guests free their slot.** "Free slots whose Zoom participant is no longer in the meeting." (`ShowInputRosterService.cs:365-376`). The freed slot becomes `ShowInputSlot(id:)`, the same reset `unassignSlot` uses (D10). A freed slot is NOT reserved. `sourcesStatus` becomes `"<name> left the meeting — input <n> is free."` for the last freed slot, where `<name>` is the slot's `name`.
4. **The gate** (spec correction 3). The law runs only when `rosterProjection.observe` accepted the snapshot, `participants` is present, and (`participants` is non-empty or `rosterRevision > 0`). `StudioViewModel.cs:11128`: `if (participants is { } && (participants.Count > 0 || snapshot.RosterRevision > 0))`. When `meetingState` is neither `in_meeting` nor `in-meeting`, the law receives `[]` (`LiveProductionSync.cs:119-123`). The UI `roster` keeps its current parse. A snapshot `rosterProjection` rejects frees nothing.
5. **Camera-off newcomers are placed** (D3). The `participant.hasVideo` filter at `AppModel.swift:1756` goes.
6. **The toggle.** `setAutoAssignInputs(on)`: no-op when unchanged. Otherwise set the flag and run `reapply` on the current `roster` (also in-meeting gated: `[]` when not in a meeting). "That is an explicit 'assign everyone now', so forget which ids were seen and fill from the whole roster" and "it also releases the operator's reserved slots" (`ShowInputsCoordinator.cs:425-438`). The Windows toggle handler reapplies on any change (`MagicSceneCoordinator.cs:509-514`). Proven on Windows by `ReapplyAutoAssign_FillsAFreeSlot_WithARoomParticipant_WhenEnabled` (`:785`) and `ReapplyAutoAssign_Disabled_DoesNotAssignFreeSlots` (`:800`).
7. **Operator paths.** `unassignSlot` calls `assignmentLaw.operatorUnassigned(slotId:)` for every kind. `assignSlot`, `toggleSlotInShow` and `toggleAssigned` lose all tombstone code; `manuallyUnassigned` is deleted. An in-show untick does not free or reserve: the slot still holds its guest, so auto-assign never touches it.
8. **One spine push per roster pass.** When the law changes any slot's `kind`, `sourceId` or `inShow`, call `recomputeFromSlots()` exactly once and skip the separate `syncSpine()` at `AppModel.swift:1781-1783`. `ShellTests.testRosterMembershipChangeRepushesTheSpine` (`ShellTests.swift:1785`) must stay green unchanged.
9. A filled slot gets `kind = "zoom"`, `sourceId`, `name = participant.name`, `inShow = true`, `offline = false`.

**Complete tests** (`ShellTestsSources.swift`). Shared fixture:

```swift
extension ShellTests {
    @MainActor
    static var sourcesCases: [(String, () -> Void)] {
        [
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
}
```

```swift
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
```

The property test. Quantification (rule 3): 120 seeds. Each runs 8 ticks over a pool of 14 ids. Roster size is drawn from 0…12, so 11 or more present forces overflow, and 0 is a real empty barrier. Roster order is shuffled each tick. Camera state is random per id per tick. Between ticks the operator unassigns a random Zoom slot one tick in three. Auto-assign is off for one seed in three. Every operand on the "expected" side comes from the TEST's own record of the previous roster and of the slots it unassigned. None comes from the law's memory (rule 2).

```swift
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
```

**Mutations to run** (each must go red; report the test that caught it):

- M1: fill from the whole roster instead of the newcomers. Expect the picker-replace, guest-for-guest and property tests to go red.
- M2: drop the `operatorClearedSlots` exclusion from the free-slot choice. Expect cleared-slot-reserved and property to go red.
- M3: never clear `operatorClearedSlots` on an empty roster. Expect empty-roster-releases to go red.
- M4: skip freeing departed Zoom slots. Expect the leaver test, out-of-meeting and property ("departed still holds") to go red.
- M5: remove the gate, so an unrevisioned empty roster runs the law. Expect rejected-roster-frees-nothing (the legacy half) to go red.
- M6: restore the `hasVideo` filter. Expect the camera-off test and the property liveness check to go red.
- M7: `reapply` keeps `operatorClearedSlots`. Expect the toggle test to go red: A lands in slot 4, not 1.
- M8: the roster pass calls `syncSpine()` in addition to `recomputeFromSlots()`. Expect `ShellTests.testRosterMembershipChangeRepushesTheSpine` to go red.
- M9: in `applyZoom`, pass `[]` to the law when the meeting state is `in-meeting` (hyphen). Expect `testRosterMembershipChangeRepushesTheSpine` or the legacy half of rejected-roster to go red. If both stay green, add an `in-meeting` placement assertion.

**Steps:**
- [ ] Step 0: tracking issue check (Global constraints).
- [ ] Step 1: Write the fixture and the 11 tests, and register `sourcesCases`. Run the harness. Record which tests fail on `e98436fc`: the build fails on `setAutoAssignInputs` / `sourcesStatus` / `autoAssignInputsEnabled`. Temporarily stub those three as no-ops to confirm the behavioural reds named in each doc comment, then remove the stubs.
- [ ] Step 2: Implement `ShowInputAssignmentLaw` and the `AppModel` changes.
- [ ] Step 3: Run the harness. Everything passes, including the 236 baseline cases unchanged.
- [ ] Step 4: Run M1-M9 and report.
- [ ] Step 5: Commit `mac sources S1: the slot law — newcomer-only auto-assign, reserved slots, departed guests freed, toggle`.

**Done when:** all checks pass; M1-M9 red; `manuallyUnassigned` no longer exists (`grep -n manuallyUnassigned mac-shell/Sources` prints nothing).

---

### Task 2: Every slot write logged with its writer; saved at once; one restore (S1)

**Lane:** shell. **Slice:** S1 rules 5-6, and "a second load is refused".

**Files:**
- Create: `ShowInputWriteScope.swift`
- Modify: `AppModel.swift`: `slots` (`:1794`) gains `didSet`; `persistPrefsNow` (`:1084-1090`); `applyRestoredPrefs` (`:656-731`); the writers `assignSlot`, `unassignSlot`, `toggleSlotInShow`, `toggleAssigned`, `applyZoom`, `setAutoAssignInputs`
- Modify: `ShowInputPersistence.swift` (`restoreSlotAssignments` in a `restore` scope); `StudioSceneEditing.swift:195-212` (`swapMultiviewSlots` in a `drag-swap` scope); `TabPageLayout.swift:68-80` and `StudioFooter.swift:969-970` (proof seeds in a `snapshot-seed` scope)
- Modify: `ShellTestsSources.swift`

**Interfaces:**

```swift
// ShowInputWriteScope.swift — Windows Models/ShowInputModels.cs:40-77, on the main actor
// (slot writes are main-actor only, like Windows' UI-thread-only rule).
@MainActor
enum ShowInputWriteScope {
    static let untracked = "UNTRACKED"
    /// The ambient reason; `untracked` outside any scope.
    static var reason: String { get }
    /// Nested scopes CHAIN ("control-api>operator-assign"), so an outer entry point
    /// (the control API, RC2) stays visible through the inner setter scope. Restores on throw.
    static func run<T>(_ reason: String, _ body: () throws -> T) rethrows -> T
}

// AppModel.swift
@Published var slots: [ShowInputSlot] = ... { didSet { slotsDidChange(from: oldValue) } }
/// TEST-ONLY: receives every slot-write line. Set only by ShellTests.
var slotWriteObserverForTesting: ((String) -> Void)?
```

Reasons, all literal strings: `operator-assign`, `operator-unassign`, `operator-in-show`, `roster-sync`, `auto-assign-toggle`, `restore`, `drag-swap`, `snapshot-seed`. Task 7 adds `capture-offline`.

**Behaviour:**

1. **The choke point.** "EVERY assignment write in the app funnels through these setters … Log each real change with the ambient ShowInputWriteScope reason so the NEXT mystery writer … is identifiable from launch.log alone." (`Models/ShowInputModels.cs:165-172`). `slotsDidChange(from:)` returns at once when `oldValue == slots`. Otherwise, for each array index present in both arrays, in ascending index order, it compares `kind`, then `sourceId`, then `inShow`. Each change emits exactly `slot-write: slot<new id> <field> '<old>'->'<new>' by=<ShowInputWriteScope.reason>` (booleans as `true`/`false`, the Windows quoting at `:188`). Each line goes to `ShellLog.write` and to `slotWriteObserverForTesting`. Other fields (name, key, mic, dropout, iso, offline) are not logged. Windows logs only the assignment fields.
2. **Immediate save.** "Persist IMMEDIATELY and synchronously: the save used to ride only ApplyShowInputRefresh, a COALESCED DispatcherQueuePriority.Low dispatcher callback, so an operator change followed by a crash (four on 2026-08-09 alone) lost the pending save" (`ShowInputsCoordinator.cs:206-213`). When `oldValue != slots`, `slotsDidChange` calls `persistPrefsNow()` after logging. `persistPrefsNow` keeps its `prefsRestored` guard and its `lastSavedPrefs` equality check, so an unchanged projection writes nothing. `slotsDidChange` must never write `slots`.
3. **Every production writer is wrapped**: `assignSlot` (`operator-assign`), `unassignSlot` (`operator-unassign`), `toggleSlotInShow` (`operator-in-show`), `toggleAssigned` (the scope of the path it takes), the law pass in `applyZoom` (`roster-sync`), `setAutoAssignInputs` (`auto-assign-toggle`), `restoreSlotAssignments` (`restore`), `swapMultiviewSlots` (`drag-swap`, and its existing `ShellLog` line keeps `by=operator-swap`), and the two proof seeds (`snapshot-seed`).
4. **One restore.** "A second load (a future respawn/second-window/deferred path) would overwrite live operator changes with stale disk state — refuse it LOUDLY." (`ShowInputsCoordinator.cs:146-153`). When `prefsRestored` is already true, `applyRestoredPrefs` logs `roster: restore REFUSED — already restored this session; a re-restore would overwrite live operator changes with disk state` and returns before touching any state.

**Complete tests:**

```swift
("sources-log/write-scope-chains-restores-throws", testWriteScopeChainsRestoresAndSurvivesAThrow),
("sources-log/every-writer-names-itself", testEverySlotWriterNamesItself),
("sources-save/slot-changes-reach-disk-now", testSlotChangesReachDiskWithoutTheAutosave),
("sources-save/second-restore-refused", testASecondRestoreIsRefused),
```

```swift
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
/// constant "meeting-T2". Setting joinMeetingId has no save side effect (AppModel.swift:410 is
/// a plain @Published). So reading "meeting-T2" back proves the ACTION saved. Fails on
/// e98436fc (disk keeps "stale-file" until the 5 s autosave).
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
    expectEqual(disk.joinMeetingId, "meeting-T2", "an auto-assign saves now")   // Task 3 amends this block
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
```

**Mutations to run:**
- Remove the `persistPrefsNow()` call from `slotsDidChange`. Expect slot-changes-reach-disk to go red, all three blocks.
- Remove the scope from `assignSlot`. Expect every-writer-names-itself, "operator assign" (`by=UNTRACKED`), to go red.
- Remove the scope from the law pass. Expect the "auto-assign" assertion to go red.
- Diff only `kind`. Expect the `sourceId` and `inShow` lines to go missing, red.
- Remove the second-restore guard. Expect second-restore-refused to go red, both assertions.
- Make `run` not restore on throw (no `defer`). Expect write-scope, the last assertion, to go red.

**Steps:** write tests → run (build fails on the missing symbols; once stubbed, the save and restore tests fail as their doc comments say) → implement → all green → mutations → commit `mac sources S1: slot-write log with writer reasons, immediate save, one restore`.

**Done when:** all green; mutations red; `grep -n "slots\[.*\]\.\(kind\|sourceId\|inShow\) *=" mac-shell/Sources/CoreVideoProShell/*.swift` lists only lines inside a `ShowInputWriteScope.run` or inside test files.

---

### Task 3: Zoom slots are meeting-scoped; the toggle is persisted and drawn (S1)

**Lane:** shell. **Slice:** S1 rule 4 (D2), the toggle UI and persistence.

**Files:**
- Modify: `ShowInputPersistence.swift` (`restoreSlotAssignments`), `AppModel.swift` (`currentPrefs` `:625-628`, `applyRestoredPrefs`, `resetAutomationDefaults`), `ShellPrefs.swift` (new key), `AutomationExtras.swift` (`AutomationExtrasSection`), `ShellApp.swift` (`SourcesPane` header, an interim spot until Task 7)
- Modify tests: `ShellTestsTier0.swift`, four tests (listed below); `ShellTestsSources.swift` (amend the third block of `testSlotChangesReachDiskWithoutTheAutosave`)

**Interfaces:**

```swift
// ShellPrefs
var autoAssignInputs: Bool?          // nil = never set → on (the Windows default, MagicSceneCoordinator.cs:458)
```

**Behaviour:**
1. `currentPrefs().showInputSlots` writes only `capture` slots (Task 4 of S4 adds `media` later). `restoreSlotAssignments` accepts only `capture` records; a `zoom` record in an older file is skipped. Reason, from spec decision 2: Zoom reuses user ids across meetings, so a restored slot could attach a different person's name and settings.
2. `applyRestoredPrefs` sets `autoAssignInputsEnabled` and `assignmentLaw.autoAssignEnabled` from `prefs.autoAssignInputs ?? true`, without running a reapply (no roster exists at launch). `currentPrefs` writes `autoAssignInputs`. `setAutoAssignInputs` calls `persistPrefsNow()` after the reapply.
3. `resetAutomationDefaults()` calls `setAutoAssignInputs(true)`, as Windows' reset does (`MagicSceneCoordinator.cs:458`).
4. UI copy, verbatim from Windows `Views/AutomationPage.xaml:9`: a toggle "Fill free inputs when participants join", placed in `AutomationExtrasSection` above "MAGIC SCENE". A second toggle with the same label goes in the Sources pane header, bound to the same property. Task 7 moves it into the table's tool row.

**Existing tests amended** (each keeps its purpose; the Zoom half changes to the D2 rule):

1. `tier0/slots-survive-relaunch` (`ShellTestsTier0.swift:203`): `guest?.sourceId == "42"` becomes `guest?.kind == "unassigned"`, with message "a Zoom slot is not restored (Zoom reuses ids; owner decision 2)". Delete the "a parked slot stays parked" line; its purpose moves to the capture slot. Add a second capture slot, `before.assignSlot(4, kind: "capture", sourceId: "cam2", name: "Tight cam")`, and park it with `before.toggleSlotInShow(4)` before the relaunch. Assert `after.slots[3].inShow == false` ("a parked slot stays parked"). The existing `cam?.inShow == true` assertion for slot 3 is unchanged. Also assert `before.currentPrefs().showInputSlots?.map(\.kind) == ["capture", "capture"]`.
2. `tier0/slot-settings-follow-the-source` (`:230`): the zoom-42 dropout assertion becomes `model.slots.allSatisfy { $0.dropoutPolicy.isEmpty }` ("a Zoom policy has no slot to follow after a relaunch"). `saved.slotDropoutPolicies?.first?.sourceKey == "zoom:42"` becomes `(saved.slotDropoutPolicies ?? []).isEmpty` ("no Zoom slot, no Zoom policy saved"). The key and mic assertions are unchanged.
3. `tier0/iso-survives-first-assignment` (`:265`): the fixture becomes `isoSelectedSourceIds = ["capture:cam1", "capture:cam2", "zoom:7"]` and `showInputSlots = [PersistedSlot(slotId: 2, kind: "capture", sourceId: "cam2", name: "Cam 2", inShow: true)]`. After `assignSlot(3, capture cam1)`, expect `isoSelectedSourceIds == ["capture:cam1", "capture:cam2"]` ("the restored ISO selection survives the first assignment; last meeting's unslotted zoom:7 is pruned"), slot 3 `iso == true`, slot 2 `iso == true`. After `unassignSlot(2)`, expect `isoSelectedSourceIds.contains("capture:cam2")`.
4. `tier0/iso-stale-intent-pruned-and-invisible` (`:322`), relaunch block: expect `relaunched.isoSelectedSourceIds == ["capture:cam1"]`, with message "a relaunch restores no Zoom slot, so all zoom: intent is pruned".
5. `sources-save/slot-changes-reach-disk-now`, third block, becomes: after the auto-assign, `disk.showInputSlots?.contains { $0.kind == "zoom" } != true` ("an auto-assigned guest is never written to disk (D2)"). Add a fourth block: `stale.save(); model.setAutoAssignInputs(false); disk = ShellPrefs.load()`, then `expectEqual(disk.autoAssignInputs, false, "the toggle saves now")` and `expectEqual(disk.joinMeetingId, "meeting-T2", "the whole file")`.

**New tests:**

```swift
("sources-law/zoom-slots-meeting-scoped", testZoomSlotsAreNeverRestoredOrSaved),
("sources-law/toggle-persisted-default-on", testTheAutoAssignToggleIsPersistedAndDefaultsOn),
```

```swift
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
```

Add `p.autoAssignInputs = false` to `roundTripSelfCheck`.

**Mutations to run:**
- `restoreSlotAssignments` accepts `zoom` again. Expect zoom-slots-meeting-scoped and amended test 1 to go red.
- `currentPrefs` writes Zoom slots. Expect zoom-slots-meeting-scoped (last assertion) and amended block 5 to go red.
- Drop `autoAssignInputs` from `init(from:)`. Expect toggle-persisted to go red ("off survives a relaunch", and the round trip).
- `resetAutomationDefaults` leaves the toggle alone. Expect toggle-persisted to go red.
- `setAutoAssignInputs` skips `persistPrefsNow`. Expect amended block 5's fourth part to go red.

**Proof (visible change):** `COREVIDEO_SHELL_TAB_SNAPSHOT=<dir> COREVIDEO_SHELL_TAB_LIST=Automation,Sources ./.build/release/CoreVideoProShell` at 1360 and 1920. Attach the PNGs showing both toggles.

**Steps:** amend and write tests → red as listed → implement → green → mutations → snapshots → commit `mac sources S1: Zoom slots are meeting-scoped; persisted auto-assign toggle on Automation and Sources`.

**Done when:** all green, mutations red, PNGs attached. That ends S1. The S1 live proof is in "Integration and owner check" below.

---

### Task 4: `SourceRowFactsProjection`: observed facts, never requests (S2)

**Lane:** shell. **Slice:** S2 (pure half).

**Files:**
- Create: `SourceRowFacts.swift`
- Modify: `ShellTestsSources.swift`

**Interfaces:**

```swift
struct SourceSlotRef: Equatable {
    let slotId: Int; let kind: String; let sourceId: String; let inShow: Bool
    init(_ slot: ShowInputSlot)
    init(slotId: Int, kind: String, sourceId: String, inShow: Bool)
    var sourceKey: String? { get }          // "zoom:<pid>" / "capture:<id>", same as ShowInputSlot.sourceKey
}
struct ZoomParticipantFact: Equatable { let id: String; let videoOn: Bool; let talking: Bool; let sourceGeneration: Int64 }
struct ZoomRosterFact: Equatable { let epoch: String; let meetingState: String; let participants: [ZoomParticipantFact] }
struct ZoomFormatFact: Equatable {
    let participantId: String; let rosterEpoch: String; let sourceGeneration: Int64
    let width: Int; let height: Int; let fps: Int; let frameId: Int; let frameAtMs: Double
    /// Parses a `zoom-source-format` event: fields under event["format"] (ZoomEngineRuntime.cpp:1953-1964).
    static func parse(event: JSONObject) -> ZoomFormatFact?
}
struct BusSourceRow: Equatable {
    let sourceId: String; let kind: String; let width: Int; let height: Int
    let framesIngested: Double; let sinceLastNewFrameMs: Double?   // JSON null → nil
    let liveness: String?; let onAir: String?; let lossReason: String?; let failInMs: Double?
    let lastSampleAgeMs: Double?; let dropoutPolicy: String; let dropoutPolicyApplies: Bool?
    /// Parses snapshot `sources[]` (SourceVideoIngress.cpp:91-131), keyed by sourceId.
    /// Zoom rows are keyed by the RAW participant id, capture rows by "capture:<id>".
    static func parse(_ sources: [JSONObject]) -> [String: BusSourceRow]
}
struct CaptureSignal: Equatable { let connectionState: String; let signalPresent: Bool }

struct SourceRowFacts: Equatable {
    var signal = "IDLE"        // IDLE | NO INCOMING | VIDEO OFF | CONNECTING | LIVE | TALKING | STALLED
    var format = "—"           // "W×H" (U+00D7) or "—"
    var fps = "—"              // integer string or "—"
    var formatStale = false
    var frameAgeMs: Double? = nil
    var tally = "none"         // pgm | pvw | talking | hold | none
    var writer = "OFF"         // OFF | ARMED | REC | ERROR | —
    var chip: String { signal == "LIVE" && tally == "pgm" ? "PGM" : signal }  // MultiviewInputRow.cs:56
}

struct SourceRowFactsProjection {
    static let staleAfterMs = 1500.0            // MultiviewInputRowsProjection.cs:484 (> 1500)
    static let fpsWindowMs = 900.0              // capture FPS needs samples spanning ≥ this
    init(slotIds: [Int] = Array(1...10))
    mutating func setSlots(_ refs: [SourceSlotRef])            // a changed sourceKey resets that row
    mutating func applyRoster(_ roster: ZoomRosterFact)        // a new epoch clears every Zoom format
    mutating func applyFormat(_ fact: ZoomFormatFact)          // fenced (below)
    mutating func applyBus(_ rows: [String: BusSourceRow], captures: [String: CaptureSignal], atMs: Double)
    mutating func applyTally(program: Set<String>, preview: Set<String>)   // sourceKeys
    mutating func applyWriters(_ chips: [String: String])      // sourceKey → recording|armed|error|unknown
    mutating func engineStopped()                              // operator Capture off, or core respawn
    func facts(slotId: Int) -> SourceRowFacts
}

enum IsoWriterChip {
    /// Port of CoreVideoPro.MediaCore/Services/IsoOutputLifecyclePolicy.cs `Observe`, plus the
    /// Mac rule that an "unreported" ISO stream is "unknown" (shown "—", never OFF; spec §4.9).
    /// Keyed by the stream's sourceId, else its participantId (the unreported branch,
    /// CoreStateCapture.cpp:2054-2066). A key without ':' is treated as "zoom:<key>"
    /// (Task 5 Step 0 confirms the real format).
    static func observe(_ recording: JSONObject?) -> [String: String]
}
```

**Behaviour.** This ports `MultiviewInputRowsProjection.PatchSlot` (`:329-487`) with the Mac's data sources (spec corrections 5 and 6).

- **Any row.** Unassigned, not in the show, or engine stopped gives `IDLE` with format and fps reset (`:362-368`). A slot whose `sourceKey` changes resets format, fps, stale and age, and takes the writer chip of the new key (`:334-344`).
- **Capture row** (`:369-385`). With no device row: `NO INCOMING`. A device that is not connected, or connected without `signalPresent`: `CONNECTING`. Otherwise `LIVE`. Format comes from the bus row `capture:<id>`, as `"\(width)×\(height)"` when both are > 0, else `"—"`. The FPS is MEASURED. The projection keeps the last 5 `(atMs, framesIngested)` samples per capture key. When the oldest-to-newest span is ≥ `fpsWindowMs`, fps is `Int(((Δframes / ΔatMs) * 1000).rounded())`, else `"—"`. FPS is `"—"` whenever `sinceLastNewFrameMs > staleAfterMs`. **`CaptureDeviceRow`'s `frameRate` and `resolution` are never read** (spec correction 5).
- **Zoom row.**
  - No roster fact since start or since `engineStopped`: `IDLE` (`:386-392`).
  - Roster state not `in_meeting`/`in-meeting`: `NO INCOMING` (`:393-399`).
  - Participant absent: `NO INCOMING` (`:400-408`).
  - The participant's `sourceGeneration` > 0 and different from the generation last applied to this row: reset the format (`:409-443`).
  - `videoOn == false`: `VIDEO OFF`, format reset (`:444-458`).
  - No accepted format fact for this row: `CONNECTING` (`:459-472`).
  - Otherwise format and fps come from the last accepted fact. Read the bus row keyed by the RAW pid. When `sinceLastNewFrameMs > staleAfterMs`: `STALLED`, `formatStale = true`, `frameAgeMs` = that age, fps `"—"`, format kept. Otherwise `TALKING` if the participant is talking, else `LIVE` (`:483-486`).
- **Format fence** (`ApplySourceFormatFact`, `:106-125`). Drop the fact when: the engine is stopped, no roster fact exists, the roster is not in a meeting, `fact.rosterEpoch != roster.epoch`, the participant is absent or `videoOn == false`, the participant generation is > 0 and differs from `fact.sourceGeneration`, or `fact.frameAtMs <=` the row's stored `lastFrameAtMs`.
- **A new roster epoch** clears every Zoom row's format and stored frame clock (`:86-99`).
- **Tally** (`MultiviewTileCropPolicy.cs:31-37`, plus `MultiviewInputRowsProjection.cs:298`). `VIDEO OFF` gives `none`. Otherwise `formatStale` gives `hold`; key in `program` gives `pgm`; in `preview` gives `pvw`; a Zoom participant talking gives `talking`; else `none`.
- **Writer:** `recording`→`REC`, `armed`→`ARMED`, `error`→`ERROR`, `unknown`→`—`, absent→`OFF` (`MultiviewInputRow.cs:57`).
- **`IsoWriterChip.observe`** is a verbatim port. `error` when lifecycle state is `failed`/`interrupted`, lifecycle health is `failed`, or stream status is `warning`/`failed`/`error`. `armed` when state is `requested`/`preparing`/`starting`. `recording` when state is `producing`/`live` and health is `healthy`/`degraded`; with no lifecycle, when `recording.active == true` and status is `recording`/`warning`. A stream whose status is `"unreported"` maps to `unknown`. `off` entries are omitted.

**Complete tests.** Fixture helpers:

```swift
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
```

```swift
("sources-facts/zoom-state-table", testZoomRowStateTable),
("sources-facts/capture-reads-observations-only", testCaptureRowReadsObservationsNeverRequests),
("sources-facts/reused-zoom-id-fenced", testReusedZoomIdClearsOldFormatAndRejectsOldFacts),
("sources-facts/epoch-and-frame-clock-fence", testANewEpochAndAnOlderFrameAreRejected),
("sources-facts/tally-precedence-all-inputs", testTallyPrecedenceOverEveryInputCombination),
("sources-facts/iso-chip-is-writer-lifecycle", testIsoChipUsesObservedWriterLifecycle),
("sources-facts/reassignment-resets-row", testReassigningASlotResetsItsRowBeforeTheNewSourceShows),
```

```swift
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
```

`SourceRowFacts` needs a memberwise `init` with defaults (`SourceRowFacts(signal:format:fps:)`) for the equality assertions.

**Mutations to run:**
- Read capture format and fps from `CaptureSignal`/device data (add the fields and use them). Expect capture-reads-observations-only to go red.
- `> 1500` becomes `>= 1500`. Expect zoom-state-table ("exactly 1500 ms") to go red.
- Drop the generation fence. Expect reused-zoom-id ("late fact … rejected") to go red.
- Drop the epoch fence. Expect epoch-and-frame-clock ("retired epoch") to go red.
- Drop the frame-clock check. Expect epoch-and-frame-clock ("older frame") to go red.
- Swap the `pgm` and `hold` precedence. Expect tally-precedence to go red.
- Chip shows `PGM` for any signal. Expect tally-precedence (chip) to go red.
- `unreported` maps to OFF. Expect iso-chip to go red.
- `setSlots` does not reset a reassigned row. Expect reassignment-resets-row to go red.

**Steps:** tests → build fails (new types) → implement → green → mutations → commit `mac sources S2: SourceRowFactsProjection — observed row facts (Windows MultiviewInputRowsProjection port)`.

**Done when:** all green, mutations red.

---

### Task 5: Facts wired from the core; chips on the existing rows; picker guard (S2)

**Lane:** shell. **Slice:** S2 (data plumbing plus first consumer).

**Files:**
- Create: `SourceFactsStore.swift`
- Modify: `AppModel.swift`: `applySnapshot` (`:1592-1720`), `applyZoom`, `handleEvent` (`:1898-1917`), `setCapture` (`:1889`), the two `rosterProjection.resetForProcess()` sites (`:918`, `:1477`), `slotsDidChange`, `assignSlot`
- Modify: `ShellApp.swift`: `SlotRow` (`:1713-1846`). It gains chips; Task 7 replaces it.
- Modify: `ShellTestsSources.swift`

**Step 0, before any code (spec risk table, master plan §7: re-verify the UNVERIFIED items).** Build nothing new. Run the existing app or core with a camera, a screen capture, the fake engine (`native/build-release-core/corevideo-zoom-engine-fake` exists) and a short recording with one ISO armed. Dump one `GET 127.0.0.1:8011/snapshot` (or the shell's `lastSnapshot` through the diagnostics export). Write the answers into the commit body and into `docs/superpowers/plans/2026-11-16-mac-sources-phase1-outcomes.md`:
  (a) Do capture `sources[]` rows carry real `liveness`/`onAir`/`lossReason`, or `"unreported"`?
  (b) Do ISO stream nodes carry per-writer `status` and `sourceId`, and in what id format (`zoom:<pid>` or bare)?
  (c) Does a `zoom-source-format` event reach `AppModel.handleEvent` (log the event type)?
  (d) Are Zoom `sources[]` rows keyed by the raw pid?
  (e) Do capture `sources[]` rows carry the frame width and height?
If (a) or (b) is unreported, the chip shows only what is observed (`—`), and you file a native follow-up issue. If (b) uses bare ids, keep the `IsoWriterChip` bare→`zoom:` mapping; otherwise delete it. If (c) or (d) fails, stop and report: the Zoom half of this task depends on them.

**Interfaces:**

```swift
@MainActor final class SlotFactsBox: ObservableObject { @Published private(set) var facts = SourceRowFacts() }
@MainActor final class SourceFactsStore {
    private(set) var projection = SourceRowFactsProjection()
    func box(_ slotId: Int) -> SlotFactsBox          // stable instance per slot id 1...10
    func mutate(_ change: (inout SourceRowFactsProjection) -> Void)   // then writes each box ONLY if its facts changed
}
// AppModel
let sourceFacts = SourceFactsStore()                  // NOT @Published (Global constraints)
var sourceFactsClockForTesting: (() -> Double)?       // ms; production: CACurrentMediaTime() * 1000
func handleEventForTesting(_ event: JSONObject)       // test seam onto handleEvent
func assignSlot(_ slotId: Int, kind: String, sourceId: String, name: String, openedFor: String?)
```

**Behaviour:**
1. `slotsDidChange` calls `sourceFacts.mutate { $0.setSlots(slots.map(SourceSlotRef.init)) }`.
2. On an accepted roster, `applyZoom` maps participants to `ZoomParticipantFact`, using the new `RosterParticipant.sourceGeneration` and `talking`, and calls `applyRoster(ZoomRosterFact(epoch: rosterProjection.epoch, meetingState: snapshot meetingState, …))`.
3. `handleEvent` gains `case "zoom-source-format"`: `ZoomFormatFact.parse(event:)` then `applyFormat`. Nothing else. No `syncSpine`, no `syncScenes`, no `@Published` write on `AppModel` (spec §4.4).
4. `applySnapshot`, when `sources` is present: `applyBus(BusSourceRow.parse(sources), captures: captureDevices → CaptureSignal, atMs: clock)`. When `programFrame` is present: `program = Set(videoSources.compactMap { $0["sourceId"] as? String })`. Preview comes from `buildRoutes(for: previewSceneId)`, mapping `participantId` → `"zoom:"+id` and `captureDeviceId` → `"capture:"+id` (empty when `previewSceneId` is empty). Then `applyTally`. When `recording` is present: `applyWriters(IsoWriterChip.observe(recording))`.
5. `engineStopped()` runs on `setCapture(enabled: false)` and at both `rosterProjection.resetForProcess()` sites.
6. **Picker guard** (audit row 12). `assignSlot(…, openedFor:)` refuses when `openedFor != slots[i].sourceKey`. It leaves the slot untouched and sets `sourcesStatus = "Input <n> changed while its menu was open. Choose again."`. `openedFor: nil` means "the slot was unassigned when the menu opened". The 4-argument `assignSlot` stays (the control API and tests use it) and does not guard.
7. **Consumer.** `SlotRow` observes `model.sourceFacts.box(slot.id)` (`@ObservedObject`). Next to the source menu it shows a signal chip (`facts.chip`), `facts.format`, then `facts.fps` in `.plexMono(10)`. Next to the ISO checkbox it shows the writer chip (`facts.writer`). Colours follow Windows `MultiviewInputRow.cs:58-71`, mapped to `Studio` tokens: LIVE `Studio.accent`, PGM `Studio.red`, TALKING/STALLED `Studio.amber`, others `Studio.secondary`; writer REC `Studio.red`, ARMED/ERROR `Studio.amber`. The source menu captures `slot.sourceKey` when its content is built, and its buttons call `assignSlot(…, openedFor:)`. `sourcesStatus` is drawn under the slot list when non-empty.

**Complete tests:**

```swift
("sources-facts/wired-zoom-format-one-row-no-sync", testFormatEventsPatchOneRowWithoutASync),
("sources-facts/wired-capture-bus-not-device", testCaptureRowIsWiredFromTheBus),
("sources-facts/wired-tally-and-writer", testTallyAndWriterAreWiredFromTheSnapshot),
("sources-facts/picker-guard", testAStalePickerChoiceIsRefused),
```

```swift
import Combine

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
```

**Mutations to run:**
- The `zoom-source-format` case calls `syncSpine()`. Expect wired-zoom-format ("no spine push") to go red.
- `SourceFactsStore.mutate` writes every box unconditionally. Expect wired-zoom-format ("another row") to go red.
- Make `sourceFacts` an `@Published` dictionary on `AppModel` that is written on every fact. Not testable by `objectWillChange` of box 2. Note it as a review item, and check it in the Task 7 Instruments pass.
- Feed `captureDevices.frameRate` to fps. Expect wired-capture ("not the device's 30") to go red.
- Skip `applyTally`. Expect wired-tally ("PGM") to go red.
- Drop the `openedFor` check. Expect picker-guard to go red.

**Proof:** `COREVIDEO_SHELL_TAB_SNAPSHOT` Sources at 1360 and 1920. Extend `TabSnapshotHarness.seed` (in a `snapshot-seed` scope) with a format event and a `sources[]` row so one row shows LIVE `1920×1080` `30` and one shows `CONNECTING`.

**Steps:** Step 0 spike → tests → red → implement → green → mutations → snapshots → commit `mac sources S2: observed facts wired (sources[], zoom-source-format, tally, ISO writer), chips on rows, picker guard`.

**Done when:** all green, mutations red, outcomes doc has the Step 0 answers, PNGs attached.

---

### Task 6: Types, picker groups, column layout and workspace state (S3, pure half)

**Lane:** shell. **Slice:** S3.

**Files:**
- Create: `SourcesWorkspace.swift` (`SourceType`, `SourcePickerCatalog`, `SourcesColumnLayout`, `SourcesColumnPrefs`, `SourcesWorkspaceState`, `SourcesSummary`)
- Modify: `ShellPrefs.swift` (`sourcesWorkspace: SourcesColumnPrefs?`), `ShellTestsSources.swift`

**Interfaces:**

```swift
enum SourceType: String, Equatable {
    case zoomGuest = "Zoom guest", camera = "Camera", captureCard = "Capture card", screen = "Screen"
    case window = "Window", unassigned = "Unassigned"   // still / mediaClip arrive with S4
    /// Mirrors Windows InferCaptureDeviceKind (Services/ShowInputRosterService.cs:96-117) for the Mac ids:
    /// "screen:" → screen, "window:" → window (SckScreenCaptureAdapter.mm:269,287),
    /// vendor blackmagic|aja (case-insensitive) → captureCard, otherwise camera.
    static func of(kind: String, sourceId: String, vendor: String?) -> SourceType
    var category: String           // Zoom | Cameras | Screens | Unassigned
    var pickerPrefix: String       // "Zoom · ", "Camera · ", "Capture card · ", "Screen · ", "Window · "
}
struct SourcePickerOption: Equatable { let sourceKey: String; let kind: String; let sourceId: String; let label: String; let unavailable: Bool }
enum SourcePickerCatalog {
    /// Groups in the Windows order Zoom, Camera, Screen. An empty group is omitted. When the slot's current
    /// source is absent, a first entry reads "Saved source unavailable — <name>" (ShowInputRosterService.cs:244-248).
    static func groups(roster: [RosterParticipant], devices: [CaptureDeviceRow],
                       current: ShowInputSlot?) -> [(title: String, options: [SourcePickerOption])]
}
struct SourcesColumnPrefs: Codable, Equatable {
    var showType = true, showSignal = true, showAudio = true, inspectorOpen = false
    init(); init(from decoder: Decoder) throws     // decodeIfPresent with these defaults
}
enum SourcesColumnLayout {
    static let headers = ["#", "Source", "Type", "Format", "FPS", "Signal", "Audio", "MV", "ISO", "Actions"]
    /// Port of SourcesTableRowControl.ColumnWidths (:115-120). -1 = the flexible Source column; 0 = hidden.
    static func widths(tableWidth: CGFloat, prefs: SourcesColumnPrefs) -> [CGFloat]
    static func showsThumbnail(tableWidth: CGFloat) -> Bool          // >= 750 (:112)
    static func inspectorBeside(pageWidth: CGFloat) -> Bool          // >= 1500 (SourcesWorkspaceControl.cs:158)
    static let inspectorWidth: CGFloat = 340
}
struct SourcesWorkspaceState: Equatable {        // UI-only; never writes slots, MV or ISO
    static let categories = ["All sources", "Zoom", "Cameras", "Screens", "Media", "Unassigned"]
    var selectedSlotId: Int?; var query = ""; var category = "All sources"; var columns = SourcesColumnPrefs()
    mutating func inspect(_ slotId: Int)         // select + open inspector
    func matches(slotId: Int, displayName: String, sourceLabel: String, type: SourceType) -> Bool
}
enum SourcesSummary {
    /// "x/10 assigned · n in MV · n ISO selected · n match filter" (SourcesWorkspaceControl.cs:150).
    static func text(slots: [ShowInputSlot], matching: Int) -> String
}
```

**Behaviour:** pure. The column formula, verbatim from Windows `SourcesTableRowControl.cs:115-119`: `[26, -1, type && w>=1050 ? max(78, .08w) : 0, max(w>=750 ? 100 : 85, .13w), max(42, .05w), signal && w>=750 ? max(86, .09w) : 0, audio && w>=1050 ? max(86, .12w) : 0, max(36, .04w), max(60, .06w), 140]`. `matches`: category `All sources` or equal to `type.category`, AND the query (trimmed) is empty or appears case-insensitively in `String(format: "%02d", slotId) + " " + displayName + " " + sourceLabel + " " + type.rawValue` (`ViewModels/SourcesWorkspaceState.cs:18-34`). `inspect` sets `selectedSlotId` and `columns.inspectorOpen = true`. A filter that hides the selected row does not change `selectedSlotId`.

**Complete tests:**

```swift
("sources-table/column-widths-windows-breakpoints", testColumnWidthsMatchTheWindowsBreakpoints),
("sources-table/types-and-categories", testSourceTypesLabelsAndCategories),
("sources-table/picker-groups", testPickerGroupsInWindowsOrderWithTheSavedUnavailableEntry),
("sources-table/filters-never-write", testInspectionFiltersAndColumnsNeverChangeAssignmentsOrLiveSelections),
("sources-table/column-prefs-round-trip", testColumnPreferencesRoundTripAndMissingFieldsDefaultVisible),
("sources-table/summary-copy", testSummaryUsesTheWindowsCopy),
```

```swift
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
```

Add `p.sourcesWorkspace = SourcesColumnPrefs()` (with one field flipped) to `roundTripSelfCheck`. Compute the fractional expected widths (749, 1049, 1050) once by hand and keep them in the test as literals. They must never be computed by calling the implementation.

**Mutations to run:**
- 1050 breakpoint becomes 1000. Expect column-widths ("1049") to go red.
- Swap the Format floors (85/100). Expect column-widths ("700", "749") to go red.
- `window:` maps to camera. Expect types ("Window") to go red.
- Drop the empty-group filter. Expect picker-groups ("empty groups") to go red.
- `inspect` also assigns the slot (make it call `assignSlot`). Expect filters-never-write to go red.
- Plain synthesized `Codable` on `SourcesColumnPrefs`. Expect column-prefs ("missing fields") to go red.

**Steps:** tests → red → implement → green → mutations → commit `mac sources S3: source types, grouped picker, Windows column layout and workspace state`.

---

### Task 7: The Sources table (S3, view half)

**Lane:** shell. **Slice:** S3.

**Files:**
- Create: `SourcesTable.swift` (`SourcesTable`, `SourcesTableRow`, `SourcesToolsRow`, `CaptureDevicesSection`)
- Modify: `TabPageLayout.swift` (`.sources: SourcesTable()`; the seed becomes the mixed roster below), `ShellApp.swift` (delete `SourcesPane`, `SlotRow` and `StepCard` if unused elsewhere; `ChromaKeyControl` stays until Task 10), `AppModel.swift` (`takeCaptureDeviceOffline`, `refreshCaptureDevices`, `sourcesWorkspace` state + prefs)
- Modify tests: replace `tier0/sources-rows-fit-the-page` (`ShellTestsTier0.swift:386`), which measures the deleted `SlotRow`; `ShellTestsSources.swift`

**Interfaces:**

```swift
// AppModel
@Published var sourcesWorkspace = SourcesWorkspaceState()      // columns saved under ShellPrefs.sourcesWorkspace (autosave)
func takeCaptureDeviceOffline(_ deviceId: String)               // disconnect + unassign every slot holding it
func refreshCaptureDevices()                                    // sends list-capture-devices through quitRequest(…)
```

**Behaviour:**
1. **Layout** (spec §4.6). The header and rows share `SourcesColumnLayout.widths(tableWidth:prefs:)`. `tableWidth` = the page width (`TabPageLayout.pageWidth(for: .sources, window:)`) minus the card padding (28), minus `inspectorWidth + 12` when the inspector is beside the table. The thumbnail column (96 × 54 pt) shows the D1 placeholder "Preview unavailable" and hides under 750. No horizontal scroller.
2. **Row:**
   - number;
   - placeholder thumbnail;
   - source picker from `SourcePickerCatalog`, with the type line `SourceType.rawValue` under it; it uses `assignSlot(…, openedFor:)`, and connects a chosen capture device as today;
   - Type, Format, FPS, Signal chip, Audio text: "Zoom source audio", the paired microphone name, or "No paired microphone";
   - MV checkbox, accessibility label "Input N in multiview";
   - ISO checkbox with the writer chip;
   - Actions: "Inspect" (selects, opens the inspector; Task 10 draws it) and a "…" menu. Grade is not drawn (Task 10, `gradeEditorAvailable`).
3. **Interim D5 controls.** Until Task 10, the "…" menu holds "Rename…" (a popover `TextField` writing the name exactly as `SlotRow` did), "Microphone" (capture only, the existing `pairAudio`), the existing `ChromaKeyControl` menu content as a "Key" submenu, "On dropout" (zoom only, as today), and "Unassign". Nothing the operator could do in the old row is lost.
4. **Tools row:** a search field, the category picker (`SourcesWorkspaceState.categories`), a "Columns" menu (Type, Signal, Audio), the "Fill free inputs when participants join" toggle moved here from Task 3's header, and "Open inspector" / "Close inspector".
5. **Summary** (`SourcesSummary.text`), the `rosterRecoveryNotice`, and the status line (`sourcesStatus`). The table note uses the Windows copy (`SourcesWorkspaceControl.cs:89`): "Observed source format/FPS. A stale source is not advancing video. ISO selection is separate from writer state."
6. **Selection** is `sourcesWorkspace.selectedSlotId`. A row click selects. Selection never writes a slot.
7. **Capture devices section** (below the table; replaces the 8-device cap at `ShellApp.swift:1572,1607-1610`). Every device, in two groups, "Cameras" (camera + capture card) and "Screens" (screen + window). Each device shows its signal dot, name and warning line, as today, plus "Connect" / "Disconnect" (existing `connectCaptureDevice`) and "Take offline". The header has "Refresh". The Windows empty-state copy applies when the list is empty. The "Zoom feed health" card stays (Mac-only, spec §4.6).
8. **`takeCaptureDeviceOffline`.** Port of `ShowInputsCoordinator.TakeCaptureDeviceOfflineAsync` (`:460-500`). Send `disconnect-capture-device`. Inside a `capture-offline` scope, reset every slot whose `kind == "capture" && sourceId == deviceId` to `ShowInputSlot(id:)`. Do NOT reserve those slots: Windows `editor.Unassign()` there is not `UnassignShowInput`. Then `recomputeFromSlots()` once and `sourcesStatus = "<device name> is offline."` (`:492`).
9. **`refreshCaptureDevices`.** Request `["type": "list-capture-devices"]` through the existing seam-aware `quitRequest`. Parse `response["devices"]` with the same mapping `applySnapshot` uses (factor that mapping into one `static func parseCaptureDevices(_:)` used by both), then `refreshSlotHealth()` and `reconnectRestoredCaptures()`.
10. **Accessibility.** Every control's `accessibilityLabel` includes "Input N" (spec §4.6).

**Complete tests:**

```swift
("sources-table/fits-the-page", testSourcesTableFitsThePageAtBothWindowWidths),
("sources-table/take-offline", testTakeOfflineUnassignsEverySlotHoldingTheDeviceWithoutReserving),
("sources-table/refresh-capture-devices", testRefreshAsksTheCoreAndAppliesTheList),
```

```swift
/// Replaces tier0/sources-rows-fit-the-page. FIXTURE: TabSnapshotHarness.seed (the mixed roster).
@MainActor
static func testSourcesTableFitsThePageAtBothWindowWidths() {
    for window in [1360.0, 1920.0] {
        let model = AppModel()
        TabSnapshotHarness.seed(model)
        let page = TabPageLayout.pageWidth(for: .sources, window: CGFloat(window))
        let host = NSHostingView(rootView: SourcesTable().environmentObject(model).frame(width: page))
        host.layoutSubtreeIfNeeded()
        expect(host.fittingSize.width <= page + 0.5, "the table fits a \(Int(window)) pt window (\(host.fittingSize.width) ≤ \(page))")
        let table = page - 28
        let fixed = SourcesColumnLayout.widths(tableWidth: table, prefs: model.sourcesWorkspace.columns)
            .filter { $0 > 0 }.reduce(0, +)
        expect(fixed < table, "the flexible Source column keeps a positive width at \(Int(window))")
    }
}

@MainActor
static func testTakeOfflineUnassignsEverySlotHoldingTheDeviceWithoutReserving() {
    let model = AppModel()
    model.captureDevices = [CaptureDeviceRow(id: "cam1", name: "Wide cam", kind: "video", vendor: "avfoundation",
                                             connectionState: "connected", signalPresent: true, warning: "")]
    model.assignSlot(1, kind: "capture", sourceId: "cam1", name: "Wide cam")
    model.assignSlot(3, kind: "capture", sourceId: "cam1", name: "Wide cam")
    var lines: [String] = []
    model.slotWriteObserverForTesting = { lines.append($0) }
    model.takeCaptureDeviceOffline("cam1")
    expect(model.slots[0].kind == "unassigned" && model.slots[2].kind == "unassigned", "both slots are unassigned")
    expect(lines.allSatisfy { $0.hasSuffix("by=capture-offline") } && !lines.isEmpty, "each write names capture-offline")
    expectEqual(model.sourcesStatus, "Wide cam is offline.", "the Windows status copy")
    model.applyZoomForTesting(lawRoster(["A"], revision: 1))
    expectEqual(zoomSlot(model, holding: "A"), 1, "taking a device offline does not reserve its slot")
}

@MainActor
static func testRefreshAsksTheCoreAndAppliesTheList() {
    let model = AppModel()
    var asked: [String] = []
    model.quitRequestForTesting = { body, _ in
        asked.append(body["type"] as? String ?? "")
        return ["type": "capture-devices", "devices": [["id": "screen:0", "name": "Display 1",
                 "vendor": "ScreenCaptureKit", "connectionState": "detected", "signalPresent": false]]]
    }
    model.refreshCaptureDevices()
    spin(until: { !model.captureDevices.isEmpty }, limit: 2)
    expectEqual(asked, ["list-capture-devices"], "Refresh sends the core's list command")
    expectEqual(model.captureDevices.map(\.id), ["screen:0"], "and shows what the core answered")
}
```

`TabSnapshotHarness.seed`, in a `snapshot-seed` scope, becomes the mixed roster from spec §6:
- slot 1, Zoom "101", live, talking, `1920×1080@30` via a format event;
- slot 2, Zoom "102", stalled (bus age 2400);
- slot 3, capture "cam-1" with a mic, live at `1280×720` via `sources[]`;
- slot 4, capture "screen:0" ("Display 1");
- slot 5, capture "cam-gone", offline;
- slot 1 ISO armed, slot 3 on Program.

Update `testFormatEventsPatchOneRowWithoutASync`-style seeding accordingly. Keep the seed deterministic: no clock reads, so set `sourceFactsClockForTesting`.

**Mutations to run:**
- `takeCaptureDeviceOffline` calls `unassignSlot` (which reserves). Expect take-offline ("does not reserve") to go red.
- Drop the `capture-offline` scope. Expect take-offline (reasons) to go red.
- `refreshCaptureDevices` parses `response["captureDevices"]`. Expect refresh to go red.
- Hard-code the old 720 pt row width. Expect fits-the-page at 1360 to go red. (Confirmatory: it stays green if the table happens to fit. Keep it for the shape, and rely on the snapshot.)

**Proofs (S3 layout, spec §6 "Pixel proofs"):**
- `COREVIDEO_SHELL_TAB_SNAPSHOT=<dir> COREVIDEO_SHELL_TAB_LIST=Sources` gives `tab-sources-1360.png` and `tab-sources-1920.png`, with the mixed seed.
- `COREVIDEO_SHELL_SNAPSHOT=<png> COREVIDEO_SHELL_TAB=Sources COREVIDEO_SHELL_WINDOW=1360x900`, and again at `1920x1080`, against the running app with the fake engine and a real camera. This is the real-window proof that the offscreen harness cannot give (AppKit controls, real fonts).
- Instruments (Time Profiler or SwiftUI view-body counts) for 60 s with 4 fake-engine guests delivering format events. Report SwiftUI body evaluations per second for `SourcesTableRow` (spec risk "redraws too often"). Rule of thumb: a row whose facts did not change is not re-evaluated by a fact on another row.
- The owner's side-by-side with Windows (spec S3 proof): the two snapshots next to a Windows Sources screenshot, attached to the PR.

**Steps:** tests → red → implement → green → mutations → snapshots and Instruments → commit `mac sources S3: the Sources table — columns, filters, capture section, labelled types`.

**Done when:** all green, mutations red, four PNGs and the Instruments note attached. That ends S3.

---

### Task 8: Per-source display names and lower-third text (S5)

**Lane:** shell. **Slice:** S5 (names, lower-third text, Overlays consumer).

**Files:**
- Create: `SourceNames.swift` (`SourceLowerThirdText`, the name resolution helpers)
- Modify: `AppModel.swift` (`assignSlot` `:1820`, multiview labels `:2334,2340`, `audioSourceLabel` `:3990-4001`, the control snapshot `slots[].name` `:4578`, multiview cue labels `:2916,2939,3384`, `showLowerThird` `:3840-3861`, `currentPrefs` / `applyRestoredPrefs`), `ProgramLowerThird.swift` (`lowerThirdDisplayName` `:91-104`), `ShellPrefs.swift`, `Overlays.swift` ("Use source text" toggle), `SourcesTable.swift` (the name shown in the row and in "Rename…")
- Modify: `ShellTestsSources.swift`

**Interfaces:**

```swift
// ShellPrefs (all optional)
var sourceDisplayNames: [String: String]?      // capture keys only on disk; Zoom names are meeting-scoped
var sourceSecondaryLines: [String: String]?    // missing = source default, "" = hide, text = custom
var lowerThirdUseSourceText: Bool?             // D8, nil = off

// AppModel
@Published var sourceDisplayNames: [String: String] = [:]
@Published var sourceSecondaryLines: [String: String] = [:]
@Published var lowerThirdUseSourceText = false
func derivedSourceName(for slot: ShowInputSlot) -> String   // slot.name if non-empty, else roster/device name, else "Input N"
func displayName(for slot: ShowInputSlot) -> String         // override[sourceKey] ?? derived; "" when unassigned
func setDisplayName(slotId: Int, _ value: String)           // trimmed; blank or == derived removes the override
func setSecondaryLine(slotId: Int, _ value: String?)        // nil = source default

// SourceNames.swift: port of Services/SourceLowerThirdText.cs
enum SecondaryLineMode { case sourceDefault, custom, hidden }
enum SourceLowerThirdText {
    static func mode(usesDefault: Bool, text: String) -> SecondaryLineMode
    static func value(_ mode: SecondaryLineMode, custom: String) -> String?      // nil | "" | trimmed
    static func resolve(_ overrides: [String: String], sourceId: String?, title: String, org: String) -> (title: String, org: String)
    @discardableResult
    static func apply(names: inout [String: String], secondary: inout [String: String],
                      sourceId: String, name: String?, secondaryLine: String?) -> Bool
}
```

**Behaviour:**
1. **The name belongs to the source.** "Setting it blank resets to the derived name … Persist blank (or 'same as derived') as a reset so we never store a redundant override." (`ViewModels/ShowInputSlotViewModel.cs:421-437`). `slot.name` is now only the DERIVED label captured when the slot was assigned: `assignSlot` always sets `slot.name = name`, which fixes spec §3.2 item 7. The override lives in `sourceDisplayNames[sourceKey]`. `derivedSourceName` prefers `slot.name`, so existing callers that pass the roster name keep their text. That is the reason `tier0/lower-third-keys-a-program-source` stays green.
2. **Zoom overrides are meeting-scoped.** They are removed when the law sees an empty accepted roster, and never written to disk. Capture overrides persist.
3. **Migration** (spec risk table). On restore, when `prefs.sourceDisplayNames == nil` (an older file), each restored capture `PersistedSlot` with a non-empty `name` seeds `sourceDisplayNames["capture:" + sourceId]` with that name. It is keyed by the record's OWN source, so it can never attach to another source.
4. **Consumers read `displayName(for:)`:** the multiview source labels, `audioSourceLabel`, `lowerThirdDisplayName`, the control snapshot `slots[].name`, the multiview cue labels and log line, and the Sources row and inspector. `set-source-policy.displayName` is Task 9.
5. **Lower-third text.** Port `SourceLowerThirdText` verbatim: `Mode`, `Value`, `Resolve` ("Missing = source default; an explicitly saved empty string = hide the entire secondary line", `SourceLowerThirdText.cs:19-23`), and `Apply`. In `showLowerThird`, `title` = `lowerThirdUseSourceText ? SourceLowerThirdText.resolve(sourceSecondaryLines, sourceId: subject.source.sourceId, title: lowerThirdTitle, org: "").title : lowerThirdTitle`. The keyed-out command uses the same title the key-in sent. The Overlays tab gets `Toggle("Use source text", …)` under "Title", with the copy "Uses each source's lower-third text from Sources when it has one."

**Complete tests:**

```swift
("sources-names/blank-or-derived-resets", testABlankOrDerivedNameResetsTheOverride),
("sources-names/name-follows-the-source", testANameFollowsItsSourceNotTheSlot),
("sources-names/persisted-capture-only-and-migrated", testNamesPersistForCaptureOnlyAndMigrateFromTheSlotName),
("sources-names/consumers-read-the-resolved-name", testEveryConsumerReadsTheResolvedName),
("sources-lt/modes-distinct", testSecondaryLineModesStayDistinct),
("sources-lt/saved-blank-suppresses-until-reset", testASavedBlankSuppressesFallbackUntilReset),
("sources-lt/use-source-text-keys-the-override", testUseSourceTextKeysTheSourcesSecondaryLine),
```

```swift
@MainActor
static func testABlankOrDerivedNameResetsTheOverride() {
    let model = AppModel()
    model.assignSlot(3, kind: "capture", sourceId: "cam1", name: "FaceTime")
    model.setDisplayName(slotId: 3, "  Hero cam  ")
    expectEqual(model.displayName(for: model.slots[2]), "Hero cam", "an override, trimmed")
    expectEqual(model.sourceDisplayNames["capture:cam1"], "Hero cam", "stored by source")
    model.setDisplayName(slotId: 3, "   ")
    expect(model.sourceDisplayNames["capture:cam1"] == nil, "blank resets")
    model.setDisplayName(slotId: 3, "FaceTime")
    expect(model.sourceDisplayNames["capture:cam1"] == nil, "equal to the derived name stores nothing")
}

/// Spec §3.2 item 7: a slot reassigned to another source kept the previous source's name.
@MainActor
static func testANameFollowsItsSourceNotTheSlot() {
    let model = AppModel()
    model.assignSlot(1, kind: "capture", sourceId: "cam1", name: "FaceTime")
    model.setDisplayName(slotId: 1, "Hero cam")
    model.assignSlot(1, kind: "capture", sourceId: "cam2", name: "Desk cam")
    expectEqual(model.displayName(for: model.slots[0]), "Desk cam", "the new source shows its own name")
    model.assignSlot(1, kind: "capture", sourceId: "cam1", name: "FaceTime")
    expectEqual(model.displayName(for: model.slots[0]), "Hero cam", "the old source gets its name back")
}

@MainActor
static func testNamesPersistForCaptureOnlyAndMigrateFromTheSlotName() {
    let model = AppModel()
    model.assignSlot(1, kind: "capture", sourceId: "cam1", name: "FaceTime")
    model.setDisplayName(slotId: 1, "Hero cam")
    model.assignSlot(2, kind: "zoom", sourceId: "42", name: "Ana")
    model.setDisplayName(slotId: 2, "Dr Ana")
    expectEqual(model.currentPrefs().sourceDisplayNames, ["capture:cam1": "Hero cam"], "Zoom names never reach disk")
    model.applyZoomForTesting(lawRoster([], revision: 1))
    expect(model.sourceDisplayNames["zoom:42"] == nil, "a Zoom name ends with the meeting")
    var older = ShellPrefs()          // no sourceDisplayNames key
    older.showInputSlots = [PersistedSlot(slotId: 4, kind: "capture", sourceId: "cam9", name: "Old name", inShow: true)]
    let migrated = AppModel(); migrated.applyRestoredPrefs(older)
    expectEqual(migrated.sourceDisplayNames, ["capture:cam9": "Old name"], "the slot name migrates onto ITS OWN source")
    expect(ShellPrefs.roundTripSelfCheck() == nil, "the three new keys survive encode→decode")
}

@MainActor
static func testEveryConsumerReadsTheResolvedName() {
    let model = AppModel()
    seedProgramSource(model, pid: "42", name: "Jordan")
    model.assignSlot(1, kind: "zoom", sourceId: "42", name: "Jordan")
    model.setDisplayName(slotId: 1, "Jordan Wallace, host")
    let spine = model.spinePushRequests().last?["spinePayload"] as? JSONObject
    let sources = (spine?["multiview"] as? JSONObject)?["sources"] as? [JSONObject]
    expectEqual(sources?.first?["label"] as? String, "Jordan Wallace, host", "the multiview label")
    expectEqual(model.audioSourceLabel("42"), "Jordan Wallace, host", "the audio label")
    var sent: [JSONObject] = []
    model.overlayCommandObserver = { sent.append($0) }
    model.showLowerThird()
    expectEqual(sent.first?["sourceName"] as? String, "Jordan Wallace, host", "the lower third")
}

/// Port of SourceLowerThirdTextTests.DefaultCustomAndHiddenRemainDistinctAndHideDoesNotEraseCustomDraft.
static func testSecondaryLineModesStayDistinct() {
    let typed = "  Presenter  "
    expect(SourceLowerThirdText.value(.sourceDefault, custom: typed) == nil, "default is nil")
    expectEqual(SourceLowerThirdText.value(.custom, custom: typed), "Presenter", "custom is trimmed")
    expectEqual(SourceLowerThirdText.value(.hidden, custom: typed), "", "hidden is empty")
    expectEqual(SourceLowerThirdText.mode(usesDefault: true, text: ""), .sourceDefault, "mode default")
    expectEqual(SourceLowerThirdText.mode(usesDefault: false, text: ""), .hidden, "mode hidden")
    expectEqual(SourceLowerThirdText.mode(usesDefault: false, text: "Presenter"), .custom, "mode custom")
}

/// Port of SourceLowerThirdTextTests.SavedBlankSuppressesAllMetadataFallbackUntilExplicitReset.
static func testASavedBlankSuppressesFallbackUntilReset() {
    var names: [String: String] = [:], secondary: [String: String] = [:]
    expect(SourceLowerThirdText.resolve(secondary, sourceId: "zoom:a", title: "Guest", org: "Guest") == ("Guest", "Guest"), "default")
    expect(SourceLowerThirdText.apply(names: &names, secondary: &secondary, sourceId: "zoom:a", name: "Alice", secondaryLine: "  "), "changed")
    expect(SourceLowerThirdText.resolve(secondary, sourceId: "zoom:a", title: "Presenter", org: "New role") == ("", ""), "saved blank hides all")
    expect(!SourceLowerThirdText.apply(names: &names, secondary: &secondary, sourceId: "zoom:a", name: "Alice", secondaryLine: ""), "no change")
    expect(SourceLowerThirdText.apply(names: &names, secondary: &secondary, sourceId: "zoom:a", name: "Alice", secondaryLine: nil), "reset")
    expect(SourceLowerThirdText.resolve(secondary, sourceId: "zoom:a", title: "Presenter", org: "New role") == ("Presenter", "New role"),
           "after reset the fallback returns")
}

@MainActor
static func testUseSourceTextKeysTheSourcesSecondaryLine() {
    let model = AppModel()
    seedProgramSource(model, pid: "42", name: "Jordan")
    model.assignSlot(1, kind: "zoom", sourceId: "42", name: "Jordan")
    model.lowerThirdTitle = "Typed title"
    model.setSecondaryLine(slotId: 1, "Chief engineer")
    var sent: [JSONObject] = []
    model.overlayCommandObserver = { sent.append($0) }
    model.showLowerThird()
    expectEqual(sent.last?["title"] as? String, "Typed title", "off (D8 default): the typed title")
    model.lowerThirdUseSourceText = true
    model.showLowerThird()
    expectEqual(sent.last?["title"] as? String, "Chief engineer", "on: the source's saved line")
    model.setSecondaryLine(slotId: 1, "")
    model.showLowerThird()
    expectEqual(sent.last?["title"] as? String, "", "a saved blank hides the line")
}
```

**Mutations to run:**
- `setDisplayName` stores a value equal to derived. Expect blank-or-derived-resets to go red.
- Key overrides by slot id. Expect name-follows-the-source to go red.
- Persist Zoom names. Expect persisted-capture-only to go red.
- The migration seeds by slot number into whatever is there now. Expect persisted-capture-only to go red. To make it visible, add a second restored capture in slot 1 holding a different id, and check that `cam9`'s name does not land on it.
- The multiview label reads `slot.name`. Expect consumers ("multiview label") to go red.
- `resolve` treats `""` as missing. Expect saved-blank-suppresses to go red.
- Ignore `lowerThirdUseSourceText`. Expect use-source-text to go red.

**Steps:** tests → red → implement → green → mutations → commit `mac sources S5: per-source display names and lower-third text with the Overlays consumer`.

---

### Task 9: Capture dropout, the role store, and source-policy commands (S5)

**Lane:** shell. **Slice:** S5 (dropout for capture, Role, `set-source-policy`).

**Files:**
- Create: `SourcePolicies.swift` (`CaptureDropoutStatus`, `ProductionRoles`)
- Modify: `AppModel.swift`: `sourcePolicyCommands` / `appendSourcePolicies` (`:2457-2492`) and their call site (`:3269`); `setDropoutPolicy` (`:2497-2504`); `spinePushRequests` participants (`:2296-2305`); `currentPrefs` `slotDropoutPolicies` (`:621-624`); `applyRestoredPrefs` dropout restore (`:675-680`); the comment at `:168-175`, which is stale (spec §3.3)
- Modify tests: `ShellTests.swift:1906` (`testSourcePolicyCommandsAreZoomOnlyAndOrdered`, renamed and rewritten below; update its registration at `:3937`'s neighbour); `ShellTestsSources.swift`

**Interfaces:**

```swift
enum CaptureDropoutStatus {
    static let unavailableNotice = "This source does not report signal loss yet, so it always shows its last picture."
    static let policies = [("hold", "Hold last frame (up to 5 s, then slate)"), ("black", "Black (then slate)")]
    /// Port of Services/CaptureDropoutPresentation.cs Status (:18-37).
    static func line(_ row: BusSourceRow?) -> String
    static func applies(_ row: BusSourceRow?) -> Bool          // kind == capture && dropoutPolicyApplies == true
}
struct ProductionRoles: Equatable {
    /// Windows ProductionRoleService.Roles (Services/ProductionRoleService.cs:17-28), values and labels verbatim.
    static let roles: [(value: String, label: String)]
    private(set) var byParticipant: [String: String] = [:]
    /// One holder per role (StudioViewModel.cs:5041-5051). Refused when the participant is absent
    /// from `present`. nil/blank clears. Returns the status line Windows writes, or nil when unchanged.
    mutating func set(_ participantId: String, role: String?, present: [RosterParticipant]) -> String?
    mutating func clear()                                  // meeting-scoped: called on an empty accepted roster
}
// AppModel
static let roleControlAvailable = false                    // D4
private(set) var productionRoles = ProductionRoles()
func setParticipantRole(_ participantId: String, role: String?)
static func sourcePolicyCommands(slots: [ShowInputSlot], displayName: (ShowInputSlot) -> String) -> [JSONObject]
```

**Behaviour:**
1. **Dropout for capture** (spec §3.3, the stale rule). The core accepts a policy on `capture:<id>` (`native/src/core/MediaCore.cpp:1907-1922`). `setDropoutPolicy` accepts `zoom` and `capture` slots. `slotDropoutPolicies` persists capture slots only (D2). Restore attaches capture policies to capture slots by `sourceKey`.
2. **`sourcePolicyCommands`** emits one `set-source-policy` per assigned `zoom`/`capture` slot, de-duplicated by wire id keep-first and sorted by the wire `sourceId` (`zoom:<pid>`, `capture:<id>`) ordinal. Each carries `displayName: displayName(slot)` always. It carries `dropoutPolicy` only when the slot's policy is `hold` or `black`; an explicit hold is sent, PRESENT-OR-KEEP (`MediaCore.cpp:1898-1905`). The call site passes `displayName(for:)`. Policies stay last in the batch, after `load-scene-graph`. The existing `appendSourcePolicies` test stays green.
3. **Capture status line.** Verbatim branches from `CaptureDropoutPresentation.cs:26-36`:
   - `lost`+`failed` gives `"Lost (<reason>) — slate on Program"`;
   - `lost` otherwise gives `"Lost (<reason>) — holding, slate in <s>"`, or `Program black` for policy black;
   - `stalled` gives `"No new frames for <age> — holding last frame"`, or `Program black`;
   - `live` gives `"Live"`;
   - anything else gives `""`.
   `<reason>` is `lossReason` with `-` replaced by a space, or "signal loss" when empty. Seconds use `0.0 s`, and "unknown" when the value is absent.
4. **Roles.** A port of `SetParticipantProductionRole` (`StudioViewModel.cs:5020-5058`). The status line is `"<name> is now <label>"` or `"<name> role cleared"`. Roles clear on an empty accepted roster. The spine participant's `"role"` becomes `productionRoles.byParticipant[id] ?? "guest"`. With D4, nothing in the UI can set a role while `roleControlAvailable` is false, so the wire is unchanged in practice.

**Complete tests:**

```swift
("sources-policy/commands-zoom-and-capture-with-names", testSourcePolicyCommandsCoverZoomAndCaptureWithNames),
("sources-policy/capture-status-lines", testCaptureDropoutStatusUsesTheWindowsWording),
("sources-policy/capture-policy-persists", testACapturePolicyPersistsAndFollowsItsSource),
("sources-role/one-holder-per-role", testOneHolderPerRoleAndMeetingScoped),
```

The rewritten test replaces `testSourcePolicyCommandsAreZoomOnlyAndOrdered` (`ShellTests.swift:1906`) and keeps its registration name slot, renamed `("source-policy/zoom-and-capture-ordered", …)`:

```swift
@MainActor
static func testSourcePolicyCommandsCoverZoomAndCaptureWithNames() {
    var zoomB = ShowInputSlot(id: 1); zoomB.kind = "zoom"; zoomB.sourceId = "20"; zoomB.dropoutPolicy = "black"; zoomB.name = "B"
    var zoomA = ShowInputSlot(id: 2); zoomA.kind = "zoom"; zoomA.sourceId = "11"; zoomA.dropoutPolicy = "hold"; zoomA.name = "A"
    var cam = ShowInputSlot(id: 3); cam.kind = "capture"; cam.sourceId = "cam"; cam.dropoutPolicy = "black"; cam.name = "Cam"
    var unset = ShowInputSlot(id: 4); unset.kind = "zoom"; unset.sourceId = "30"; unset.name = "C"
    let dup = zoomA
    let commands = AppModel.sourcePolicyCommands(slots: [zoomB, zoomA, cam, unset, dup]) { "name:" + $0.name }
    expectEqual(commands.compactMap { $0["sourceId"] as? String }, ["capture:cam", "zoom:11", "zoom:20", "zoom:30"],
                "every assigned zoom/capture source once, ordinal by wire id")
    expectEqual(commands[0]["dropoutPolicy"] as? String, "black", "capture takes a dropout policy now (MediaCore.cpp:1907)")
    expectEqual(commands[1]["dropoutPolicy"] as? String, "hold", "an explicit hold is sent")
    expect(commands[3]["dropoutPolicy"] == nil, "no operator choice: no policy (the core default applies)")
    expectEqual(commands.compactMap { $0["displayName"] as? String }, ["name:Cam", "name:A", "name:B", "name:C"],
                "the resolved name rides every command (the failed-source slate name)")
    expect(commands.allSatisfy { $0["type"] as? String == "set-source-policy" }, "the core's command name")
}

static func testCaptureDropoutStatusUsesTheWindowsWording() {
    func row(_ liveness: String?, _ onAir: String?, policy: String = "hold", reason: String? = nil,
             failIn: Double? = nil, age: Double? = nil, applies: Bool? = true) -> BusSourceRow {
        BusSourceRow(sourceId: "capture:cam", kind: "capture", width: 0, height: 0, framesIngested: 0,
                     sinceLastNewFrameMs: nil, liveness: liveness, onAir: onAir, lossReason: reason,
                     failInMs: failIn, lastSampleAgeMs: age, dropoutPolicy: policy, dropoutPolicyApplies: applies)
    }
    expectEqual(CaptureDropoutStatus.line(row("lost", "failed")), "Lost (signal loss) — slate on Program", "lost, failed")
    expectEqual(CaptureDropoutStatus.line(row("lost", "producing", reason: "device-unplugged", failIn: 3200)),
                "Lost (device unplugged) — holding, slate in 3.2 s", "lost, holding")
    expectEqual(CaptureDropoutStatus.line(row("lost", "producing", policy: "black", failIn: nil)),
                "Lost (signal loss) — Program black, slate in unknown", "lost, black, unknown time")
    expectEqual(CaptureDropoutStatus.line(row("stalled", "producing", age: 1800)),
                "No new frames for 1.8 s — holding last frame", "stalled")
    expectEqual(CaptureDropoutStatus.line(row("live", "producing")), "Live", "live")
    expectEqual(CaptureDropoutStatus.line(row("live", "producing", applies: false)), "", "policy does not apply: no line")
    expect(!CaptureDropoutStatus.applies(row("live", "producing", applies: false)), "and the control is disabled")
}

@MainActor
static func testACapturePolicyPersistsAndFollowsItsSource() {
    let model = AppModel()
    model.assignSlot(5, kind: "capture", sourceId: "cam1", name: "Cam")
    model.setDropoutPolicy(slotId: 5, policy: "black")
    expectEqual(model.slots[4].dropoutPolicy, "black", "a capture slot takes a policy")
    var saved = model.currentPrefs()
    saved.showInputSlots = [PersistedSlot(slotId: 2, kind: "capture", sourceId: "cam1", name: "Cam", inShow: true)]
    let after = AppModel(); after.applyRestoredPrefs(saved)
    expectEqual(after.slots[1].dropoutPolicy, "black", "it follows the camera to its new slot")
    expectEqual(after.slots[4].dropoutPolicy, "", "not the old slot number")
}

/// Port of StudioViewModel.SetParticipantProductionRole (:5020-5058).
static func testOneHolderPerRoleAndMeetingScoped() {
    let present = [RosterParticipant(id: "a", name: "Ana", hasVideo: true, muted: false, talking: false, assigned: true),
                   RosterParticipant(id: "b", name: "Ben", hasVideo: true, muted: false, talking: false, assigned: true)]
    var roles = ProductionRoles()
    expectEqual(roles.set("a", role: "host", present: present), "Ana is now Host", "assign")
    expectEqual(roles.set("b", role: "host", present: present), "Ben is now Host", "reassign")
    expectEqual(roles.byParticipant, ["b": "host"], "one holder per role: Ana loses Host")
    expect(roles.set("b", role: "host", present: present) == nil, "unchanged is a no-op")
    expect(roles.set("zed", role: "reader", present: present) == nil && roles.byParticipant["zed"] == nil,
           "an absent participant cannot take a role")
    expectEqual(roles.set("b", role: nil, present: present), "Ben role cleared", "clear")
    _ = roles.set("a", role: "reader", present: present); roles.clear()
    expect(roles.byParticipant.isEmpty, "roles end with the meeting")
    expectEqual(ProductionRoles.roles.map(\.value),
                ["host", "co-host", "reader", "panelist", "guest-1", "guest-2", "guest-3", "guest-4"],
                "the Windows role list")
}
```

**Mutations to run:**
- Keep the `slot.kind == "zoom"` filter in `sourcePolicyCommands`. Expect commands-zoom-and-capture to go red.
- Sort by bare `sourceId`. Expect commands-zoom-and-capture (order) to go red.
- Omit `displayName` when it equals `slot.name`. Expect commands-zoom-and-capture (names) to go red.
- Show "Live" when `dropoutPolicyApplies` is false. Expect capture-status-lines to go red.
- Restore dropout to `kind == "zoom"` slots. Expect capture-policy-persists to go red.
- Drop the previous-holder removal. Expect one-holder-per-role to go red.

**Steps:** tests and the rewrite → red → implement → green → mutations → commit `mac sources S5: capture dropout with status, meeting-scoped roles (hidden), set-source-policy carries names`.

---

### Task 10: The inspector, with the Key moved in and a reserved Grade section (S5)

**Lane:** shell. **Slice:** S5 (inspector view, identity guard, Key, Grade slot).

**Files:**
- Create: `SourceInspector.swift` (`SourceInspector`, `SourceInspectorGradeSlot`, `InspectorBinding`, `SourceAssignmentHint`)
- Modify: `SourcesTable.swift` (inspector placement; remove the Task 7 interim "…" items Rename/Microphone/Key/On dropout; add the KEY chip), `ShellApp.swift` (delete `ChromaKeyControl` once its fields live in the inspector), `AppModel.swift` (chroma-key setters; `RosterParticipant.persistentId`; `sourceGradeContext`), `TabPageLayout.swift` (seed opens the inspector on slot 3 for one snapshot pair)
- Modify: `ShellTestsSources.swift`

**Interfaces:**

```swift
// The grading plan's contract (grading spec §5.8, slice G4). G4 replaces ONLY the body of
// SourceInspectorGradeSlot with `GradeInspectorSection(context:)` and flips
// AppModel.gradeEditorAvailable. Nothing else in this file changes for G4.
struct SourceGradeContext: Equatable {
    let slotId: Int
    let sourceKey: String            // "zoom:<pid>" / "capture:<id>"
    let gradeSourceId: String        // grading spec §5.8 item 1: Zoom participant id, or "capture:<deviceId>"
    let displayName: String
    let persistentId: String?        // grading spec §5.8 item 2: Zoom only, from the roster's "persistentId"
}
extension AppModel {
    static var gradeEditorAvailable: Bool { false }   // G1/G4 turn it on; hides the row "Grade", toolbar
                                                       // "Grade selected…" and inspector header "Grade…" until then
    func sourceGradeContext(slotId: Int) -> SourceGradeContext?   // nil when unassigned
    func setChromaKey(slotId: Int, enabled: Bool)
    func setChromaKeyColor(slotId: Int, hex: String)
    func setChromaKeyParameter(slotId: Int, _ path: WritableKeyPath<ShowInputSlot, Double>, _ value: Double)
}
struct SourceInspectorGradeSlot: View { let context: SourceGradeContext? }   // reserved body (below)
struct InspectorBinding: Equatable {
    let openedFor: String?                            // the sourceKey when the editor opened
    func isStale(current: String?) -> Bool            // openedFor != current
    static let staleNotice = "Source assignment changed. Reopen these options for the current source."
}
enum SourceAssignmentHint {
    /// ShowInputSlotViewModel.cs:360-363, verbatim; "missing" = the slot is offline.
    static func text(assigned: Bool, missing: Bool, inShow: Bool) -> String
}
struct RosterParticipant { ...; var persistentId: String? = nil }   // parse "persistentId" (ZoomEngineRuntime.cpp:1443)
```

**Behaviour:**
1. **Placement** (`SourcesWorkspaceControl.cs:156-161`). Beside the table in a 340 pt column when `SourcesColumnLayout.inspectorBeside(pageWidth:)`, else stacked below. Open only while `sourcesWorkspace.columns.inspectorOpen` is true. Bound to `sourcesWorkspace.selectedSlotId`.
2. **Identity guard** (`Views/SourcePolicyFlyout.cs:34-42`). The inspector records `InspectorBinding(openedFor: slot.sourceKey)` when it opens or the selection changes. When the slot's `sourceKey` changes underneath it, every field disables and `InspectorBinding.staleNotice` shows, until the operator reselects the row or presses "Reopen".
3. **Header:** the large preview placeholder (320 × 180 pt, "Preview unavailable", D1), format and age from the row facts, and `SourceAssignmentHint.text`. The verbatim Windows strings:
   - unassigned: "Choose a source for this input."
   - missing: "Saved assignment retained. Reconnect this source or choose a replacement; another source is never substituted automatically."
   - hidden: "Assigned, but hidden from the show. Enable In show to include this input."
   - else: "Assigned and included in the show. Unassign clears only this input; the source remains available."
4. **Source tab:**
   - Display name with "Reset to source name" (Task 8 setters).
   - Lower-third text: name, a secondary-line mode picker ("Source default", "Custom text", "Hide secondary line", `SourceLowerThirdFlyout.cs:16`), and the custom text field.
   - Role, only when `AppModel.roleControlAvailable` (D4). It uses the Windows list, and is disabled when the guest is absent, with "Guest is unavailable. Role can be changed when they return; dropout policy is saved."
   - Dropout, for zoom and capture. Capture uses the `CaptureDropoutStatus.policies` labels and shows `CaptureDropoutStatus.line(row)`. It is disabled with `unavailableNotice` when `!applies(row)`.
   - "Unassign this input".
5. **Picture tab:**
   - **Key** (Mac-only, kept). The same fields `ChromaKeyControl` had: "Key this source", "Green screen" / "Blue screen" presets, and the Similarity / Smoothness / Spill sliders with `%.2f` labels. Each calls the new `setChromaKey*` setters, which write the slot and call `syncScenes()` exactly as `ChromaKeyControl` did (`ShellApp.swift:1655-1711`). Behaviour is unchanged: persisted by `sourceKey` (`ShellPrefs.swift:85-93`), sent only when enabled (`AppModel.swift:2518-2533`).
   - **Grade** (reserved), below Key. `SourceInspectorGradeSlot` draws a titled section "Grade" with one line, "Per-source colour grade arrives with the grading update." (spec §4.7). That is all.
6. **Audio tab:** the paired microphone menu (capture only, existing `pairAudio`), and "Gain, mute and processing live on the Audio page."
7. **Row:** the interim "…" items move here. The row keeps "Inspect", "Unassign" in "…", and a "KEY" chip (`.plexMono(9, .semibold)`, `Studio.accent`) when `slot.keyEnabled`, so the state stays visible without the inspector (spec §4.7).
8. **`sourceGradeContext(slotId:)`.**
   - zoom: `gradeSourceId` = the bare pid, `persistentId` = the roster entry's.
   - capture: `gradeSourceId` = `"capture:" + id`, `persistentId` nil.
   - `displayName` = `displayName(for:)`.

**Complete tests:**

```swift
("sources-inspector/identity-guard", testTheInspectorDisablesWhenItsSourceChanges),
("sources-inspector/assignment-hints", testAssignmentHintsUseTheWindowsCopy),
("sources-inspector/key-wire-unchanged", testTheKeyMovedIntoTheInspectorSendsTheSameNode),
("sources-inspector/grade-context-contract", testTheGradeSlotContextIsWhatTheGradingPlanNeeds),
```

```swift
static func testTheInspectorDisablesWhenItsSourceChanges() {
    let binding = InspectorBinding(openedFor: "capture:cam1")
    expect(!binding.isStale(current: "capture:cam1"), "same source: live")
    expect(binding.isStale(current: "capture:cam2"), "reassigned: stale")
    expect(binding.isStale(current: nil), "unassigned: stale")
    expectEqual(InspectorBinding.staleNotice,
                "Source assignment changed. Reopen these options for the current source.", "the spec §4.7 copy")
}

static func testAssignmentHintsUseTheWindowsCopy() {
    expectEqual(SourceAssignmentHint.text(assigned: false, missing: false, inShow: false),
                "Choose a source for this input.", "unassigned")
    expectEqual(SourceAssignmentHint.text(assigned: true, missing: true, inShow: true),
                "Saved assignment retained. Reconnect this source or choose a replacement; another source is never substituted automatically.",
                "missing outranks in-show")
    expectEqual(SourceAssignmentHint.text(assigned: true, missing: false, inShow: false),
                "Assigned, but hidden from the show. Enable In show to include this input.", "hidden")
    expectEqual(SourceAssignmentHint.text(assigned: true, missing: false, inShow: true),
                "Assigned and included in the show. Unassign clears only this input; the source remains available.", "in show")
}

/// The Mac-only key must keep working (prompt; spec S5 proof). FIXTURE: blue key, similarity
/// 0.55, the other two at the core defaults. The expected node is written out here, not built
/// by chromaKeyNode, so a setter that writes the wrong field is visible.
@MainActor
static func testTheKeyMovedIntoTheInspectorSendsTheSameNode() {
    let model = AppModel()
    model.assignSlot(3, kind: "capture", sourceId: "cam1", name: "Cam")
    let syncs = model.sceneSyncRequestCount
    model.setChromaKey(slotId: 3, enabled: true)
    model.setChromaKeyColor(slotId: 3, hex: "#0000ff")
    model.setChromaKeyParameter(slotId: 3, \.keySimilarity, 0.55)
    let expected: JSONObject = ["enabled": true, "keyR": 0.0, "keyG": 0.0, "keyB": 1.0,
                                "similarity": 0.55, "smoothness": 0.1, "spill": 0.2]
    let node = model.chromaKeyNode(for: model.slots[2])
    expect(node.map { NSDictionary(dictionary: $0).isEqual(to: expected) } ?? false, "the route's chromaKey node: \(node ?? [:])")
    expectEqual(model.sceneSyncRequestCount, syncs + 3, "each change re-syncs the scene, as ChromaKeyControl did")
    guard let after = relaunched(model) else { expect(false, "relaunch"); return }
    expectEqual(after.slots[2].keyColorHex, "#0000ff", "the key is still persisted by source")
    model.setChromaKey(slotId: 3, enabled: false)
    expect(model.chromaKeyNode(for: model.slots[2]) == nil, "off sends no node")
}

/// The interface G4 builds on (grading spec §5.8 items 1-2).
@MainActor
static func testTheGradeSlotContextIsWhatTheGradingPlanNeeds() {
    let model = AppModel()
    model.applyZoomForTesting(["meetingState": "in_meeting", "rosterEpoch": "1:1:g", "rosterRevision": NSNumber(value: 1),
                               "participants": [["userId": "42", "displayName": "Ana", "videoOn": true, "persistentId": "p-ana"]]])
    model.assignSlot(4, kind: "capture", sourceId: "cam1", name: "Cam")
    let zoom = model.sourceGradeContext(slotId: 1)
    expectEqual(zoom, SourceGradeContext(slotId: 1, sourceKey: "zoom:42", gradeSourceId: "42",
                                         displayName: "Ana", persistentId: "p-ana"), "a Zoom slot: bare id + persistentId")
    expectEqual(model.sourceGradeContext(slotId: 4)?.gradeSourceId, "capture:cam1", "a capture slot: capture:<id>")
    expect(model.sourceGradeContext(slotId: 4)?.persistentId == nil, "no persistentId for capture")
    expect(model.sourceGradeContext(slotId: 9) == nil, "unassigned: no context")
    expect(!AppModel.gradeEditorAvailable, "Grade actions stay hidden until the grading plan turns them on")
}
```

**Mutations to run:**
- `isStale` compares slot ids instead of source keys. Expect identity-guard to go red.
- Swap the missing and hidden hint precedence. Expect assignment-hints to go red.
- `setChromaKeyParameter` writes `keySmoothness` for every path. Expect key-wire-unchanged to go red.
- `setChromaKeyColor` skips `syncScenes()`. Expect key-wire-unchanged (sync count) to go red.
- `gradeSourceId` = `sourceKey` for Zoom. Expect grade-context-contract to go red.

**Proofs:**
- **S5 layout:** `COREVIDEO_SHELL_TAB_SNAPSHOT` Sources at 1360 (inspector stacked below) and 1920 (inspector beside, since the 1920 page ≥ 1500), with the inspector open on slot 3. Plus one with it closed. Then `COREVIDEO_SHELL_SNAPSHOT` at `COREVIDEO_SHELL_WINDOW=1920x1080` with the inspector open in the running app.
- **Chroma key pixels** (spec S5 proof: "a keyed capture source on Program before and after the move is identical"). Use the same prefs file with one keyed capture slot on Program. Content: a screen capture of a static solid-green window with a white square, keyed green. Record 5 s of Program with the pre-Task-10 build and again with the post-Task-10 build. Decode both: `ffmpeg -i <mp4> -vf signalstats -f null -` per-frame `YAVG/UAVG/VAVG`. Pass when every frame's means agree within ±1.0 between the two recordings, and the keyed recording differs from an unkeyed control recording by more than 10 in `UAVG` or `VAVG`. The control proves the key is actually on. The wire test above is the independent proof (the core receives byte-identical input). The recording is the pixel confirmation the spec asks for. `ImageRenderer` cannot prove IOSurface pixels.

**Steps:** tests → red → implement the inspector and setters → move the controls → green → mutations → snapshots and recording A/B → commit `mac sources S5: the inspector — names, lower-third text, dropout, role (hidden), Key moved in, Grade reserved`.

**Done when:** all green, mutations red, PNGs and the A/B numbers attached. `ChromaKeyControl` no longer exists, and no row control writes key fields directly. That ends S5 and Phase 1 of the Sources workstream.

---

## Integration and owner check

**Automated, end of each slice:** the shell harness (`3961` checks baseline, plus every case above, 0 failed), `bash scripts/lint-mac-design.sh`, and the snapshot PNGs listed per task.

**No `mac-show-drill.py` run is required in this phase.** No task touches the render or ingest path or any `native/` file. The only new core traffic is `list-capture-devices` (operator-triggered) and the `displayName` field on the existing `set-source-policy` batch. The 8-feed drill gates S6 (the source sheet), which is Phase 2. The master plan §7 "Mac-twin check for every Windows-only native module" does not apply: no native module is ported here.

**Fake-engine run (S1 proof from the spec).** Run the installed shell against the fake engine (`COREVIDEO_ZOOM_ENGINE_PATH=<native/build-release-core/corevideo-zoom-engine-fake>`, `COREVIDEO_FAKE_ENGINE_PARTICIPANTS=3`, the env `mac-show-drill.py` sets). Join; replace guest A with guest B through the row picker; wait three roster ticks (3 s at the 1 Hz `zoomTimer`). Read the shell log (`~/Library/Logs/CoreVideoPro-shell.log`). It must contain the operator's `slot-write … by=operator-assign` lines, and NO later line that writes A into any slot. Drive the replace through the control API `input.assign` if the Control plan's slice RC2 has landed. Otherwise this step folds into owner check 2, because the owner asked that the desktop not be taken over without being asked (CLAUDE.md test-interaction rule).

**Fault drill (spec risk "freeing Zoom slots could empty the wall during a core restart").** With 3 fake guests slotted, kill `corevideo-native`. While the supervisor respawns it, the log shows `ZoomRosterProjection` rejecting the retired epoch, and NO `by=roster-sync` freeing lines before the new process's first accepted roster. Whatever the new process's first accepted roster says is then applied by the Windows rule: if the meeting ended with the old engine, the slots free, exactly as on Windows. Record which happened.

**Owner live checks** (spec §6, the Phase 1 subset), in a real meeting with at least one camera and one screen capture, on the signed installed build:
1. (spec 2) Replace a guest by hand, then wait a minute. Nobody comes back by themselves.
2. (spec 3) Unassign a slot, then have someone join. They do not take that slot.
3. (spec 4) A guest turns their camera off, then on. The chip goes VIDEO OFF, then LIVE.
4. (spec 5) Arm an ISO, start and stop recording. The chip goes ARMED, REC, OFF. If Task 5 Step 0 found the writer unreported, the chip reads "—" throughout, and that native follow-up is the open item.
5. (spec 6) Quit and relaunch outside a meeting. Capture slots return with names, keys and microphones on the right sources. No Zoom slot returns (D2).
6. (new) Side by side with Windows: the table, the inspector, and the auto-assign toggle on both tabs.

Spec checks 1 and 7 (thumbnails, and Program unchanged while the Sources tab opens and closes) belong to S6 and are not run here.

## Risks carried from the spec

| Risk | Status in this plan |
|---|---|
| Capture liveness and ISO writer lifecycle may be unreported on Mac | Task 5 Step 0 measures it. The chip shows only observed state ("—"), and a native follow-up issue is filed. No native change is made here. |
| Freeing Zoom slots could empty the wall during a core restart | Task 1 rule 4 + `testARejectedRosterFreesNothing`, plus the fault drill above. |
| Role control sends a role nothing on Mac consumes | D4: hidden. Wire default unchanged. |
| Per-source lower-third text has a small consumer until Graphics | D8 toggle on Overlays. The same store Graphics will read. |
| Display-name migration could attach a name to the wrong source | Task 8 migrates by the record's own source key; tested with a mutation. |
| A ten-row table with live facts redraws too often | Per-slot `SlotFactsBox` (spec correction 7), the Task 5 test that another row is not invalidated, and the Task 7 Instruments pass. |
| NEW: a guest who drops out of the meeting briefly loses their slot's key, mic and dropout (D10) | Accepted for Phase 1. If the owner objects, a meeting-scoped stash keyed by `sourceKey` restores them on rejoin. That is a small follow-up task. |
| NEW: D6 departs from Windows (`isMe` filtered) | Owner to confirm. One line to remove in Task 1 if overridden. |
| NEW: tasks 1-10 all edit `AppModel.swift` (4746 lines) | Expect merge conflicts between parallel tasks. New logic goes in the new files; `AppModel.swift` gets call sites only, which keeps conflicts small. |

## Out of scope

- S4 (stills as slot sources; clips listed and gated) and S6 (the source sheet, thumbnails, tally borders, the drill gate). Phase 2.
- Everything inside the Grade section: the grading spec's G1-G7 own it. This plan defines `SourceGradeContext`, `SourceInspectorGradeSlot` and `gradeEditorAvailable`, and nothing else. **Coordination note for the grading plan:** lane order puts Sources S3 ahead of Grade G1. G1's "interim Grade button on the slot row" (grading spec §5.8, last paragraph) should therefore be the row action that Task 7 leaves hidden behind `gradeEditorAvailable`, not a button on the deleted `SlotRow`.
- Media clip playback, Browser / SRT / RTMP / NDI groups and catalogs, Zoom quality caps, the capture audio-sync nudge, control-surface Sources actions (Control plan RC2 binds to `assignSlot`, `unassignSlot`, `setDisplayName`, `toggleSlotInShow`, `setAutoAssignInputs` and the `ShowInputWriteScope` chaining defined here), Graphics columns, crop/position/scale, and drag-reorder on the table.
- Any change to Windows or to `native/`.
