# macOS Parity Batch Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.
>
> **Also required reading before ANY task:** `docs/superpowers/plan-authoring-rules.md`. Rule 4's corollary applies to this plan too: when a supplied test contradicts a reasonable implementation, the test is the FIRST suspect — say so in your report rather than bending the design to it.

**Goal:** Close the verified macOS behaviour gaps against the Windows shell — 1080P source parity, per-source dropout policy, the loud ISO-preflight warning — and land the corrected parity audit so future scoping reasons about the product that exists.

**Architecture:** All shell work is pure-policy-first: each behaviour lands as a pure Swift type (testable in the in-binary ShellTests suite) that `AppModel` consumes at its existing wire call sites (`pushSpine`, `pushScenes`, `toggleRecording`). The one core-contract-visible change (the 1080P kind flip) is isolated into its own task so a reviewer can approve the budget machinery while holding the flip for live acceptance.

**Tech Stack:** Swift (SwiftPM app, no XCTest — the in-binary `ShellTests` harness), the MediaCore stdio JSON-RPC bridge contract, one comment edit in `native/` (C++ header).

**Spec:** `docs/mac-parity-plan.md` — refreshed by Task 1 of this plan (the version on `origin/main` is dated 2026-08-07 and is wrong; the 2026-09-27 re-audit on branch `docs/mac-parity-2026-09-27` is the base text, and two of ITS claims are corrected below with evidence).

## Evidence base (verified 2026-10-09 against origin/main @ 47feb000)

Every claim a task relies on, with the source. Trust these over the old audit:

- **E1.** Windows caps raw video subscriptions at `DefaultMaxVideoSubscriptions = 10` (`native-shell/CoreVideoPro.MediaCore/Services/ZoomMediaSpinePayloadBuilder.cs:19`; the header comment records the 6→8→10 history and the 2026-08-09 Susan Cho frozen-tile incident that forced 10).
- **E2.** The budget ORDER is `ZoomSourceSetPolicy.Resolve` (same dir): Program routes → Program Tiles → Preview routes → Preview Tiles → multiview slots in slot order → ISO-armed → sticky Tiles audio. Camera-OFF sources keep audio but **never spend video budget**; only video is capped.
- **E3.** The 1080P concurrency cap is CORE-side and already reaches Mac: `native/src/modules/ZoomSubscriptionResolutionPolicy.h:94` — `kMaxConcurrentFullResolutionCameras = 8`, soak-proven 2026-09-13, granted **in payload order** per spine payload.
- **E4.** The core deliberately fences the Mac shell at 720P: `wantsFullResolution` returns true only for kind `"participant-video"`, and the header says verbatim *"the macOS shell sends kind 'video' with purpose 'program' for every assigned guest and must not be moved to N x 1080P"* (h:71-72). So the old audit's §5.1 ("a Mac operator can exceed the bandwidth ruling") is **wrong** — Mac is capped BELOW parity, not above budget.
- **E5.** In the real engine path (`native/src/modules/ZoomEngineRuntime.cpp:435-470`), `Budget::resolve` is called for every non-audio subscription **with no videoOn check** — a camera-off `participant-video` subscription burns a 1080P grant. The camera-on filter is therefore the SHELL's job (Windows does it in `ZoomSourceSetPolicy`).
- **E6.** For kind `"video"` the engine sourceUuid is `"video-<pid>-<purpose>"` (purpose is part of the identity → changing purpose churns the subscription); for `"participant-video"` it is `"participant-video-<pid>-camera"` (purpose-free, survives Preview→Program promotion). ZoomEngineRuntime.cpp:452-457.
- **E7.** Mac's `pushSpine` (mac-shell `AppModel.swift:1294-1360`) subscribes `assignedIds` — a **Set** (nondeterministic order), all `purpose: "program"`, kind `"video"`, no cap, no camera-on filter. The 10-slot patch bay bounds the count at 10 by coincidence.
- **E8.** A `hasVideo` flip on an already-assigned participant does NOT re-push the spine today: the roster-apply path (`AppModel.swift:846-874`) calls `recomputeFromSlots()` only when auto-assign placed someone (`assignedAny`). Harmless today (subscriptions ignore hasVideo); fatal once the budget filters on camera-on — the Susan Cho failure mode, mirrored.
- **E9.** Dropout policy core contract (`native/src/core/MediaCore.cpp:2252-2296`): command `set-source-policy`, `sourceId` must be `"zoom:<pid>"` for `dropoutPolicy` (a non-zoom id pushes a sticky scene warning EVERY sync — permanently degrades health), values exactly `"hold"` | `"black"`, PRESENT-OR-KEEP (an omitted key never resets a stored policy), and policies must come AFTER `load-scene-graph` in a batch (loadSceneGraph clears `sceneValidationWarnings_`).
- **E10.** Windows sends one `set-source-policy` per entry, **ordered by sourceId ordinal** for determinism, omitting null keys (`MediaCoreCommandBuilder.cs:412-439`). Default policy is `"hold"` (`ProductionModels.cs:231`).
- **E11.** Windows T3.7 (#470, commit e1223d3e): `IsoCapturePreflight.Describe(isoEnabled, zoomIsoSourceCount, rawMediaActive)` warns when ISOs are armed and capture is observed off — because ISO writers open LAZILY at their first frame, so a never-fed source leaves `framesWritten 0`, no file, and **no downstream warning is possible**. Round-2 ruling: *an unobserved capture state is not a warning.* Copy carries the count ("7 Zoom ISO sources") and the fix ("Turn Engine on").
- **E12.** Mac recording folder: computed absolutely at send time from `moviesDirectory` (`AppModel.swift:2334-2339`), displayed in `SettingsPane.swift:12-14`; nothing persisted, nothing relative. The Windows T2.8 bug class (persisted relative pref resolved against core CWD) cannot occur. Audit §5.2 **closes as non-gap**.
- **E13.** Mac has no Tiles wall surface (`mac-shell` "tiles" references are naming only). Audit item T3.5 (tiles colour pickers) is Windows UI over a surface Mac doesn't have → N/A-until-Tiles, not a gap.
- **E14.** ShellTests harness: tests are `private static func test...()` on `enum ShellTests` using `expect(_:_:)` / `expectEqual(_:_:_:)`, registered as `("group/name", testFunc)` in the `runAndExit()` table (`ShellTests.swift:771+`). Same module → internal access, no visibility changes needed.
- **E15.** ShellPrefs is permanently additive-safe ONLY if a new field is optional-decoded: add the property, a `CodingKeys` case, a `decodeIfPresent` line in `init(from:)`, AND a populated value in `roundTripSelfCheck()` (`ShellPrefs.swift`; the file's own comments explain the two silent-wipe hazards). The `prefs/round-trip` test enforces the last two.
- **E16.** `rawMediaActive` is observed shell-side from the zoom snapshot (`AppModel.swift:842`); `lastZoomSnapshot` retains the last non-empty snapshot, so `lastZoomSnapshot["rawMediaActive"] as? Bool` is `nil` exactly when no meeting snapshot has ever reported (the "unobserved" state of E11).

## Global Constraints

- Build: `cd mac-shell && swift build -c release` (clean build ≈ 8s on this machine).
- Shell suite: `COREVIDEO_SHELL_TESTS=1 mac-shell/.build/release/CoreVideoProShell` — must end `N tests passed`, zero failures.
- Self-check: `COREVIDEO_SHELL_SELFCHECK=1 mac-shell/.build/release/CoreVideoProShell` — runs redaction + prefs round-trip.
- CI gates that must stay green: `mac-shell-tests`, `mac-shell-design-lint`, `mac-show-drill`, `native-stub-macos`, `native-metal-macos`.
- UI code uses `Studio.*` colour tokens and `.grotesk(...)`/`.plexMono(...)` fonts ONLY (design lint enforces brand tokens).
- Dropout policy strings are exactly `"hold"` and `"black"`; command type exactly `"set-source-policy"`; zoom-only (E9).
- Video budget cap is exactly **10** (E1); 1080P concurrency cap stays **8** core-side (E3) — this plan never touches `kMaxConcurrentFullResolutionCameras`.
- One branch + PR per task, targeting `main`. House commit style: short imperative subject, body explains the WHY with evidence. End commit messages with `Co-Authored-By: Claude Fable 5 <noreply@anthropic.com>`.
- Stacked-PR pitfall (learned 2026-08-02): retarget child PRs to main BEFORE deleting parent branches; a squash-merge with `--delete-branch` auto-closes children unrecoverably.
- Each task's report must include the named mutation results (plan-authoring-rules rule 7) — run each mutation, confirm the named test reds, revert.

---

### Task 1: Land the corrected parity audit on main

The 2026-09-27 re-audit never merged; `origin/main` still carries the 2026-08-07 doc (~940 commits stale). Two of the re-audit's own claims are now wrong (E4, E12). This task is docs-only and unblocks every future scoping pass.

**Files:**
- Modify: `docs/mac-parity-plan.md` (base text: the version at commit `888c45ca` on branch `docs/mac-parity-2026-09-27`)

**Interfaces:**
- Consumes: the Evidence base above (E1-E13), verbatim.
- Produces: nothing code-facing; Tasks 2-5 cite the refreshed doc.

- [ ] **Step 1: Branch from origin/main**

```bash
git fetch origin && git switch -c docs/mac-parity-refresh origin/main
git checkout 888c45ca -- docs/mac-parity-plan.md   # start from the re-audit text
```

- [ ] **Step 2: Apply the corrections.** Edit `docs/mac-parity-plan.md`:
  - Retitle the audit date to 2026-10-09; note it supersedes both the 2026-08-07 doc and the unmerged 2026-09-27 branch.
  - **Rewrite §5.1**: the ceiling evolved 6→8→10 subscriptions (E1) plus a core-side 8-camera 1080P cap (E3). The Mac is fenced at 720P by the core's kind gate (E4) — the gap is **under-parity (no 1080P), not over-budget**. Record that camera-off subs burn budget in the real runtime (E5), which is why the shell owns the camera-on filter.
  - **Close §5.2** with E12 verbatim evidence (file:line).
  - **Reclassify T3.5** (tiles colour pickers) as N/A-until-Tiles with E13.
  - **Add a §8: the 2026-09-27 → 10-09 window**: 150 commits on main; `mac-shell` 2 (both #823 delivery diagnostics); `native/` 90 (reach Mac via the show drill); 15 Windows-shell-only fixes, naming the operator-visible ones — #610 stream-degradation surfacing, #507 transport-control stability, #733/#746 core-restart recovery + Zoom rejoin (Mac received its half via the #659/#661/#663 slices, which touched mac-shell), #538 RTMP-ingest Sources UI. Note all 7 open feature PRs (#837-#846) touch zero mac-shell files.
  - **Replace §7's suggested order** with this plan's task list and path.

- [ ] **Step 3: Commit and open the PR**

```bash
git add docs/mac-parity-plan.md
git commit -m "docs(parity): correct the audit — the Mac is fenced BELOW parity, not over budget"
gh pr create --title "docs(parity): 2026-10-09 re-audit — Mac is under-parity at 720P, §5.2 closes" \
  --body "Supersedes the unmerged docs/mac-parity-2026-09-27 branch. Evidence inline per claim."
```

- [ ] **Step 4: After merge, delete the superseded branch** `docs/mac-parity-2026-09-27` (local + origin).

---

### Task 2: ZoomSourceBudget — deterministic, capped, camera-on-filtered spine subscriptions

Pure policy + wiring. **Deliberately wire-conservative:** kind stays `"video"` and the wire purpose stays `"program"` for every entry, because purpose is part of the engine's subscription identity for kind `"video"` (E6) — emitting real purposes now would tear down warmed subscriptions on every bus move. `ZoomSourceBudget.Entry` still carries the real purpose; Task 3 activates it. What changes behaviourally in this task: subscription order becomes deterministic (budget order), camera-off participants stop holding video subscriptions, the count is capped at 10, and a `hasVideo` change re-pushes the spine (E8).

**Files:**
- Create: `mac-shell/Sources/CoreVideoProShell/ZoomSourceBudget.swift`
- Modify: `mac-shell/Sources/CoreVideoProShell/AppModel.swift` — `pushSpine()` (~1294), the roster-apply path (~846-874), `buildRoutes` stays as-is (it is reused, not duplicated)
- Test: `mac-shell/Sources/CoreVideoProShell/ShellTests.swift` (+ registry entries)

**Interfaces:**
- Consumes: `SceneRoute` (AppModel.swift:27) and its `.json` serialization; `ShowInputSlot`; `RosterParticipant.hasVideo`; `AppModel.buildRoutes(for:isProgram:)` — change `private` to internal (same file, drop the keyword) so the budget derivation reuses the ONE route source of truth instead of growing a parallel derivation that drifts.
- Produces (Tasks 3 depends on these exact names):
  - `struct ZoomSourceBudget` with `struct Entry: Equatable { let participantId: String; let purpose: String }`, `static let maxVideoSubscriptions = 10`, and
    `static func videoEntries(programRouted: [String], previewRouted: [String], multiviewAssigned: [String], isoArmed: [String], cameraOn: Set<String>) -> [Entry]`
  - `static func routedZoomPids(_ routes: [JSONObject]) -> [String]` (on `ZoomSourceBudget`) — ordered, de-duplicated participantIds of fixed zoom routes.
  - `static func spineSubscriptionPayloads(_ entries: [Entry]) -> [JSONObject]` (on `AppModel`) — the exact objects placed in the spine `subscriptions` array.

**Behaviour prose:**
- `videoEntries` concatenates the four tiers in order (program, preview, multiview, iso — E2 scaled to Mac's model, which has no Tiles), skips any pid not in `cameraOn` (E2/E5 — camera-off spends nothing; since `cameraOn` derives from the roster, this also drops departed participants, so no "not in the Zoom SDK roster" warnings), de-duplicates keep-first (a pid's purpose is its highest tier), and truncates at `maxVideoSubscriptions`.
- In `pushSpine`, derive the tiers:
  - `programRouted` = `ZoomSourceBudget.routedZoomPids(buildRoutes(for: programSceneId, isProgram: true))` (empty sceneId → `buildRoutes` already returns `[]` for an unknown scene; guard `programSceneId.isEmpty` with `[]` to keep it explicit)
  - `previewRouted` = same for `previewSceneId`
  - `multiviewAssigned` = `slots.filter { $0.kind == "zoom" && $0.inShow && !$0.offline }.map(\.sourceId)` (slot order)
  - `isoArmed` = `isoRecordingEnabled ? slots.filter { $0.kind == "zoom" && $0.iso }.map(\.sourceId) : []`
  - `cameraOn` = `Set(roster.filter(\.hasVideo).map(\.id))`
  then `let subscriptions = Self.spineSubscriptionPayloads(ZoomSourceBudget.videoEntries(...))`, replacing the `assignedIds.enumerated().map` block.
- `spineSubscriptionPayloads` (this task): `["participantId": entry.participantId, "kind": "video", "purpose": "program", "priority": index]` — with a comment stating the fence (E4/E6) and that the 1080P flip task activates `entry.purpose`.
- `routedZoomPids`: from each route object take `"participantId" as? String` where present and non-empty; preserve order; de-duplicate keep-first. (An `active-speaker` layer has no participantId; a `capture-input` route has `captureDeviceId` instead — both contribute nothing.)
- **The re-sync trigger (E8):** in the roster-apply path, before `roster = RosterParticipant.parse(...)`, capture `let cameraOnBefore = Set(roster.filter(\.hasVideo).map(\.id))`; after the parse + auto-assign block, if `Set(roster.filter(\.hasVideo).map(\.id)) != cameraOnBefore` and `recomputeFromSlots()` was not already called (`!assignedAny`), call `syncSpine()`. Without this, a camera turned on after assignment never subscribes — the mirrored Susan Cho defect.

**Fixture invariants (rule 1):** every pid in an expected output appears in `cameraOn` unless the test is ABOUT camera-off; pids are unique per tier input except where the test exercises dedupe; the cap tests construct ≥ 11 camera-on candidates so truncation is reachable.

- [ ] **Step 1: Write the four failing tests** in `ShellTests.swift`, registered as `("budget/order", testBudgetProgramOutranksTheWall)`, `("budget/camera-off", testBudgetCameraOffSpendsNothing)`, `("budget/iso-dedupe-determinism", testBudgetIsoTierAndDeterminism)`, `("budget/route-shape", testRoutedZoomPidsReadTheRealRouteShape)`, `("budget/wire-shape", testSpineSubscriptionWireShape)`:

```swift
private static func testBudgetProgramOutranksTheWall() {
    // p-pgm holds a fixed Program route but sits LAST in the wall; order must
    // come from the tier, not the slot index. Also exercises keep-first dedupe:
    // p-pgm and p-cue appear in the wall tier too and must appear ONCE, at
    // their higher tier. Invariant: cameraOn covers every pid used.
    let entries = ZoomSourceBudget.videoEntries(
        programRouted: ["p-pgm"],
        previewRouted: ["p-cue"],
        multiviewAssigned: ["p-wall1", "p-wall2", "p-pgm", "p-cue"],
        isoArmed: [],
        cameraOn: ["p-pgm", "p-cue", "p-wall1", "p-wall2"])
    expectEqual(entries.map(\.participantId),
                ["p-pgm", "p-cue", "p-wall1", "p-wall2"],
                "budget order is program, preview, then the wall in slot order — once each")
    expectEqual(entries[0].purpose, "program", "a fixed program route takes the program purpose")
    expectEqual(entries[1].purpose, "preview", "a fixed preview route takes the preview purpose")
    expectEqual(entries[2].purpose, "multiview", "a wall-only source takes the multiview purpose")
}

private static func testBudgetCameraOffSpendsNothing() {
    // 11 camera-on wall sources w1..w11 (w10/w11 exist to make the cap
    // reachable) plus a camera-OFF program route ahead of all of them.
    // Windows rule (ZoomSourceSetPolicy): camera-off never spends VIDEO
    // budget. The cap is 10 — Windows DefaultMaxVideoSubscriptions, the
    // fixture-derived constant here.
    let wall = (1...11).map { "w\($0)" }
    let entries = ZoomSourceBudget.videoEntries(
        programRouted: ["p-off"],
        previewRouted: [],
        multiviewAssigned: wall,
        isoArmed: [],
        cameraOn: Set(wall))
    expectEqual(entries.count, 10, "the video budget is exactly 10 subscriptions")
    expect(!entries.contains { $0.participantId == "p-off" },
           "a camera-off source never spends video budget")
    expect(entries.contains { $0.participantId == "w10" },
           "the slot a camera-off source would have taken goes to the next candidate")
    expect(!entries.contains { $0.participantId == "w11" },
           "the 11th camera-on candidate is over budget")
    expectEqual(ZoomSourceBudget.maxVideoSubscriptions, 10,
                "CONFIRMATORY: documents the owner ruling; the count assertion above is the guard")
}

private static func testBudgetIsoTierAndDeterminism() {
    // An iso-armed-only pid enters LAST; an iso-armed pid already listed by a
    // higher tier is not repeated. Two identical calls agree — CONFIRMATORY
    // for a pure function; the real nondeterminism guard is budget/wire-shape
    // plus the pushSpine call site replacing the old Set-derived input.
    let first = ZoomSourceBudget.videoEntries(
        programRouted: ["a"], previewRouted: [], multiviewAssigned: ["b"],
        isoArmed: ["i1", "a"], cameraOn: ["a", "b", "i1"])
    expectEqual(first.map(\.participantId), ["a", "b", "i1"],
                "iso-armed sources enter after the wall, already-listed pids once only")
    expectEqual(first[2].purpose, "iso", "an iso-only source takes the iso purpose")
    let second = ZoomSourceBudget.videoEntries(
        programRouted: ["a"], previewRouted: [], multiviewAssigned: ["b"],
        isoArmed: ["i1", "a"], cameraOn: ["a", "b", "i1"])
    expect(first == second, "CONFIRMATORY: deterministic for identical inputs")
}

private static func testRoutedZoomPidsReadTheRealRouteShape() {
    // Drive the REAL SceneRoute serialization (rule 10: exercise the structure,
    // not a hand-copied dictionary): a fixed zoom route, an unbound
    // active-speaker layer and a capture route — only the zoom pid survives.
    let zoom = SceneRoute(routeId: "r1", mode: "fixed", participantId: "p9",
                          captureDeviceId: nil, rect: (0, 0, 1, 1), zIndex: 0)
    let speaker = SceneRoute(routeId: "r2", mode: "active-speaker", participantId: nil,
                             captureDeviceId: nil, rect: (0, 0, 1, 1), zIndex: 1)
    let capture = SceneRoute(routeId: "r3", mode: "capture-input", participantId: nil,
                             captureDeviceId: "cam-1", rect: (0, 0, 1, 1), zIndex: 2)
    let pids = ZoomSourceBudget.routedZoomPids([zoom.json, speaker.json, capture.json])
    expectEqual(pids, ["p9"], "only routes carrying a participantId contribute budget pids")
}

private static func testSpineSubscriptionWireShape() {
    let entries = [ZoomSourceBudget.Entry(participantId: "p1", purpose: "program"),
                   ZoomSourceBudget.Entry(participantId: "p2", purpose: "multiview")]
    let payloads = AppModel.spineSubscriptionPayloads(entries)
    expectEqual(payloads.count, 2, "one subscription per budget entry")
    expectEqual(payloads[0]["participantId"] as? String, "p1", "payload order is budget order")
    expectEqual(payloads[0]["priority"] as? Int, 0, "priority is the budget index")
    expectEqual(payloads[1]["priority"] as? Int, 1, "priority is the budget index")
    // The core's fence (ZoomSubscriptionResolutionPolicy.h): kind "video" is
    // never promoted to 1080P, and for kind "video" the purpose is part of the
    // engine's subscription identity — a real purpose here would churn warmed
    // subscriptions on every bus move. Both flip together in the 1080P task.
    expectEqual(payloads[1]["kind"] as? String, "video",
                "kind stays the fenced legacy kind until the 1080P flip task")
    expectEqual(payloads[1]["purpose"] as? String, "program",
                "the wire purpose stays constant until the kind flip makes identity purpose-free")
}
```

NOTE (rule 4 corollary): if `SceneRoute`'s memberwise init or `.json` keys differ from the fixture above, the FIXTURE is the suspect — read `AppModel.swift:27` and `buildRoutes` and fix the test to the real shape; do not restructure `SceneRoute`.

- [ ] **Step 2: Run the suite, verify the five new tests fail** (compile failure for missing types counts — add minimal stubs only if needed to see the assertions themselves fail).

- [ ] **Step 3: Implement** `ZoomSourceBudget.swift` (`videoEntries`, `routedZoomPids`, the doc comment citing E1-E5 by file:line), `AppModel.spineSubscriptionPayloads`, the `pushSpine` replacement, drop `private` from `buildRoutes`, and the roster-apply re-sync trigger with this comment on it: `// A camera turning ON must re-push the spine: the budget filters on camera-on, so without this the participant never re-subscribes (the Windows 2026-08-09 frozen-tile defect, mirrored).`

- [ ] **Step 4: Run the suite and self-check — all green.** `cd mac-shell && swift build -c release && COREVIDEO_SHELL_TESTS=1 .build/release/CoreVideoProShell && COREVIDEO_SHELL_SELFCHECK=1 .build/release/CoreVideoProShell`

- [ ] **Step 5: Run the named mutations (rule 7), confirm each reds, revert:**
  1. Remove the `cameraOn` filter in `videoEntries` → `budget/camera-off` reds (count 11 / p-off present).
  2. Remove keep-first dedupe → `budget/order` reds (p-pgm appears twice).
  3. Swap the multiview tier ahead of program → `budget/order` reds.
  4. Change the truncation to `maxVideoSubscriptions - 1` and to `+ 1` → `budget/camera-off` reds both ways.
  5. In `routedZoomPids`, also accept routes without a participantId → `budget/route-shape` reds.
  6. **Known unguarded call sites (rule 8, report them as such):** deleting the `syncSpine()` re-sync call in roster-apply, or reverting `pushSpine` to the old `assignedIds` Set, is NOT caught by this suite — both are AppModel async paths the in-binary harness cannot drive. They are covered by Step 6 and by Task 3's live acceptance.

- [ ] **Step 6: Manual smoke** — `scripts/run-mac-shell.sh`, join a test meeting if available (or verify via the stub core): Diagnostics shows subscriptions in budget order; toggling a camera off and on re-lists it within one spine sync.

- [ ] **Step 7: Commit + PR**

```bash
git add mac-shell/Sources/CoreVideoProShell/ZoomSourceBudget.swift mac-shell/Sources/CoreVideoProShell/AppModel.swift mac-shell/Sources/CoreVideoProShell/ShellTests.swift
git commit -m "feat(mac): budget-ordered, camera-on, capped spine subscriptions

The wire kind stays 'video' (the core's 720P fence) and the wire purpose
stays 'program' (purpose is subscription identity for kind 'video'); this
lands the ordering/filter/cap machinery the 1080P flip needs. Camera-off
sources no longer hold video subscriptions, so a hasVideo flip now re-pushes
the spine — without that, a camera turned on after assignment never
re-subscribes (the Windows 2026-08-09 frozen-tile defect, mirrored)."
```

---

### Task 3: The 1080P flip — kind `participant-video`, real purposes

One wire change + one core comment + live acceptance. Separated from Task 2 so a reviewer can hold THIS while approving the machinery. After this task, up to 8 Mac cameras are granted 1080P in budget order and overflow demotes to 720P stably (E3), published as `zoomSubscriptionChurn.fullResolutionDemoted`.

**Files:**
- Modify: `mac-shell/Sources/CoreVideoProShell/AppModel.swift` — `spineSubscriptionPayloads` only
- Modify: `native/src/modules/ZoomSubscriptionResolutionPolicy.h` — the two comment blocks naming the Mac shell's kind (around lines 71-72 and 98-103): rewrite to say the Mac shell now sends `participant-video` in budget order with a camera-on filter; kind `"video"` remains the un-promoted legacy kind for older shells. Comments only — **no constant, no logic**.
- Test: `mac-shell/Sources/CoreVideoProShell/ShellTests.swift` — edit `testSpineSubscriptionWireShape`

**Interfaces:**
- Consumes: `ZoomSourceBudget.Entry.purpose` (Task 2), `AppModel.spineSubscriptionPayloads` (Task 2).
- Produces: the final Mac spine wire shape: `kind: "participant-video"`, `purpose: entry.purpose`, priority = budget index.

**Behaviour prose:**
- `spineSubscriptionPayloads` emits `"kind": "participant-video"` and `"purpose": entry.purpose`. Identity becomes `"participant-video-<pid>-camera"` engine-side (E6), so purposes may now vary per entry without churn; the one-time resubscribe when this ships is expected and visible in churn metrics.
- Known cosmetic effect: the Diagnostics subscriptions rows (`AppModel.swift:733` defaults kind to `"video"`) will display `participant-video` — display-only, nothing in mac-shell branches on the kind string (verified: the `["audio","video"]` matches in `Lifecycle.generated.swift` are the recording-contract validator, unrelated to spine kinds).

- [ ] **Step 1: Edit the wire-shape test to the new contract** (same test, new expectations):

```swift
private static func testSpineSubscriptionWireShape() {
    let entries = [ZoomSourceBudget.Entry(participantId: "p1", purpose: "program"),
                   ZoomSourceBudget.Entry(participantId: "p2", purpose: "multiview")]
    let payloads = AppModel.spineSubscriptionPayloads(entries)
    expectEqual(payloads.count, 2, "one subscription per budget entry")
    expectEqual(payloads[0]["participantId"] as? String, "p1", "payload order is budget order")
    expectEqual(payloads[0]["priority"] as? Int, 0, "priority is the budget index")
    // participant-video is the tiered kind: the core grants 1080P to the first
    // 8 camera-on entries IN THIS ORDER (ZoomSubscriptionResolutionPolicy.h),
    // which is the entire point of the budget. Identity is purpose-free for
    // this kind, so the real purpose rides the wire without churn.
    expectEqual(payloads[0]["kind"] as? String, "participant-video",
                "the tiered kind — 1080P eligible, budget-order granted")
    expectEqual(payloads[0]["purpose"] as? String, "program", "the entry's real purpose")
    expectEqual(payloads[1]["purpose"] as? String, "multiview", "the entry's real purpose")
}
```

- [ ] **Step 2: Run suite — the edited test fails** (kind/purpose mismatch).
- [ ] **Step 3: Flip the two values in `spineSubscriptionPayloads`; update the two `ZoomSubscriptionResolutionPolicy.h` comment blocks.**
- [ ] **Step 4: Full verification** — mac-shell suite + self-check, AND the native suites the header touch rides with: configure/build the stub preset and run its tests (`native/` CMake stub config — comment-only change, but prove the build). CI's `mac-show-drill`/`native-metal-macos` cover the Metal config on push.
- [ ] **Step 5: Mutations (rule 7):**
  1. Revert kind to `"video"` → wire-shape test reds.
  2. Hardcode purpose `"program"` → wire-shape test reds (entry p2).
- [ ] **Step 6: LIVE ACCEPTANCE (required before this ships in a tagged beta — same pattern as the 2026-08-03 engine live test).** Owner meeting, ≥3 cameras: (a) Diagnostics shows `participant-video` subscriptions at 1920-wide delivery for camera-on sources; (b) `zoomSubscriptionChurn.fullResolutionDemoted` is 0 at ≤8 cameras; (c) toggle one camera off→on: tile resumes (Task 2's re-sync under the real engine); (d) sustained render holds frame rate with no monitor shedding (the 8×1080P Metal decode path has NOT been soak-proven on this rig — the 2026-09-13 soak was the Windows pipeline; if the Metal path shows distress, the fallback is reverting this task's one-line kind flip, which restores the 720P fence; the budget machinery stays).
- [ ] **Step 7: Commit + PR** (body cites E3-E6 and the live-acceptance result or its pending status).

---

### Task 4: Per-source dropout policy — surface, persistence, wire

The §4.1 gap: the core half shipped in #535 slice 4a and reaches Mac (E9); Mac lacks the operator surface and the persistence. Windows is moving this control into the input rows right now (PR #844) — the Mac surface is the matching place: the `SlotRow`.

**Files:**
- Modify: `mac-shell/Sources/CoreVideoProShell/AppModel.swift` — `ShowInputSlot` (+1 field), `pushScenes()` (append policies), new `setDropoutPolicy`, the prefs snapshot/restore (the same two blocks that handle `chromaKeys`, AppModel.swift:423-459 region)
- Modify: `mac-shell/Sources/CoreVideoProShell/ShellPrefs.swift` — `PersistedSlotPolicy`, prefs field + CodingKeys + `init(from:)` + `roundTripSelfCheck()` (all four, E15)
- Modify: `mac-shell/Sources/CoreVideoProShell/ShellApp.swift` — `SlotRow` (the menu)
- Test: `mac-shell/Sources/CoreVideoProShell/ShellTests.swift`

**Interfaces:**
- Consumes: core contract E9/E10; `ShowInputSlot`; `pushScenes()`'s `commands` array; the ShellPrefs additive-safety protocol (E15).
- Produces:
  - `ShowInputSlot.dropoutPolicy: String` — `""` = unset (core default "hold" applies); `"hold"`/`"black"` = operator-chosen, always sent.
  - `AppModel.setDropoutPolicy(slotId: Int, policy: String)` — validates membership in `["hold", "black"]`, writes the slot, calls `syncScenes()`.
  - `static func sourcePolicyCommands(slots: [ShowInputSlot]) -> [JSONObject]` (on `AppModel`)
  - `static func appendSourcePolicies(to commands: [JSONObject], slots: [ShowInputSlot]) -> [JSONObject]` (on `AppModel`)
  - `struct PersistedSlotPolicy: Codable, Equatable { var slotId = 0; var dropoutPolicy = "" }`; `ShellPrefs.slotDropoutPolicies: [PersistedSlotPolicy]?`

**Behaviour prose:**
- `sourcePolicyCommands`: zoom slots with non-empty `sourceId` and `dropoutPolicy` ∈ {hold, black} → `["type": "set-source-policy", "sourceId": "zoom:" + sourceId, "dropoutPolicy": policy]`, sorted by `sourceId` ordinal (E10), de-duplicated by sourceId keep-first (two slots holding the same participant must not race). An explicit `"hold"` IS sent — PRESENT-OR-KEEP means it is the only way to overwrite an earlier `"black"` (E9). Capture slots NEVER emit — a non-zoom id pushes a sticky warning every sync (E9); this is the expensive regression the test guards. `displayName` is deliberately not sent (Windows sends it for slate names; Mac's slate path is unverified — note it in the parity doc as a follow-up, don't ship it blind).
- `appendSourcePolicies` appends the policy commands AFTER whatever is already in the batch; `pushScenes` replaces its `guard !commands.isEmpty` flow with `let batch = Self.appendSourcePolicies(to: commands, slots: slots)` and sends `batch` when non-empty (policies alone, with no scene set, are still a valid batch — the ordering rule only matters when `load-scene-graph` is present).
- Persistence mirrors `chromaKeys`: snapshot saves `slots.filter { $0.kind == "zoom" && !$0.dropoutPolicy.isEmpty }`; restore applies by `slotId`. Slot-keyed like `CapturePairing`/`PersistedChromaKey` (zoom pids churn across meetings; the slot is the stable operator concept).
- UI: in `SlotRow`, zoom-assigned slots only, after `ChromaKeyControl` and before the ISO toggle: a borderless `Menu` labelled with the current policy ("Hold last frame" / "Black"), items calling `model.setDropoutPolicy(slotId:policy:)`, styled with the existing `Studio.field`/`Studio.border`/`.grotesk(11)` row idiom (design lint).

**Fixture invariants (rule 1):** sourceIds `"11"`/`"20"` are chosen so ordinal order ≠ slot order (slot 1 carries "20"); the capture slot carries a policy the UI can never set — the test is about the FILTER, not the UI.

- [ ] **Step 1: Write the failing tests**, registered as `("dropout/zoom-only-ordered", ...)`, `("dropout/after-scene-graph", ...)`:

```swift
private static func testSourcePolicyCommandsAreZoomOnlyAndOrdered() {
    // Core contract (MediaCore::setSourcePolicy, #535 slice 4a): only
    // "zoom:<pid>" may carry dropoutPolicy — a non-zoom id pushes a sticky
    // scene warning EVERY sync; values are exactly "hold"/"black"; an explicit
    // hold must be SENT (PRESENT-OR-KEEP: omission keeps the stored black).
    var zoomB = ShowInputSlot(id: 1); zoomB.kind = "zoom"; zoomB.sourceId = "20"
    zoomB.dropoutPolicy = "black"
    var zoomA = ShowInputSlot(id: 2); zoomA.kind = "zoom"; zoomA.sourceId = "11"
    zoomA.dropoutPolicy = "hold"
    var capture = ShowInputSlot(id: 3); capture.kind = "capture"; capture.sourceId = "cam"
    capture.dropoutPolicy = "black"   // unreachable via UI; the filter is the guard
    var unset = ShowInputSlot(id: 4); unset.kind = "zoom"; unset.sourceId = "30"
    _ = unset   // dropoutPolicy stays "" — must emit nothing
    let commands = AppModel.sourcePolicyCommands(slots: [zoomB, zoomA, capture, unset])
    expectEqual(commands.count, 2, "capture and policy-less slots emit nothing")
    expectEqual(commands[0]["sourceId"] as? String, "zoom:11", "ordinal order by sourceId")
    expectEqual(commands[0]["dropoutPolicy"] as? String, "hold",
                "an explicit hold is sent — it must overwrite an earlier black")
    expectEqual(commands[1]["sourceId"] as? String, "zoom:20", "ordinal order by sourceId")
    expectEqual(commands[1]["dropoutPolicy"] as? String, "black", "black rides verbatim")
    expectEqual(commands[0]["type"] as? String, "set-source-policy", "the core's command name")
}

private static func testSourcePoliciesFollowTheSceneGraphInTheBatch() {
    // MediaCore: loadSceneGraph clears sceneValidationWarnings_, so a policy
    // placed BEFORE load-scene-graph has any refusal warning silently wiped.
    // applyCommands runs in the order given — policies must come last.
    var slot = ShowInputSlot(id: 1); slot.kind = "zoom"; slot.sourceId = "7"
    slot.dropoutPolicy = "black"
    let batch = AppModel.appendSourcePolicies(
        to: [["type": "load-scene-graph"], ["type": "set-preview-scene"]],
        slots: [slot])
    expectEqual(batch.count, 3, "policies are appended, never interleaved")
    expectEqual(batch[0]["type"] as? String, "load-scene-graph", "scene commands keep position")
    expectEqual(batch[2]["type"] as? String, "set-source-policy",
                "the policy command comes after every scene command")
}
```

- [ ] **Step 2: Run — both fail.**
- [ ] **Step 3: Implement** the model field, the two statics, `setDropoutPolicy`, the `pushScenes` change.
- [ ] **Step 4: Persistence** — all four ShellPrefs touch points (E15): property, CodingKeys case, `decodeIfPresent` line, and `roundTripSelfCheck()` gains `p.slotDropoutPolicies = [PersistedSlotPolicy(slotId: 2, dropoutPolicy: "black")]`. Snapshot/restore in AppModel beside `chromaKeys`. The existing `prefs/round-trip` test is the guard — run it.
- [ ] **Step 5: UI** — the `SlotRow` menu per the prose. Build + eyeball via `scripts/run-mac-shell.sh` (assign a zoom slot, set Black, relaunch, confirm it restored).
- [ ] **Step 6: Suite + self-check green.**
- [ ] **Step 7: Mutations (rule 7):**
  1. Remove the zoom-only filter → `dropout/zoom-only-ordered` reds (count 3).
  2. Emit unset policies as `"dropoutPolicy": ""` → same test reds (count 3).
  3. Remove the ordinal sort → both sourceId assertions red.
  4. Insert policies at index 0 in `appendSourcePolicies` → `dropout/after-scene-graph` reds.
  5. Delete the `slotDropoutPolicies` line from `init(from:)` → `prefs/round-trip` reds (this is E15's existing guard doing its job — report it firing).
  6. **Known unguarded (rule 8):** `pushScenes` not calling `appendSourcePolicies`, and policy re-send after a core respawn, are not driveable from this suite. Verify the respawn case manually in Step 5's run if a core restart occurs, and list it in the report as residual.
- [ ] **Step 8: Commit + PR.**

---

### Task 5: ISO-armed-with-capture-off is loud (T3.7 mirror)

Windows shipped this as #502 (E11). The failure cannot be caught downstream — ISO writers open lazily at their first frame, so a never-fed source leaves no stream to carry a warning. Mac has the same lazy-open AVF writers and today says nothing.

**Files:**
- Modify: `mac-shell/Sources/CoreVideoProShell/RecordingCommandPolicy.swift` — add `enum IsoCapturePreflight`
- Modify: `mac-shell/Sources/CoreVideoProShell/AppModel.swift` — `toggleRecording()` start branch (after `isoLegacy` is computed, ~2345)
- Test: `mac-shell/Sources/CoreVideoProShell/ShellTests.swift`

**Interfaces:**
- Consumes: `isoLegacy` (the zoom-only ISO ids already computed in `toggleRecording`), `captureEnabled` (operator intent — the Capture button), `lastZoomSnapshot["rawMediaActive"] as? Bool` (observed state; `nil` = never reported, E16), `pushWarning(_:)`.
- Produces: `static func warning(zoomIsoCount: Int, captureIntended: Bool, rawMediaActive: Bool?) -> String?` on `IsoCapturePreflight`.

**Behaviour prose:** warn iff `zoomIsoCount > 0` AND (`captureIntended == false` OR `rawMediaActive == false`). `rawMediaActive == nil` with intent ON is unobserved → no warning (E11's round-2 ruling: warning on an unobserved state fires on every pre-join recording start). Intent OFF is itself a fact — the shell owns the toggle — so it warns regardless of observation; this is a deliberate, documented strengthening of the Windows helper, which only had the observed signal. Copy carries the count, correct plurality, and the fix: `"<N> Zoom ISO source[s] armed with Capture off — their ISO files will not start until frames flow. Turn Capture on, then record."` Call site: `if let warning = IsoCapturePreflight.warning(zoomIsoCount: isoLegacy.count, captureIntended: captureEnabled, rawMediaActive: lastZoomSnapshot["rawMediaActive"] as? Bool) { pushWarning(warning) }` — recording still starts (warn, never block: Program recording is unaffected, matching Windows).

- [ ] **Step 1: Write the failing tests**, registered as `("iso/preflight-loud", ...)`, `("iso/preflight-unobserved", ...)`:

```swift
private static func testIsoPreflightWarnsLoudly() {
    // The Windows T3.7 defect mirrored: 7 ISO streams sat at framesWritten 0,
    // no file, recording.warning null. The counts are the fixture constants;
    // the copy must carry the number and the fix.
    let seven = IsoCapturePreflight.warning(zoomIsoCount: 7, captureIntended: false,
                                            rawMediaActive: false)
    expect(seven != nil, "ISOs armed with capture off must warn")
    expect(seven?.contains("7 Zoom ISO sources") == true, "the warning names the count")
    expect(seven?.contains("Turn Capture on") == true, "the warning names the fix")
    let one = IsoCapturePreflight.warning(zoomIsoCount: 1, captureIntended: false,
                                          rawMediaActive: false)
    expect(one?.contains("1 Zoom ISO source ") == true, "singular copy for one source")
    expectEqual(IsoCapturePreflight.warning(zoomIsoCount: 0, captureIntended: false,
                                            rawMediaActive: false), nil,
                "no ISO sources, no warning")
    expectEqual(IsoCapturePreflight.warning(zoomIsoCount: 3, captureIntended: true,
                                            rawMediaActive: true), nil,
                "capture on and observed active is healthy")
    expect(IsoCapturePreflight.warning(zoomIsoCount: 3, captureIntended: true,
                                       rawMediaActive: false) != nil,
           "intent ON but observed OFF still warns — the observed state is the truth")
}

private static func testIsoPreflightNeverWarnsOnAnUnobservedState() {
    // Windows round-2 ruling: capture-off must be a FACT. Intent ON with no
    // snapshot ever reported (nil) is unobserved — warning here would fire on
    // every pre-join recording start.
    expectEqual(IsoCapturePreflight.warning(zoomIsoCount: 4, captureIntended: true,
                                            rawMediaActive: nil), nil,
                "an unobserved capture state is not a warning")
    expect(IsoCapturePreflight.warning(zoomIsoCount: 4, captureIntended: false,
                                       rawMediaActive: nil) != nil,
           "intent OFF is itself observed — the shell owns the Capture toggle")
}
```

- [ ] **Step 2: Run — both fail.**
- [ ] **Step 3: Implement the helper + the one call site.**
- [ ] **Step 4: Suite + self-check green.**
- [ ] **Step 5: Mutations (rule 7):**
  1. Treat `nil` rawMediaActive as `false` → `iso/preflight-unobserved` reds.
  2. Break plurality (always "sources") → singular assertion reds.
  3. Drop the count interpolation → count assertion reds.
  4. Ignore `captureIntended` → both the intent-OFF assertions red.
- [ ] **Step 6: Commit + PR.**

---

### Task 6: Close-out — doc truth + full gate run

**Files:**
- Modify: `docs/mac-parity-plan.md`

- [ ] **Step 1:** Mark §5.1 (budget + 1080P), §4.1 (dropout), T3.7 (ISO preflight) as SHIPPED with PR numbers; record Task 3's live-acceptance result (or its pending status, explicitly).
- [ ] **Step 2:** Full local gate run: mac-shell build + suite + self-check; confirm the latest `main` CI run is green across all five Mac jobs.
- [ ] **Step 3:** Commit + PR. Report done with the evidence (test counts, CI run id), not just the claim.

---

## Self-review (performed at authoring)

- **Spec coverage:** audit §5.1 → Tasks 2+3 (reframed per E4/E5); §5.2 → closed in Task 1 (E12); §4.1 dropout → Task 4; T3.7 → Task 5; T3.5 → N/A'd in Task 1 (E13); stranded audit → Task 1; post-9/27 window → recorded in Task 1 §8 (porting #610/#507/#538-class items is explicitly OUT of this plan — they are listed for the next scoping pass, not silently dropped).
- **Placeholder scan:** none — every test body is complete; the two "known unguarded" items are rule-8 disclosures, not deferred work.
- **Type consistency:** `ZoomSourceBudget.Entry(participantId:purpose:)`, `videoEntries(programRouted:previewRouted:multiviewAssigned:isoArmed:cameraOn:)`, `spineSubscriptionPayloads(_:)`, `sourcePolicyCommands(slots:)`, `appendSourcePolicies(to:slots:)`, `IsoCapturePreflight.warning(zoomIsoCount:captureIntended:rawMediaActive:)` are used with identical spellings across Tasks 2-5.
- **Rule 6 walk (carried obligations):** E4's "must not be moved to N x 1080P" is discharged by Task 2 building the budget FIRST and Task 3 updating that exact comment; E8's missing re-sync is discharged in Task 2 Step 3 with its hazard comment; E9's batch-ordering obligation is discharged by `appendSourcePolicies` + its test.
