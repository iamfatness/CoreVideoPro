# macOS per-source colour grading, Phase 1 (G0, G1, G2): implementation plan

Status: DRAFT for owner review, 2026-11-16.

> **For agentic workers:** REQUIRED SUB-SKILL: use superpowers:subagent-driven-development
> (recommended) or superpowers:executing-plans to carry this plan out task by task. Steps use
> checkbox (`- [ ]`) syntax. `docs/superpowers/plan-authoring-rules.md` is binding. Every task
> reports each named mutation and whether it went red. **When a supplied test contradicts a
> reasonable implementation, suspect the test first and say so in the report** (rule 4, corollary).

**Goal.** A Mac operator can grade one source without touching any other. They press Grade on a
source row, pick a built-in look, move four sliders, and only that source changes on Program and
in every output. The grade survives a core respawn, a Zoom stream change for the same person, and
a relaunch (capture sources). After G2 they also get the advanced primaries stack, undo and redo,
copy and paste, and `.cvgrade` presets that move between Mac and Windows unchanged.

**Spec:** `docs/superpowers/specs/2026-11-15-mac-source-grading-scopes-design.md`, slices G0, G1
and G2 (§6). Sequencing and rules: `docs/superpowers/plans/2026-11-15-mac-parity-master-plan.md`
(§2 Phase 1, §7).

**Architecture.** The shared core already grades per source on Metal: `SourceGradeState` holds an
accepted grade per source id, and `applyTo` stamps it on every plan layer for that source
(`native/src/core/SourceGradeState.h:50-60`, called at `MediaCore.cpp:4087,5249`). Metal shades each
layer with its own grade (`native/src/compositor/MetalCompositorAdapter.mm:1241-1246`). What the Mac
lacks is the identity needed to apply one, and a shell to drive it. G0 adds one read-only request
(`query-source-grade`) and one snapshot node (`sourceGrades`) to the shared core. G1 adds a Swift
`SourceGradeStore` as the only sender of `set-source-grade`, a per-source basic editor and a Grade
button on today's slot row. G2 adds the advanced document, its register-before-apply flow, undo,
copy and paste, and presets.

**Tech stack.** C++17 (shared core, GoogleTest via the vendored gtest: no `GTEST_SKIP`, skip by
`fprintf(stderr, …)` plus early return), Objective-C++ Metal (readback tests only), Swift 5.9 and
SwiftUI on macOS 13 (in-binary `ShellTests` harness), CryptoKit (SHA-256), C# xUnit (two small
cross-platform tests), Python 3 (one proof script and its unit test).

## Verified facts this plan is built on

Each was re-read in code on 2026-11-16. Line numbers are for branch `mac-tier0b` at `e98436fc`.

1. **Applying needs the source's epoch, and Metal never publishes it.** `SourceGradeState::apply`
   refuses `source-identity-changed` unless the request's `sourceEpoch` equals the epoch observed on
   that source's frame **in the last render tick** (`SourceGradeState.h:36-41`). `observe()` clears
   and rebuilds the epoch map on every tick (`:32-35`, called at `MediaCore.cpp:5816` inside
   `renderSyntheticTick`, which the production display tick runs, `MediaCore.cpp:5364`). Windows
   learns the epoch only from a `grade-preview` observation that carries a texture handle
   (`native-shell/CoreVideoPro.WinUI/ViewModels/ColorGradeEditorViewModel.cs:117-125`, ending at `:125`, calls
   `ObserveGradeAuthority` only after a non-zero `SharedHandleHex`), and refuses to apply before one
   (`ColorGradeEditorViewModel.Apply.cs:44`). On Metal `supportsGradePreview()` is false (default,
   `native/src/modules/Interfaces.h:1408`; only D3D11 overrides it), so every `grade-preview` event
   is `unavailable` with epoch 0 (`native/src/core/GradePreviewController.h:86`). The only other
   epoch emitter, `participant-shared-texture`, needs `participantSharedTextures`, which
   `MetalCompositorAdapter.mm` never fills (`ProgramFramePreview.cpp:830-839`; grep finds no
   writer in the Metal adapter). **So a Mac shell cannot apply a Zoom grade today. Confirmed.**
2. **Zoom epochs move often.** `ZoomEngineRuntime.cpp:1577-1582` mints a new epoch whenever a
   stream's width, height, participant or source generation changes, which happens several times
   in a join ramp and on every resolution re-subscribe. An applied grade is then painted
   **neutral** (`SourceGradeState.h:52,58`) until someone re-applies with the new epoch.
3. **Mac capture frames carry epoch 0** (no `sourceEpoch` in `AvfCaptureDeviceAdapter.mm`), and are
   keyed `capture:<outputSourceId>` (`AvfCaptureDeviceAdapter.mm:226-227`). Epoch 0 still has to be
   **present this tick**: a capture source with no frame is refused like any other.
4. **The frame key is the grade key.** Zoom frames carry the raw participant id (CLAUDE.md "Slice
   1": `ZoomParticipantSource` keys by the raw id, deliberately not `zoom:<pid>`). Routes bind
   `participantId` to the raw id and capture-input routes to `capture:<deviceId>`
   (`native/src/core/RouteSourcePolicy.h:32-39`), and `applyTo` matches
   `layer.participantId`, else `layer.sourceId` (`SourceGradeState.h:54`). The Mac slot model's
   `sourceKey` is `zoom:<pid>` (`mac-shell/Sources/CoreVideoProShell/ShowInputPersistence.swift:17-24`),
   which is **not** the grade key for Zoom. Windows uses the raw id
   (`StudioViewModel.Grading.cs:174-177`).
5. **A per-layer grade replaces the Program look, it does not stack**
   (`MetalCompositorAdapter.mm:1241`). A neutral per-layer grade therefore also removes that layer
   from the Program look. `SourceGradeState` has no way to forget a grade.
6. **Requests read the root, not a payload.** `set-source-grade` and `register-grade-document` read
   fields off the request root (`native/src/rpc/JsonRpcServer.cpp:278-293`; `MediaCore.cpp:1242-1255`).
   `success(id, obj)` merges `obj` into `{ok:true}`; `failure` answers
   `{ok:false, error:{code, message}}` (`JsonRpcServer.h:46-47`; asserted at
   `native/tests/JsonRpcServerTest.cpp:987`). The Swift bridge throws `BridgeError.remote(message)`
   on `ok:false` (`MediaCoreBridge.swift:281-283`).
7. **Every new sessionState field must be declared.** `contracts/observation.mjs:13-47` reads the
   `state.emplace("…")` calls in `CoreStateCapture.cpp` and throws unless
   `contracts/observation.schema.json` declares the field with `typed`, `qualification` and
   `control` policies; `npm run contract:generate` then regenerates
   `native-shell/CoreVideoPro.MediaCore/Services/CoreObservationModel.g.cs`. A `typed: omit` field
   is never projected onto the Windows shell's typed sync path.
8. **The request manifest is checked both ways.** `kCoreRequestTypes` is a
   `std::array<std::string_view, 50>` (`native/src/core/Protocol.h:129`);
   `ContractParity.RequestManifestMatchesTheRpcDispatcher` compares it with every
   `hasType(request, "…")` in `JsonRpcServer.cpp`, and
   `ContractParity.EveryRequestTheShellsSendIsHandledByTheCore` scans every `"type": "…"` literal in
   `mac-shell/Sources/CoreVideoProShell/*.swift` except `ShellTests.swift`
   (`native/tests/ContractParityTest.cpp:267-303`). **So no Swift file may contain
   `"type": "query-source-grade"` before the core handles it.**
9. **The Mac re-assert hook is `AppModel.onConnected()`** (`mac-shell/Sources/CoreVideoProShell/AppModel.swift:1474`),
   run on every core generation; the Program look is re-pushed inside it at `:1496-1498`.
   Windows clears its revision map on core recovery (`StudioViewModel.Grading.cs:34-36`) and
   carries grades across a respawn on scene routes (`:213-243`).
10. **The Zoom roster on the wire carries `persistentId` and `sourceGeneration`**
    (`native/src/modules/ZoomEngineRuntime.cpp:1441-1444`); a participant keeps its generation
    only while its persistentId is unchanged or empty (`ZoomEngineState.cpp:114-121`).
    `RosterParticipant.parse` reads neither today (`AppModel.swift:308-322`).

## Defaults taken (owner may override)

| # | Default | Source | Tasks that change on override |
|---|---|---|---|
| D1 | Keep the programme-wide window, relabelled "Program Look" with the line "Sources without their own grade." | spec §9 D1 | Task 6 (relabel step; on "remove", delete `ColorGradePane`, its window, the `ActionChip("Grade")` at `ShellApp.swift:791` and the `colorgrade` popout alias) |
| D2 | After a relaunch, capture grades re-apply automatically once frames arrive; Zoom grades return only as a draft with the Windows notice. | spec §9 D2 | Task 5 (`testRelaunchReappliesCaptureButZoomOnlyAsDraft`); Task 6 seed |
| D3 | Ship basic and advanced grading before the private preview, with the warning "No private preview on this Mac build yet. Applied edits go to air." | spec §9 D3 | Task 6 (warning line), Task 10 (whole task waits for G5 on override) |
| D4 | Re-apply automatically when `sourceGrades` reports `active:false` for the same person (persistentId), never for a different one. | spec §9 D4 | Task 5 (`testAnEpochChangeReassertsForTheSamePersonOnly`); on override the editor shows "Source identity changed…" instead |
| D5 | Within one meeting, a session grade follows its person to a new participant id when the old id has left the roster. Windows does not do this; it serves live owner check 2 ("the grade follows the person"). Session Zoom grades are dropped when the meeting ends. | this plan | Task 5 (`testGradeFollowsThePersonToANewParticipantId`) |
| D6 | **No `clear` in G0.** Reset applies a neutral grade, which the core keeps; that source then ignores the Program look until the next launch (fact 5). The store keeps the neutral session grade so a respawn stays consistent, and removes the saved entry so a relaunch returns the source to the Program look. The editor says so. Override: add an additive `clear: true` to `set-source-grade` in G0 (Tasks 1-2 grow by about half a day; Task 5's Reset sends it; the D6 copy goes). | this plan | Tasks 1, 2, 5, 6 |
| D7 | On `grade-revision-conflict` or `source-identity-changed`, the store re-queries once, re-checks the person, and retries once (spec §5.3). Windows keeps the draft and never retries (`AdvancedGradeEditorTests.cs:39`), because several Windows editors can hold one source. The Mac store is the only sender and allows one editor per source, so a conflict means its own bookkeeping went stale (a lost acknowledgement or a respawn race). | spec §5.3 | Task 5 |
| D8 | In G2 the Swift validator checks a `cube` operation's text is present and matches its SHA-256 and the 1.5 MB cap; full `.cube` parsing arrives with G3. The core still refuses a malformed cube at `register-grade-document`. | this plan | Task 9 (fixture skip list) |
| D9 | G2's adjustment list shows and preserves `curves` and `cube` operations (enable, intensity, reorder, duplicate, remove) but edits only primaries; the curve editor and LUT import are G3. | spec §6 | Task 10 |
| D10 | G0's commits merge to `main` only in the same PR as G1's store (Task 5) — the backlog rule that a foundation lands with its first real consumer. | CLAUDE.md "What to work on" | none |

## Before you start

- **Branch.** Create `mac-grading` from `mac-tier0b` in a new worktree
  `.claude/worktrees/mac-grading`. Commit per task; never push; never touch `main`. End commit
  messages with `Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>`.
- **Backlog.** Step 0 of Task 1: `gh issue list --label backlog --search "per-source grading"`.
  If none exists, create "macOS per-source colour grading (spec 2026-11-15, gap 2.5)" with label
  `backlog` and a link to this plan; reference it in every commit body.
- **Windows behaviour is unchanged.** Shared `native/` edits in this plan are additive: one new
  request, one new snapshot node (`typed: omit`), additive methods on `SourceGradeState`. No
  existing response, refusal reason, or Windows typed field changes. Tasks 2 and 8 each carry a
  test that proves it.
- **Builds on this Mac** (per `2026-11-14-mac-parity-m0-fixes.md` Global Constraints):
  - Native: `export PATH="$HOME/Library/Python/3.9/bin:$PATH"`; configure once with
    `cmake -S native -B native/build-metal -G Ninja -DCMAKE_BUILD_TYPE=Release -DCOREVIDEO_STUB=OFF -DCOREVIDEO_ENABLE_DEV_ADAPTERS=ON -DCOREVIDEO_WITH_METAL=ON -DCOREVIDEO_WITH_AVF_ENCODER=ON -DCOREVIDEO_WITH_COREAUDIO=ON`,
    build `cmake --build native/build-metal -j 8`, focused tests
    `native/build-metal/corevideo-native-tests --gtest_filter=<Suite>.*`, full suite
    `ctest --test-dir native/build-metal -j 6`.
  - Stub (the Windows-shaped software renderer, required whenever shared code changes):
    `cmake -S native -B native/build-stub -G Ninja -DCMAKE_BUILD_TYPE=Release`, build, `ctest`.
  - Shell: `cd mac-shell && swift build -c release && COREVIDEO_SHELL_TESTS=1 ./.build/release/CoreVideoProShell`,
    plus `COREVIDEO_SHELL_SELFCHECK=1 ./.build/release/CoreVideoProShell` and
    `bash scripts/lint-mac-design.sh`.
  - Contracts: `node contracts/generate.mjs --check` (and `npm run contract:generate` after a schema edit).
  - C#: `export PATH="$HOME/.dotnet:$PATH"; dotnet test native-shell/CoreVideoPro.MediaCore.Tests`.
- **Never build into `native/build-release-*` or `dist/`.** After changing a widely included header
  (`SourceGradeState.h`, `MediaCore.h`), rebuild with `--clean-first` before believing any crash
  (CLAUDE.md, output-supervisor build gotcha).

**Baseline measured 2026-11-16 on this Mac (record yours next to it before Task 1):**

| Gate | Result |
|---|---|
| Shell harness | `3961 shell checks passed, 0 failed.` |
| Native `native/build-metal/corevideo-native-tests` (binary dated 2026-11-15 19:06; rebuild before trusting) | 1983 tests listed; `--gtest_filter='AdvancedGrade.*:ContractParity.*:MetalCompositor.*:GradePreview*'` → `59 tests passed, 0 failed.` |
| `node contracts/generate.mjs --check` | clean (exit 0, no output) |
| `dotnet test native-shell/CoreVideoPro.MediaCore.Tests` | 3184 total, 3153 passed, **23 failed**, 8 skipped. The 23 are pre-existing and unrelated (graphics asset cache, TLS/WebSocket, Windows drive paths, process sweep). Compare the failing **set**, not the count, after each task. |

---

## Task order and lanes

```
native:  T1 ──► T2 ──► T3
                 │      └────────────► (G1 proof needs T6)
                 ├────────► T7
shell:   T4 ─────┴► T5 ──► T6 ──────────► T9 ──► T10
native:  T8 (any time; T9 needs it)
```

- T1, T4 and T8 can start in parallel.
- T5 needs T2 (ContractParity: fact 8) and T4. T3 and T7 need T2. T6 needs T5.
- T9 needs T5 and T8. T10 needs T6 and T9.
- G0 = T1, T2. G1 = T3, T4, T5, T6, T7. G2 = T8, T9, T10.

---

### Task 1 (native, G0): `SourceGradeState` answers "what is this source's epoch and revision"

**Files:**
- Modify: `native/src/core/SourceGradeState.h`
- Create: `native/tests/SourceGradeIdentityTest.cpp` (NEW)
- Modify: `native/CMakeLists.txt` (add `tests/SourceGradeIdentityTest.cpp` after `tests/AdvancedGradeTest.cpp`, line 962)

**Interfaces** (additive public members of `corevideo::core::SourceGradeState`):

```cpp
struct Query {
  bool present = false;      // the last observe() saw a frame keyed sourceId
  uint64_t sourceEpoch = 0;  // that frame's epoch; 0 when !present
  int64_t revision = 0;      // the accepted grade's revision; 0 when none
};
[[nodiscard]] Query query(const std::string& sourceId) const;

struct Status {
  std::string sourceId;
  int64_t revision = 0;
  uint64_t appliedEpoch = 0;
  uint64_t liveEpoch = 0;     // 0 when !livePresent
  bool livePresent = false;   // the last observe() saw a frame for sourceId
  bool active = false;        // livePresent && liveEpoch == appliedEpoch
};
// One entry per ACCEPTED grade (never for an observed but ungraded source),
// in sourceId order (the std::map's order). At most 256 entries (grade-capacity).
[[nodiscard]] std::vector<Status> statuses() const;
```

**Behaviour.**
- Both are pure reads of the two existing maps (`epochs_`, `grades_`); no mutation, no new state.
- `query` answers from the **last** `observe()` call. Because `observe()` replaces the epoch map, a
  source absent from the last tick is `present:false` even if it was present earlier.
- `present` is decided by map membership, never by `sourceEpoch != 0`: Mac capture frames carry
  epoch 0 (fact 3).
- `active` must equal exactly the condition under which `applyTo` paints the accepted grade rather
  than neutral (`SourceGradeState.h:52,58`). A source with no frame this tick is never `active`,
  including a capture grade whose applied epoch is 0.

**Tests** (complete; `native/tests/SourceGradeIdentityTest.cpp`):

```cpp
#include "core/SourceGradeState.h"
#include <gtest/gtest.h>

using namespace corevideo;

namespace {
modules::VideoFrame frame(const std::string& id, uint64_t epoch) {
  modules::VideoFrame f; f.participantId = id; f.sourceEpoch = epoch; return f;
}
rpc::Json applyRequest(const std::string& id, double epoch, double expected) {
  return rpc::Json::Object{{"sourceId", id}, {"sourceEpoch", epoch}, {"expectedRevision", expected}};
}
modules::CompositorColorGrade exposure(float v) { modules::CompositorColorGrade g; g.exposure = v; return g; }
}  // namespace

TEST(SourceGradeIdentity, QueryReportsPresenceEpochAndRevisionFromTheLastObservation) {
  core::SourceGradeState state;
  auto q = state.query("16778240");
  EXPECT_FALSE(q.present); EXPECT_EQ(q.sourceEpoch, 0u); EXPECT_EQ(q.revision, 0);

  state.observe({frame("16778240", 7), frame("capture:cam", 0)});
  q = state.query("16778240");
  EXPECT_TRUE(q.present); EXPECT_EQ(q.sourceEpoch, 7u); EXPECT_EQ(q.revision, 0);

  ASSERT_TRUE(state.apply(applyRequest("16778240", 7, 0), exposure(3)).get("accepted")->asBool());
  EXPECT_EQ(state.query("16778240").revision, 1);

  // observe() REPLACES the epoch map: absent from this tick means not present.
  state.observe({frame("capture:cam", 0)});
  q = state.query("16778240");
  EXPECT_FALSE(q.present); EXPECT_EQ(q.sourceEpoch, 0u);
  EXPECT_EQ(q.revision, 1) << "the accepted revision survives a missing frame";

  // The answer is exactly what the next apply needs (the G0 contract).
  state.observe({frame("16778240", 8)});
  q = state.query("16778240");
  const auto accepted = state.apply(applyRequest("16778240", double(q.sourceEpoch), double(q.revision)), exposure(4));
  EXPECT_TRUE(accepted.get("accepted")->asBool());
  EXPECT_EQ(accepted.getNumber("revision"), 2);
}

TEST(SourceGradeIdentity, EpochZeroIsPresentNotAbsent) {
  core::SourceGradeState state;
  state.observe({frame("capture:cam", 0)});
  const auto q = state.query("capture:cam");
  EXPECT_TRUE(q.present) << "Mac capture frames carry epoch 0 (AvfCaptureDeviceAdapter sets none)";
  EXPECT_EQ(q.sourceEpoch, 0u);
}

TEST(SourceGradeIdentity, StatusesListOnlyAcceptedGradesAndReportActiveFromTheLiveEpoch) {
  core::SourceGradeState state;
  // Fixture invariants: three sources observed; grades accepted for "capture:b" FIRST and
  // "16778240" second, so insertion order differs from id order ('1' < 'c' in std::string order).
  state.observe({frame("16778240", 7), frame("capture:b", 0), frame("capture:ungraded", 0)});
  ASSERT_TRUE(state.apply(applyRequest("capture:b", 0, 0), exposure(1)).get("accepted")->asBool());
  ASSERT_TRUE(state.apply(applyRequest("16778240", 7, 0), exposure(2)).get("accepted")->asBool());

  auto s = state.statuses();
  ASSERT_EQ(s.size(), 2u) << "an observed but ungraded source is never listed";
  EXPECT_EQ(s[0].sourceId, "16778240");
  EXPECT_EQ(s[1].sourceId, "capture:b");
  EXPECT_EQ(s[0].revision, 1);
  EXPECT_EQ(s[0].appliedEpoch, 7u); EXPECT_EQ(s[0].liveEpoch, 7u);
  EXPECT_TRUE(s[0].livePresent); EXPECT_TRUE(s[0].active);

  // A Zoom size change: same participant, new epoch -> painted neutral -> inactive.
  state.observe({frame("16778240", 8), frame("capture:b", 0)});
  s = state.statuses();
  EXPECT_EQ(s[0].liveEpoch, 8u); EXPECT_TRUE(s[0].livePresent); EXPECT_FALSE(s[0].active);
  EXPECT_TRUE(s[1].active);

  // capture:b absent this tick: an applied epoch of 0 must not read active against a missing 0.
  state.observe({frame("16778240", 8)});
  s = state.statuses();
  EXPECT_FALSE(s[1].livePresent); EXPECT_EQ(s[1].liveEpoch, 0u); EXPECT_FALSE(s[1].active);
}

// Structural (rule 10): drives applyTo, the thing `active` claims to describe.
TEST(SourceGradeIdentity, ActiveAgreesWithWhatApplyToPaints) {
  core::SourceGradeState state;
  const auto live = frame("16778240", 7);
  state.observe({live});
  ASSERT_TRUE(state.apply(applyRequest("16778240", 7, 0), exposure(5)).get("accepted")->asBool());
  modules::CompositorRenderPlanLayer layer; layer.participantId = "16778240";
  modules::CompositorRenderPlan plan; plan.layers.push_back(layer);
  state.applyTo(plan, {live});
  EXPECT_TRUE(state.statuses()[0].active);
  EXPECT_EQ(plan.layers[0].colorGrade.exposure, 5.f);

  const auto moved = frame("16778240", 8);
  state.observe({moved});
  plan.layers[0] = layer;
  state.applyTo(plan, {moved});
  EXPECT_FALSE(state.statuses()[0].active);
  EXPECT_EQ(plan.layers[0].colorGrade.exposure, 0.f) << "inactive == painted neutral";
}
```

**Mutations to run** (each must go red; report the failing assertion):
1. `present = (epoch != 0)` instead of map membership → `EpochZeroIsPresentNotAbsent`.
2. `active = (appliedEpoch == liveEpoch)` ignoring `livePresent` → the final block of
   `StatusesListOnly…` (capture:b, applied 0, live 0, absent).
3. `statuses()` iterates `epochs_` instead of `grades_` → `ASSERT_EQ(s.size(), 2u)`.
4. `query().revision` hard-coded 0 → `EXPECT_EQ(state.query("16778240").revision, 1)`.
5. `statuses()` returns insertion order (e.g. collect into a vector as applied) → the
   `s[0].sourceId` check. (Only possible if the implementer adds an order structure; if the
   mutation cannot be expressed against `std::map`, say so.)

- [ ] Step 0: backlog issue (see "Before you start").
- [ ] Step 1: write the tests; build; confirm they fail to compile (no `query`/`statuses`).
- [ ] Step 2: implement; focused run green on `build-metal` and `build-stub`.
- [ ] Step 3: mutations; record results.
- [ ] Step 4: commit `core: SourceGradeState reports a source's live epoch, revision and grade activity (G0)`.

**Done when:** four tests green on both build configurations; five mutation results recorded.

---

### Task 2 (native, G0): `query-source-grade` and the `sourceGrades` snapshot node on the wire

**Files:**
- Modify: `native/src/rpc/JsonRpcServer.cpp` (handler beside `set-source-grade`, line 291)
- Modify: `native/src/core/Protocol.h` (`kCoreRequestTypes` 50 → 51, add `"query-source-grade"` after `"set-source-grade"`)
- Modify: `native/src/core/MediaCore.h`, `native/src/core/MediaCore.cpp` (one public method)
- Modify: `native/src/core/CoreStateCapture.h`, `native/src/core/CoreStateCapture.cpp` (capture member + `state.emplace`)
- Modify: `contracts/observation.schema.json` (new field)
- Regenerate: `native-shell/CoreVideoPro.MediaCore/Services/CoreObservationModel.g.cs` (`npm run contract:generate`)
- Create: `native/tests/SourceGradeTestSupport.h` (NEW, shared by Tasks 2 and 3)
- Modify: `native/tests/SourceGradeIdentityTest.cpp` (MediaCore-level cases)
- Create: `native-shell/CoreVideoPro.MediaCore.Tests/SourceGradesObservationTests.cs` (NEW)

**Interfaces.**

Wire, request (root-level, like `set-source-grade`):

```json
{"id": "…", "type": "query-source-grade", "sourceId": "16778240"}
```

Wire, response: `{"ok": true, "id": "…", "present": true, "sourceEpoch": 7, "revision": 1}`. A
missing, non-string, empty, or longer-than-256-byte `sourceId` answers
`{"ok": false, "error": {"code": "protocol-error", "message": "query-source-grade requires a sourceId of 1-256 bytes."}}`.
The handler must not contain the phrase "requires a payload" (the Swift payload-contract test
`wire/contract-vs-core` parses that phrase out of this file).

C++:

```cpp
// MediaCore (public). Under coreMutex like every handle() request.
[[nodiscard]] rpc::Json querySourceGrade(const std::string& sourceId) const;  // {present, sourceEpoch, revision}
```

Snapshot node, published **unconditionally** (an empty array when no grade is accepted; the
multiviewer-node rule in CLAUDE.md):

```json
"sourceGrades": [
  {"sourceId": "16778240", "revision": 2, "appliedEpoch": 7, "liveEpoch": 8, "livePresent": true, "active": false}
]
```

`contracts/observation.schema.json`:

```json
"sourceGrades": { "typed": "omit", "qualification": "include", "control": "include" }
```

Test support (`native/tests/SourceGradeTestSupport.h`, namespace `corevideo::gradetest`):

```cpp
// One solid-colour BGRA bus source with a settable epoch and presence.
// kind MUST be "test": a "capture"-kind bus source would be removed by
// syncCaptureSources (core/CaptureBusRoster.h), because no capture adapter emits it.
class EpochSource final : public core::ISource {
 public:
  EpochSource(std::string sourceId, uint8_t r, uint8_t g, uint8_t b, uint64_t epoch,
              int width = 64, int height = 36);
  void setEpoch(uint64_t epoch);   // thread-safe (atomic)
  void setPresent(bool present);   // false: poll() returns an empty tick
  const core::SourceDescriptor& descriptor() const override;
  core::SourceTick poll(int64_t programTime100ns) override;
  core::SourceIngestCounters counters() const override;
};

// Wraps a real compositor, reports a non-"software" renderer so set-source-grade
// is admitted (MediaCore.cpp:1243), and keeps the last Program plan it was handed.
class GradeTestCompositor final : public modules::ICompositor {
 public:
  explicit GradeTestCompositor(std::unique_ptr<modules::ICompositor> inner);
  std::string rendererName() const override;            // "grade-test"
  modules::ProgramFrame render(const modules::CompositorRenderPlan&,
                               const std::vector<modules::VideoFrame>&) override;
  modules::CompositorRenderPlan lastPlan;
};
```

`EpochSource::poll` behaviour: when present, one frame with `participantId = sourceId`,
`width/pixelWidth/naturalWidth = width`, same for height, `pixelStride = width*4`, a shared BGRA
buffer filled with `(b, g, r, 0xff)`, `frameId` incremented per poll, `sourceEpoch` = current
epoch, health `Producing`; counters mirror `TestPatternSource` (`native/src/core/TestPatternSource.h:24-37`).

**Behaviour.**
- `querySourceGrade` delegates to `sourceGrades_.query()`. It is renderer-independent: the stub
  (software) build answers it too, while `set-source-grade` there still answers
  `native-source-grade-not-built` (unchanged; `GradePreviewRpc.StubRoundTrips…` keeps proving it).
- The capture step copies `sourceGrades_.statuses()` into a `CoreStateCapture` member under
  `coreMutex` (the existing pattern at `CoreStateCapture.cpp:673-677`), and `sessionState`
  emplaces the node next to `gradePreview` (`:941`), outside the lock. At most 256 small entries;
  never on the render tick.
- Epochs are written as JSON numbers (doubles). Accepted epochs are already bounded to 2^53 - 1 by
  `apply` (`SourceGradeState.h:38-39`), so no precision is lost.

**Tests** (append to `native/tests/SourceGradeIdentityTest.cpp`; these run on the stub and the
Metal build alike):

```cpp
#include "core/MediaCore.h"
#include "rpc/JsonRpcServer.h"
#include "SourceGradeTestSupport.h"

namespace {
struct Rig {
  modules::ModuleSet modules = modules::createStubModules();
  gradetest::GradeTestCompositor* compositor = nullptr;
  std::unique_ptr<core::MediaCore> core;
  std::unique_ptr<rpc::JsonRpcServer> server;
  Rig() {
    auto wrapped = std::make_unique<gradetest::GradeTestCompositor>(std::move(modules.compositor));
    compositor = wrapped.get();
    modules.compositor = std::move(wrapped);
    core = std::make_unique<core::MediaCore>(std::move(modules));
    core->useZoomSourcesForTest({});
    server = std::make_unique<rpc::JsonRpcServer>(*core);
  }
  rpc::Json query(const std::string& id) {
    return server->handle(rpc::Json::Object{{"id", "q"}, {"type", "query-source-grade"}, {"sourceId", id}});
  }
  rpc::Json set(const std::string& id, double epoch, double expected, double exposure) {
    return server->handle(rpc::Json::Object{{"id", "s"}, {"type", "set-source-grade"}, {"sourceId", id},
        {"sourceEpoch", epoch}, {"expectedRevision", expected},
        {"grade", rpc::Json::Object{{"lut", "none"}, {"exposure", exposure}}}});
  }
  void routeSideBySide() {
    (void)core->applyCommands(rpc::Json::Array{rpc::Json::Object{{"type", "load-scene-graph"}, {"sceneId", "pair"},
        {"routes", rpc::Json::Array{
            rpc::Json::Object{{"routeId", "a"}, {"mode", "capture-input"}, {"captureDeviceId", "cam-a"}, {"fitMode", "fill"},
                              {"rect", rpc::Json::Object{{"x", 0}, {"y", 0}, {"width", 0.5}, {"height", 1}}}},
            rpc::Json::Object{{"routeId", "b"}, {"mode", "capture-input"}, {"captureDeviceId", "cam-b"}, {"fitMode", "fill"},
                              {"rect", rpc::Json::Object{{"x", 0.5}, {"y", 0}, {"width", 0.5}, {"height", 1}}}}}}}});
  }
  const modules::CompositorRenderPlanLayer* layer(const std::string& participantId) const {
    for (const auto& l : compositor->lastPlan.layers) if (l.participantId == participantId) return &l;
    return nullptr;
  }
};
const rpc::Json* gradeRow(const rpc::Json& state, const std::string& id) {
  const auto* rows = state.get("sourceGrades");
  if (!rows || !rows->isArray()) return nullptr;
  for (const auto& row : rows->asArray()) if (row.getString("sourceId") == id) return &row;
  return nullptr;
}
}  // namespace

TEST(SourceGradeWire, QueryAnswersFromTheLiveTickAndRefusesAMalformedId) {
  Rig rig;
  auto cam = std::make_shared<gradetest::EpochSource>("capture:cam-a", 192, 96, 64, 7);
  rig.core->addSourceForTest(cam);

  auto r = rig.query("capture:cam-a");
  ASSERT_TRUE(r.get("ok")->asBool());
  EXPECT_FALSE(r.get("present")->asBool()) << "no tick has observed the source yet";
  EXPECT_EQ(r.getNumber("sourceEpoch"), 0); EXPECT_EQ(r.getNumber("revision"), 0);

  rig.core->renderDisplayTick();
  r = rig.query("capture:cam-a");
  EXPECT_TRUE(r.get("present")->asBool()); EXPECT_EQ(r.getNumber("sourceEpoch"), 7);

  cam->setPresent(false);
  rig.core->renderDisplayTick();
  EXPECT_FALSE(rig.query("capture:cam-a").get("present")->asBool()) << "absent this tick";

  for (const auto& bad : {rpc::Json(rpc::Json::Object{{"id", "b"}, {"type", "query-source-grade"}}),
                          rpc::Json(rpc::Json::Object{{"id", "b"}, {"type", "query-source-grade"}, {"sourceId", ""}}),
                          rpc::Json(rpc::Json::Object{{"id", "b"}, {"type", "query-source-grade"}, {"sourceId", std::string(257, 'x')}}),
                          rpc::Json(rpc::Json::Object{{"id", "b"}, {"type", "query-source-grade"}, {"sourceId", 5}}),
                          // root-level contract: a payload wrapper is NOT read
                          rpc::Json(rpc::Json::Object{{"id", "b"}, {"type", "query-source-grade"},
                                                      {"payload", rpc::Json::Object{{"sourceId", "capture:cam-a"}}}})}) {
    const auto refused = rig.server->handle(bad);
    EXPECT_FALSE(refused.get("ok")->asBool());
    EXPECT_EQ(refused.get("error")->getString("code"), "protocol-error");
  }
  EXPECT_TRUE(rig.query(std::string(256, 'x')).get("ok")->asBool()) << "256 bytes is the inclusive bound";
}

TEST(SourceGradeWire, SnapshotNodeIsUnconditionalAndTracksActivity) {
  Rig rig;
  auto a = std::make_shared<gradetest::EpochSource>("capture:cam-a", 192, 96, 64, 7);
  auto b = std::make_shared<gradetest::EpochSource>("capture:cam-b", 192, 96, 64, 11);
  rig.core->addSourceForTest(a); rig.core->addSourceForTest(b);
  rig.routeSideBySide();

  auto state = rig.core->sessionState();
  ASSERT_NE(state.get("sourceGrades"), nullptr) << "published even with no grade";
  EXPECT_TRUE(state.get("sourceGrades")->isArray());
  EXPECT_TRUE(state.get("sourceGrades")->asArray().empty());

  rig.core->renderDisplayTick();
  // Fixture invariant: both route layers exist and carry the frame keys.
  ASSERT_NE(rig.layer("capture:cam-a"), nullptr);
  ASSERT_NE(rig.layer("capture:cam-b"), nullptr);

  const auto accepted = rig.set("capture:cam-a", 7, 0, 2);
  ASSERT_TRUE(accepted.get("accepted")->asBool()) << accepted.stringify();
  rig.core->renderDisplayTick();
  state = rig.core->sessionState();
  const auto* row = gradeRow(state, "capture:cam-a");
  ASSERT_NE(row, nullptr);
  EXPECT_EQ(row->getNumber("revision"), 1);
  EXPECT_EQ(row->getNumber("appliedEpoch"), 7); EXPECT_EQ(row->getNumber("liveEpoch"), 7);
  EXPECT_TRUE(row->get("livePresent")->asBool()); EXPECT_TRUE(row->get("active")->asBool());
  EXPECT_EQ(gradeRow(state, "capture:cam-b"), nullptr) << "the ungraded neighbour is not listed";
  EXPECT_TRUE(rig.layer("capture:cam-a")->hasColorGrade);
  EXPECT_EQ(rig.layer("capture:cam-a")->colorGrade.exposure, 2.f);
  EXPECT_FALSE(rig.layer("capture:cam-b")->hasColorGrade) << "the neighbour keeps the Program look";

  a->setEpoch(8);
  rig.core->renderDisplayTick();
  state = rig.core->sessionState();
  row = gradeRow(state, "capture:cam-a");
  ASSERT_NE(row, nullptr);
  EXPECT_EQ(row->getNumber("liveEpoch"), 8);
  EXPECT_FALSE(row->get("active")->asBool());
  EXPECT_EQ(rig.layer("capture:cam-a")->colorGrade.exposure, 0.f) << "inactive is painted neutral";

  a->setPresent(false);
  rig.core->renderDisplayTick();
  state = rig.core->sessionState();
  row = gradeRow(state, "capture:cam-a");
  ASSERT_NE(row, nullptr) << "an accepted grade stays listed while its source is absent";
  EXPECT_FALSE(row->get("livePresent")->asBool()); EXPECT_EQ(row->getNumber("liveEpoch"), 0);
}
```

Fixture invariants (state them in the test file header): sources are kind `"test"`; the routes
use `capture-input` so `RouteSourcePolicy` binds `participantId = "capture:cam-a"`; if the
implementer finds `capture-input` routes are gated on a connected device in the stub, switch both
routes to `{"mode": "fixed", "participantId": "capture:cam-…"}` (participantId still matches the
frame key) and say so in the report. Do not change the source ids.

C# (`native-shell/CoreVideoPro.MediaCore.Tests/SourceGradesObservationTests.cs`, proves the
Windows typed sync path is unchanged):

```csharp
using CoreVideoPro.MediaCore.Services;
using Xunit;
namespace CoreVideoPro.MediaCore.Tests;
public sealed class SourceGradesObservationTests
{
    private const string Session = "{\"sourceGrades\":[{\"sourceId\":\"16778240\",\"revision\":2,\"appliedEpoch\":7,\"liveEpoch\":8,\"livePresent\":true,\"active\":false}],\"activeSpeakerId\":\"42\"}";
    [Fact] public void SourceGradesNeverReachesTheTypedViewButReachesQualificationAndControl()
    {
        var model = CoreObservationModel.Parse(Session);
        Assert.DoesNotContain("sourceGrades", model.TypedJson());
        Assert.Contains("\"sourceGrades\"", model.QualificationJson());
        Assert.Contains("\"sourceGrades\"", model.ControlJson());
        Assert.Contains("\"activeSpeakerId\"", model.TypedJson()); // confirmatory: the typed view still works
    }
}
```

**Mutations to run:**
1. Emit the node only when non-empty → `SnapshotNodeIsUnconditional…` first block.
2. Handler reads `request.get("payload")->getString("sourceId")` → the wrapped-payload case and the
   valid query both red.
3. Remove `"query-source-grade"` from `kCoreRequestTypes` (size back to 50) →
   `ContractParity.RequestManifestMatchesTheRpcDispatcher`.
4. Omit the schema entry → `node contracts/generate.mjs --check` exits non-zero (structural; paste
   its message).
5. Set `"typed": "include"` → `contract:generate` throws "neither typed snapshot model binds it",
   and the C# test fails if forced through. This is the "Windows unchanged" guard.
6. Capture the statuses **after** releasing coreMutex (read `core.sourceGrades_` in `sessionState`)
   → not a test failure; reject in review as a data race (say whether TSan or any test caught it;
   expected: none, so the review is the guard).

- [ ] Step 1: tests first; confirm red (unknown request → `protocol-error` from the dispatcher's default; node missing).
- [ ] Step 2: implement; regenerate contracts; focused runs on `build-metal` and `build-stub`;
      `ContractParity.*` green; C# test green.
- [ ] Step 3: full `ctest` on both builds; C# failing set equals the baseline 23.
- [ ] Step 4: `python3 scripts/mac-show-drill.py --seconds 40 --load 8` on this Mac (the snapshot
      capture runs under `coreMutex`). Record fps, delivery, coreMutex over-budget, and compare with
      a baseline run of the same binary built from `mac-tier0b`.
- [ ] Step 5: mutations; commit `core: query-source-grade and the sourceGrades snapshot node (G0)`.

**Done when:** all of the above green; the drill passes with no regression against the baseline
run; the C# failing set is unchanged; the Windows dev suite runs in CI on the PR (it cannot run here).

---

### Task 3 (native, G1): a grade is a pixel claim (Metal readback)

**Files:**
- Modify: `native/tests/MetalCompositorTest.cpp` (two cases after line 278)
- Create: `native/tests/MetalSourceGradePixelTest.cpp` (NEW; whole file under `#if COREVIDEO_WITH_METAL`)
- Modify: `native/CMakeLists.txt` (add after `tests/MetalCompositorTest.cpp`, line 968)

**Interfaces.** None new. Uses `SourceGradeTestSupport.h` from Task 2.

**Expected values** (fixture-derived, rule 2). They come from the shader
(`native/src/compositor/MetalCompositorShaders.h:119-126`) with points scaled by 0.1 and look offsets
from `native/src/compositor/ColorGradeParams.h:15-32`, worked by hand for each patch:

| Patch (R,G,B) | Grade | Expected (R,G,B) | Distance from ungraded |
|---|---|---|---|
| 192,96,64 | none | 192,96,64 | 0 |
| 192,96,64 | exposure +2 | 243,147,115 | 51 |
| 192,96,64 | contrast +5 | 224,80,32 | 32 |
| 192,96,64 | saturation +5 | 227,83,35 | 35 |
| 192,96,64 | temperature +20 | 218,96,38 | 26 |
| 192,96,64 | look warm-film | 201,93,56 | 9 |
| 192,96,64 | look punch | 212,88,46 | 20 |
| 200,60,230 | look cool-broadcast | 201,57,236 | 6 (blue) |
| 250,128,6 | look neutral | 252,128,4 | 2 |

The neutral look moves any patch by at most 2.55 levels (contrast +0.02 times at most 0.5). It is
asserted at tolerance 1; every other row at tolerance 2. If the neutral row fails on this GPU by
rounding alone (observed value 251 or 5), do **not** widen it: report it, and the neutral look is
then covered only by `colorGradeParams` (a unit fact), not by pixels.

**Tests.** `MetalCompositorTest.cpp` additions (helpers `solidLayer`, `bgraFrame`, `previewAt`,
`MAKE_COMPOSITOR_OR_SKIP` already exist in that file):

```cpp
TEST(MetalCompositor, BasicGradeAxesAndNamedLooksMoveAKnownPatch) {
  MAKE_COMPOSITOR_OR_SKIP(compositor);
  struct Case { const char* name; uint8_t r, g, b; const char* lut; float e, c, s, t; int er, eg, eb, tol; };
  const Case cases[] = {
      {"identity",        192, 96, 64,  "none",            0, 0, 0, 0,  192, 96, 64,  2},
      {"exposure+2",      192, 96, 64,  "none",            2, 0, 0, 0,  243, 147, 115, 2},
      {"contrast+5",      192, 96, 64,  "none",            0, 5, 0, 0,  224, 80, 32,  2},
      {"saturation+5",    192, 96, 64,  "none",            0, 0, 5, 0,  227, 83, 35,  2},
      {"temperature+20",  192, 96, 64,  "none",            0, 0, 0, 20, 218, 96, 38,  2},
      {"warm-film",       192, 96, 64,  "warm-film",       0, 0, 0, 0,  201, 93, 56,  2},
      {"punch",           192, 96, 64,  "punch",           0, 0, 0, 0,  212, 88, 46,  2},
      {"cool-broadcast",  200, 60, 230, "cool-broadcast",  0, 0, 0, 0,  201, 57, 236, 2},
      {"neutral",         250, 128, 6,  "neutral",         0, 0, 0, 0,  252, 128, 4,  1},
  };
  for (const auto& c : cases) {
    CompositorRenderPlan plan;
    plan.renderPlanId = std::string("basic-") + c.name;
    plan.layers.push_back(solidLayer("cam", 0, 0, 1, 1));
    plan.layers[0].hasColorGrade = true;
    plan.layers[0].colorGrade = CompositorColorGrade{c.e, c.c, c.s, c.t, c.lut};
    const auto rendered = compositor->render(plan, {bgraFrame("cam", 320, 180, c.b, c.g, c.r)});
    ASSERT_TRUE(rendered.gpuComposed) << c.name;
    const auto px = previewAt(rendered.preview, .5f, .5f);   // B, G, R, A
    EXPECT_NEAR(px[2], c.er, c.tol) << c.name << " R";
    EXPECT_NEAR(px[1], c.eg, c.tol) << c.name << " G";
    EXPECT_NEAR(px[0], c.eb, c.tol) << c.name << " B";
  }
}

TEST(MetalCompositor, APerLayerGradeReplacesTheProgramLookOnItsLayerOnly) {
  MAKE_COMPOSITOR_OR_SKIP(compositor);
  CompositorRenderPlan plan;
  plan.renderPlanId = "per-layer";
  plan.layers.push_back(solidLayer("left", 0, 0, .5f, 1));
  plan.layers.push_back(solidLayer("right", .5f, 0, .5f, 1));
  const auto left = bgraFrame("left", 160, 180, 64, 96, 192);
  const auto right = bgraFrame("right", 160, 180, 64, 96, 192);

  // 1. A graded layer beside an ungraded one, no Program look.
  plan.layers[0].hasColorGrade = true;
  plan.layers[0].colorGrade = CompositorColorGrade{2, 0, 0, 0, "none"};
  auto out = compositor->render(plan, {left, right});
  auto l = previewAt(out.preview, .25f, .5f), r = previewAt(out.preview, .75f, .5f);
  EXPECT_NEAR(l[2], 243, 2); EXPECT_NEAR(l[1], 147, 2); EXPECT_NEAR(l[0], 115, 2);
  EXPECT_NEAR(r[2], 192, 2); EXPECT_NEAR(r[1], 96, 2);  EXPECT_NEAR(r[0], 64, 2);

  // 2. Program look on, left layer carries a NEUTRAL per-layer grade: left ignores the look
  //    (the D1/D6 copy depends on this), right takes it.
  plan.colorGrade = CompositorColorGrade{2, 0, 0, 0, "none"};
  plan.layers[0].colorGrade = CompositorColorGrade{};
  out = compositor->render(plan, {left, right});
  l = previewAt(out.preview, .25f, .5f); r = previewAt(out.preview, .75f, .5f);
  EXPECT_NEAR(l[2], 192, 2); EXPECT_NEAR(l[1], 96, 2);  EXPECT_NEAR(l[0], 64, 2);
  EXPECT_NEAR(r[2], 243, 2); EXPECT_NEAR(r[1], 147, 2); EXPECT_NEAR(r[0], 115, 2);
}
```

`native/tests/MetalSourceGradePixelTest.cpp` (end to end: request → `SourceGradeState` →
`applyTo` → Metal → Program pixels):

```cpp
#if COREVIDEO_WITH_METAL
#include <gtest/gtest.h>
#include <cstdio>
#include "SourceGradeTestSupport.h"
#include "core/MediaCore.h"
#include "modules/ProgramFramePreview.h"
#include "rpc/JsonRpcServer.h"

using namespace corevideo;

namespace {
std::unique_ptr<modules::ICompositor> metalOrNull(std::string& why) {
  auto metal = modules::createMetalCompositor();
  if (!metal) { why = "createMetalCompositor returned null"; return nullptr; }
  modules::CompositorRenderPlan probe; probe.renderPlanId = "probe";
  for (const auto& w : metal->render(probe, {}).warnings)
    if (w.rfind("Metal compositor unavailable", 0) == 0) { why = w; return nullptr; }
  return metal;
}
// Program pixel at (fx, fy) from the newest program-frame-preview event (B, G, R).
// Drive with applyCommands({}) first: with no workers that is ONE full tick, and a full
// tick sets skipCpuReadback=false (MediaCore.cpp:6058), so Metal reads the preview back.
std::array<int, 3> programPixel(core::MediaCore& core, float fx, float fy) {
  const auto events = core.drainProgramFramePreviewEvents();
  if (events.empty()) return {-1, -1, -1};
  const auto* preview = events.back().get("preview");
  if (!preview) preview = events.back().get("programFramePreview");
  if (!preview) return {-1, -1, -1};
  const int w = int(preview->getNumber("width")), h = int(preview->getNumber("height"));
  const auto px = modules::base64Decode(preview->getString("bgraBase64"));
  const size_t at = (size_t(int(fy * h)) * size_t(w) + size_t(int(fx * w))) * 4;
  if (at + 3 >= px.size()) return {-1, -1, -1};
  return {px[at], px[at + 1], px[at + 2]};
}
}  // namespace

TEST(MetalSourceGrade, AGradedSourceChangesOnProgramAndItsNeighbourDoesNot) {
  std::string why;
  auto metal = metalOrNull(why);
  if (!metal) {
    std::fprintf(stderr, "[metal-test] unavailable: %s\n", why.c_str());
    ASSERT_NE(why.find("no Metal device"), std::string::npos);
    return;
  }
  auto modules = modules::createStubModules();
  modules.compositor = std::move(metal);
  core::MediaCore core(std::move(modules));
  core.useZoomSourcesForTest({});
  // Fixture: both sources show the SAME patch, so any difference on Program is the grade.
  auto left = std::make_shared<gradetest::EpochSource>("capture:cam-a", 192, 96, 64, 7);
  auto right = std::make_shared<gradetest::EpochSource>("capture:cam-b", 192, 96, 64, 11);
  core.addSourceForTest(left); core.addSourceForTest(right);
  (void)core.applyCommands(rpc::Json::Array{rpc::Json::Object{{"type", "load-scene-graph"}, {"sceneId", "pair"},
      {"routes", rpc::Json::Array{
          rpc::Json::Object{{"routeId", "a"}, {"mode", "capture-input"}, {"captureDeviceId", "cam-a"}, {"fitMode", "fill"},
                            {"rect", rpc::Json::Object{{"x", 0}, {"y", 0}, {"width", 0.5}, {"height", 1}}}},
          rpc::Json::Object{{"routeId", "b"}, {"mode", "capture-input"}, {"captureDeviceId", "cam-b"}, {"fitMode", "fill"},
                            {"rect", rpc::Json::Object{{"x", 0.5}, {"y", 0}, {"width", 0.5}, {"height", 1}}}}}}}});
  rpc::JsonRpcServer server(core);

  (void)core.applyCommands({});
  auto l = programPixel(core, .25f, .5f), r = programPixel(core, .75f, .5f);
  ASSERT_GE(l[0], 0) << "precondition: Program preview is read back on a full tick";
  EXPECT_NEAR(l[2], 192, 2); EXPECT_NEAR(r[2], 192, 2);   // precondition: both ungraded

  const auto q = server.handle(rpc::Json::Object{{"id", "q"}, {"type", "query-source-grade"}, {"sourceId", "capture:cam-a"}});
  ASSERT_TRUE(q.get("present")->asBool());
  ASSERT_EQ(q.getNumber("sourceEpoch"), 7);
  const auto set = server.handle(rpc::Json::Object{{"id", "s"}, {"type", "set-source-grade"}, {"sourceId", "capture:cam-a"},
      {"sourceEpoch", q.getNumber("sourceEpoch")}, {"expectedRevision", q.getNumber("revision")},
      {"grade", rpc::Json::Object{{"lut", "none"}, {"exposure", 2}}}});
  ASSERT_TRUE(set.get("accepted")->asBool()) << set.stringify();

  (void)core.applyCommands({});
  l = programPixel(core, .25f, .5f); r = programPixel(core, .75f, .5f);
  EXPECT_NEAR(l[2], 243, 2); EXPECT_NEAR(l[1], 147, 2); EXPECT_NEAR(l[0], 115, 2);
  EXPECT_NEAR(r[2], 192, 2); EXPECT_NEAR(r[1], 96, 2);  EXPECT_NEAR(r[0], 64, 2);

  // The identity fence is a pixel claim too: a new epoch paints the grade neutral.
  left->setEpoch(8);
  (void)core.applyCommands({});
  l = programPixel(core, .25f, .5f);
  EXPECT_NEAR(l[2], 192, 2) << "an epoch change must not leave the old grade on air";
  const auto state = core.sessionState();
  const auto* rows = state.get("sourceGrades");
  ASSERT_TRUE(rows && rows->isArray() && rows->asArray().size() == 1);
  EXPECT_FALSE(rows->asArray()[0].get("active")->asBool());
  EXPECT_EQ(rows->asArray()[0].getNumber("liveEpoch"), 8);

  // Re-asserting with the new epoch (what the Mac store does, D4) restores it.
  const auto again = server.handle(rpc::Json::Object{{"id", "s2"}, {"type", "set-source-grade"}, {"sourceId", "capture:cam-a"},
      {"sourceEpoch", 8}, {"expectedRevision", 1}, {"grade", rpc::Json::Object{{"lut", "none"}, {"exposure", 2}}}});
  ASSERT_TRUE(again.get("accepted")->asBool());
  (void)core.applyCommands({});
  l = programPixel(core, .25f, .5f); r = programPixel(core, .75f, .5f);
  EXPECT_NEAR(l[2], 243, 2);
  EXPECT_NEAR(r[2], 192, 2) << "the neighbour never changed";
}
#endif  // COREVIDEO_WITH_METAL
```

Fixture invariants: the two route rects do not overlap and sample points sit at their centres; both
sources share one colour; `applyCommands({})` must render exactly one full tick (the test relies on
`audioWorkerActive_` being false in a test process, `MediaCore.cpp:1189-1196`). If a full tick does
not emit a `program-frame-preview` event on Metal, read `core.lastProgramFrameForTest().preview`
instead (same pixels) and say so; do not switch to a light tick.

**Mutations to run:**
1. In `SourceGradeState::applyTo`, stamp the grade onto **every** layer (drop the id match) → the
   right-hand assertions in both end-to-end checks.
2. Swap `participantId`/`sourceId` priority at `SourceGradeState.h:54` → expected red on the capture
   path only if the plan layer's `sourceId` differs from its participantId; report what happens (for
   capture-input both are `capture:cam-a`, so this may stay green: say so, it is then covered for
   Zoom only by Task 7's real-engine proof).
3. In `MetalCompositorAdapter.mm:1241`, always use `renderPlan.colorGrade` → every graded case red.
4. Delete the `lut` branches from `colorGradeParams` → warm-film, punch, cool-broadcast red (neutral
   red only if the GPU is exact; record).
5. In `applyTo`, ignore the epoch (always the accepted grade) → "an epoch change must not leave the
   old grade on air".

- [ ] Step 1: write tests; run on unmodified code: the compositor-level tests should already pass
      (the shader exists); the end-to-end test is red until Task 2 is in (query). Record both facts.
- [ ] Step 2: no production change expected. If a test fails on correct code, the test is the first
      suspect.
- [ ] Step 3: mutations; full `ctest` on `build-metal`; commit
      `test(metal): per-source grade changes only its source on Program (G1 pixel proof)`.

**Done when:** three new tests green on this Mac (and self-skipping on a machine without Metal);
mutation results recorded.

---

### Task 4 (shell, G1): the grade model, source identity and the saved-grades file

**Files:**
- Create: `mac-shell/Sources/CoreVideoProShell/SourceGrade.swift` (NEW)
- Modify: `mac-shell/Sources/CoreVideoProShell/AppModel.swift` (`RosterParticipant` gains two fields and parses them, `:289-323`)
- Create: `mac-shell/Sources/CoreVideoProShell/ShellTestsGrade.swift` (NEW; `gradeCases`)
- Modify: `mac-shell/Sources/CoreVideoProShell/ShellTests.swift` (append `+ gradeCases` at `:4082`)

**Interfaces.**

```swift
/// Twin of the Windows ColorGrade (Models/ProductionModels.cs:829-842). JSON keys are the
/// System.Text.Json Web-default camelCase names, so presets move between platforms.
struct SourceColorGrade: Codable, Equatable {
    var lut = "none"
    var exposure = 0
    var contrast = 0
    var saturation = 0
    var temperature = 0
    // Task 9 adds: var advanced: AdvancedGradeDocument? = nil (decodeIfPresent)

    static let looks = ["none", "neutral", "warm-film", "cool-broadcast", "punch"]
    static let editorRange = -50...50      // ColorGradeEditorWindow.xaml:52-55
    static let wireRange = -100...100      // MediaCore.cpp:1246-1249; Windows paste, VM.Advanced.cs:166
    func validated(range: ClosedRange<Int>) throws -> SourceColorGrade   // throws GradeValidationError
    var summary: String                    // ProductionModels.cs:838-841, exactly
    var isNeutral: Bool                    // lut "none", all axes 0 (and, from Task 9, no document)
    func wireGrade(documentRef: String?) -> JSONObject   // {lut, exposure, contrast, saturation, temperature[, advanced:{documentRef}]}
}
struct GradeValidationError: Error, Equatable { let message: String }

enum GradeSourceIdentity: Equatable {
    case capture(deviceId: String)
    case zoom(participantId: String, persistentId: String, sourceGeneration: Int)
    var sourceId: String     // "capture:<id>" | "<raw pid>" — the frame key (fact 4)
    var stableKey: String?   // "capture:<id>" | "zoom-person:<persistentId>" | nil when persistentId is empty
    var binding: String      // "capture:<id>" | "person:<persistentId>" | "gen:<pid>:<sourceGeneration>"
}

/// ~/Library/Application Support/CoreVideoPro/source-grades.json (spec §5.9). Separate from
/// ShellPrefs because a G2/G3 grade can embed 1.5 MB of LUT text.
struct SourceGradeFile: Equatable {
    var grades: [String: SourceColorGrade] = [:]
    var order: [String] = []               // oldest first; every key of `grades` exactly once
    static let capacity = 64
    static let realPath: String            // the operator's file; never touched by a harness run
    static let path: String                // per-process temp file under ShellHarness.isActive (the ShellPrefs.path rule)
    static func load(from path: String) -> (file: SourceGradeFile, notices: [String])
    func save(to path: String) throws      // atomic write, creates the directory
    mutating func remember(_ grade: SourceColorGrade, key: String) -> String?   // returns the retirement notice, if any
    mutating func forget(key: String)
}
```

`RosterParticipant` gains `var persistentId = ""` and `var sourceGeneration = 0`, parsed from
`persistentId` (String) and `sourceGeneration` (number), defaulted when absent. Both are
`var`-with-default so the memberwise initializer stays source compatible.

**Behaviour.**
- `summary` is `"Look \(lut) · exposure \(f(exposure)) · contrast \(f(contrast)) · saturation \(f(saturation))"`
  where `f` prefixes `+` for positive values (Windows omits temperature from the summary; so does
  this).
- `validated` throws `"Unsupported Basic grade."` (the Windows paste message,
  `ColorGradeEditorViewModel.Advanced.cs:166-167`) for an unknown look or an axis outside `range`.
- File on disk: `{"version": 1, "grades": {"<key>": <SourceColorGrade>}, "order": ["<key>", …]}`.
  `load` is tolerant per entry (decode each value separately): a malformed entry is skipped with
  the notice `"Saved grade unavailable: <reason>"` (`StudioViewModel.Grading.cs:27`); a key not
  starting `capture:` or `zoom-person:` is dropped silently (`:26`); only the first 64 keys in
  `order` are kept (`:24`, `Take(64)`); keys present in `grades` but missing from `order` are
  appended in sorted order; an unreadable file yields an empty file and no notice.
- `remember` moves an existing key to the end without retiring; adding a 65th key retires
  `order.first` and returns `" · oldest saved source grade retired (64-source limit)"`
  (`StudioViewModel.Grading.cs:151`).

**Tests** (`ShellTestsGrade.swift`; register in `gradeCases` and append to the case list):

```swift
import Foundation

extension ShellTests {
    @MainActor
    static var gradeCases: [(String, () -> Void)] {
        [
            ("grade/model-validation-and-summary", testSourceGradeValidationAndSummaryAreWindows),
            ("grade/model-wire-shape", testSourceGradeWireShape),
            ("grade/identity-keys-and-bindings", testGradeIdentityKeysAndBindings),
            ("grade/roster-carries-persistent-identity", testRosterCarriesPersistentIdentity),
            ("grade/file-tolerant-capped-ordered", testSourceGradeFileIsTolerantCappedAndOrdered),
            ("grade/file-harness-isolation", testSourceGradeFileIsIsolatedUnderTheHarness),
            // Task 5, 6, 9, 10 append here.
        ]
    }

    static func testSourceGradeValidationAndSummaryAreWindows() {
        var g = SourceColorGrade(lut: "warm-film", exposure: 2, contrast: 0, saturation: -3, temperature: 7)
        expectEqual(g.summary, "Look warm-film · exposure +2 · contrast 0 · saturation -3",
                    "Windows ColorGrade.Summary (ProductionModels.cs:838-841)")
        expect((try? g.validated(range: SourceColorGrade.editorRange)) != nil, "±50 is valid in the editor")
        g.exposure = 51
        expect((try? g.validated(range: SourceColorGrade.editorRange)) == nil, "51 is outside the editor's −50…+50")
        expect((try? g.validated(range: SourceColorGrade.wireRange)) != nil, "51 is inside the core's ±100")
        g.exposure = 101
        do { _ = try g.validated(range: SourceColorGrade.wireRange); expect(false, "101 must be refused") }
        catch let e as GradeValidationError { expectEqual(e.message, "Unsupported Basic grade.", "Windows paste message") }
        catch { expect(false, "unexpected error type") }
        g.exposure = 0; g.lut = "teal-orange"
        expect((try? g.validated(range: SourceColorGrade.wireRange)) == nil, "an unknown look is refused (core: unsupported-look)")
        expectEqual(SourceColorGrade.looks, ["none", "neutral", "warm-film", "cool-broadcast", "punch"],
                    "Windows LutOptions, same order (ColorGradeEditorViewModel.cs:12-13)")
        expect(SourceColorGrade().isNeutral, "the default grade is neutral")
        expect(!SourceColorGrade(lut: "neutral").isNeutral,
               "the 'neutral' look adds contrast (ColorGradeParams.h:21), so it is not neutral")
    }

    static func testSourceGradeWireShape() {
        let g = SourceColorGrade(lut: "punch", exposure: -4, contrast: 1, saturation: 2, temperature: 3)
        let wire = g.wireGrade(documentRef: nil)
        expectEqual(wire["lut"] as? String, "punch", "look at the grade root")
        expectEqual(wire["exposure"] as? Int, -4, "exposure")
        expectEqual(wire["temperature"] as? Int, 3, "temperature")
        expect(wire["advanced"] == nil, "no advanced key without a document")
        let ref = String(repeating: "a", count: 64)
        let withDocument = g.wireGrade(documentRef: ref)
        expectEqual((withDocument["advanced"] as? JSONObject)?["documentRef"] as? String, ref,
                    "a registered document travels by reference (JsonRpcServer.cpp:278-290)")
    }

    static func testGradeIdentityKeysAndBindings() {
        let cam = GradeSourceIdentity.capture(deviceId: "cam-1")
        expectEqual(cam.sourceId, "capture:cam-1", "capture frame key (AvfCaptureDeviceAdapter.mm:226-227)")
        expectEqual(cam.stableKey, "capture:cam-1", "Windows GradePersistenceKey for capture")
        expectEqual(cam.binding, "capture:cam-1", "a capture binding is the hardware id")
        let guest = GradeSourceIdentity.zoom(participantId: "16778240", persistentId: "P-abc", sourceGeneration: 3)
        expectEqual(guest.sourceId, "16778240", "a Zoom grade targets the RAW participant id, never zoom:<pid>")
        expectEqual(guest.stableKey, "zoom-person:P-abc", "Windows GradePersistenceKey (StudioViewModel.Grading.cs:16-17)")
        expectEqual(guest.binding, "person:P-abc", "a Zoom binding is the person")
        let reused = GradeSourceIdentity.zoom(participantId: "16778240", persistentId: "P-xyz", sourceGeneration: 4)
        expect(reused.binding != guest.binding, "a reused id with another person never shares a binding")
        let anonymous = GradeSourceIdentity.zoom(participantId: "16778240", persistentId: "", sourceGeneration: 3)
        expect(anonymous.stableKey == nil, "no persistentId → never persisted (Windows returns null)")
        expectEqual(anonymous.binding, "gen:16778240:3", "no persistentId → id plus source generation")
        let regenerated = GradeSourceIdentity.zoom(participantId: "16778240", persistentId: "", sourceGeneration: 4)
        expect(regenerated.binding != anonymous.binding, "a new source generation breaks an anonymous binding")
    }

    static func testRosterCarriesPersistentIdentity() {
        let parsed = RosterParticipant.parse([
            ["userId": "16778240", "displayName": "Guest", "persistentId": "P-abc", "sourceGeneration": 3],
            ["userId": "16778241", "displayName": "Legacy"],
        ])
        expectEqual(parsed.first?.persistentId, "P-abc", "engine shape (ZoomEngineRuntime.cpp:1443)")
        expectEqual(parsed.first?.sourceGeneration, 3, "engine shape (ZoomEngineRuntime.cpp:1444)")
        expectEqual(parsed.last?.persistentId, "", "absent → empty")
        expectEqual(parsed.last?.sourceGeneration, 0, "absent → 0")
    }

    static func testSourceGradeFileIsTolerantCappedAndOrdered() {
        // Fixture: 66 valid capture keys c00…c65 listed oldest-first in `order`, one malformed entry,
        // one foreign key. Invariant: every valid key appears in `order` exactly once.
        var grades: [String: Any] = [:]
        var order: [String] = []
        for i in 0..<66 {
            let key = String(format: "capture:c%02d", i)
            grades[key] = ["lut": "none", "exposure": i % 50, "contrast": 0, "saturation": 0, "temperature": 0]
            order.append(key)
        }
        grades["capture:bad"] = ["lut": 5]
        order.insert("capture:bad", at: 0)
        grades["media:x"] = ["lut": "none", "exposure": 0, "contrast": 0, "saturation": 0, "temperature": 0]
        let dir = NSTemporaryDirectory() + "grade-file-\(UUID().uuidString)"
        let path = dir + "/source-grades.json"
        try? FileManager.default.createDirectory(atPath: dir, withIntermediateDirectories: true)
        let data = try! JSONSerialization.data(withJSONObject: ["version": 1, "grades": grades, "order": order])
        try! data.write(to: URL(fileURLWithPath: path))

        var (file, notices) = SourceGradeFile.load(from: path)
        expectEqual(file.grades.count, 64, "Take(64) (StudioViewModel.Grading.cs:24)")
        expectEqual(file.order.first, "capture:c00", "oldest valid key first")
        expectEqual(file.order.last, "capture:c63", "the first 64 valid keys in order are kept")
        expect(file.grades["media:x"] == nil, "a foreign key is dropped")
        expectEqual(notices.filter { $0.hasPrefix("Saved grade unavailable: ") }.count, 1,
                    "the malformed entry is named once (Grading.cs:27)")
        expectEqual(file.grades["capture:c07"]?.exposure, 7, "values survive")

        let notice = file.remember(SourceColorGrade(exposure: 9), key: "capture:new")
        expectEqual(notice, " · oldest saved source grade retired (64-source limit)", "Grading.cs:151")
        expect(file.grades["capture:c00"] == nil, "the oldest is retired")
        expectEqual(file.order.last, "capture:new", "the newest is last")
        expect(file.remember(SourceColorGrade(exposure: 1), key: "capture:c10") == nil, "re-saving a key retires nothing")
        expectEqual(file.order.last, "capture:c10", "re-saving moves the key to the end")
        expectEqual(file.grades.count, 64, "still at capacity")

        try! file.save(to: path)
        let (reloaded, _) = SourceGradeFile.load(from: path)
        expectEqual(reloaded, file, "round trip is exact")
    }

    static func testSourceGradeFileIsIsolatedUnderTheHarness() {
        expect(SourceGradeFile.path != SourceGradeFile.realPath,
               "a harness run can never read or replace the operator's saved grades")
        expect(SourceGradeFile.realPath.hasSuffix("/Library/Application Support/CoreVideoPro/source-grades.json"),
               "spec §5.9 location")
    }
}
```

**Mutations to run:**
1. `sourceId` for Zoom returns `"zoom:" + pid` → identity test.
2. `binding` for Zoom ignores persistentId (`"gen:…"` always) → the reused-id assertion still red?
   It must be: `gen:16778240:4` vs `gen:16778240:3` differ, so **this mutation stays green**; it is
   caught by Task 5's `testAnEpochChangeReassertsForTheSamePersonOnly` instead (record that).
3. `load` keeps the last 64 instead of the first → `order.first` / `order.last`.
4. `remember` re-adding an existing key retires the oldest → the "re-saving a key" assertion.
5. Drop the `summary` sign prefix → summary assertion.

- [ ] Steps: tests red (compile) → implement → green → `swift build` clean, harness count up by 6
      cases → mutations → commit `mac: per-source grade model, identity and saved-grade file (G1)`.

**Done when:** harness green; `lint-mac-design.sh` green; mutation results recorded.

---

### Task 5 (shell, G1): `SourceGradeStore` — the only sender, the fences, and every re-assertion

**Files:**
- Create: `mac-shell/Sources/CoreVideoProShell/SourceGradeStore.swift` (NEW)
- Modify: `mac-shell/Sources/CoreVideoProShell/CoreCommands.swift` (two root-level builders)
- Modify: `mac-shell/Sources/CoreVideoProShell/AppModel.swift` (own the store; hooks in `onConnected`
  `:1474`, `applySnapshot` `:1592`, the meeting-end branch `:1745-1747`, and the quit path)
- Modify: `mac-shell/Sources/CoreVideoProShell/ShellTestsGrade.swift`

**Interfaces.**

```swift
extension CoreCommands {
    // Root-level requests (fact 6). These literals are what ContractParity's Swift scan sees.
    static func querySourceGrade(sourceId: String) -> JSONObject          // ["type": "query-source-grade", "sourceId": …]
    static func setSourceGrade(sourceId: String, sourceEpoch: UInt64,
                               expectedRevision: Int64, grade: JSONObject) -> JSONObject
}

enum GradeApplyOutcome: Equatable {
    case applied(revision: Int64)
    case refused(String)       // operator text, verbatim Windows wording
    case superseded            // a newer edit replaced this one before it was sent
    case stale(String)         // the core generation changed while it was in flight
}

struct SourceGradeSummary: Equatable {    // the G4 inspector reads this
    let line: String           // SourceColorGrade.summary
    let indicator: String?     // Task 9: the Windows AdvancedIndicator; nil in G1
    let active: Bool           // false while sourceGrades reports active:false
    let identityChanged: Bool  // the live person no longer matches the grade's binding
}

@MainActor
final class SourceGradeStore: ObservableObject {
    typealias Send = (JSONObject) async throws -> JSONObject
    init(send: @escaping Send, filePath: String = SourceGradeFile.path, saveDelay: TimeInterval = 0.75)
    var identityProvider: (String) -> GradeSourceIdentity? = { _ in nil }   // current identity for a source id
    var now: () -> Date = Date.init                                         // re-assert pacing clock (tests inject)
    @Published private(set) var lastNotice: String

    // Apply (the editor's only path to air).
    func apply(_ grade: SourceColorGrade, sourceId: String, authoredFor binding: String) async -> GradeApplyOutcome

    // Lifecycle.
    func coreGenerationChanged()
    func observe(sourceGrades: [JSONObject]?, sources: [GradeSourceIdentity], now: Date)
    func meetingEnded()
    func flush()                                   // write the saved file now (quit)

    // Reads (G1 editor seed; G4 inspector).
    func seed(for sourceId: String) -> (grade: SourceColorGrade, draftNotice: String?)
    func summary(for sourceId: String) -> SourceGradeSummary?
    func isGraded(_ sourceId: String) -> Bool

    static let reassertInterval: TimeInterval = 0.5      // first 10 attempts per source
    static let reassertSlowInterval: TimeInterval = 2.0  // after that
}
```

AppModel additions:

```swift
private(set) lazy var sourceGrades: SourceGradeStore   // send = the quitRequestForTesting seam, else the bridge
func gradeIdentity(for sourceId: String) -> GradeSourceIdentity?   // roster (Zoom) or captureDevices (capture)
var gradeSources: [GradeSourceIdentity]                            // every roster participant and capture device now known
/// Harness-only: the saved-grade file the NEXT AppModel() uses. nil → SourceGradeFile.path.
/// Exists because every AppModel() in one harness process would otherwise share one file, and a
/// debounced save from one case could land in the next case's spin and be re-applied there (D2).
static var gradeFilePathForTesting: String?
```

Test helper (add beside `gradeStore`); **every** grade test that builds an `AppModel` uses it:

```swift
@MainActor static func gradeModel(file: String? = nil) -> AppModel {
    AppModel.gradeFilePathForTesting = file ?? tempGradeFile()
    defer { AppModel.gradeFilePathForTesting = nil }
    return AppModel()
}
```

**Behaviour.** The apply flow (spec §5.3, with the exact strings):
1. **Person check first.** If `identityProvider(sourceId)?.binding != binding`, answer
   `.refused("Source identity changed. Close and reopen its grade editor before applying.")`
   (`ColorGradeEditorViewModel.Apply.cs:26`) and send nothing.
2. **One in flight per source.** If an apply for `sourceId` is in flight, park this one as the
   source's pending apply. A pending apply already parked returns `.superseded`. When the in-flight
   apply finishes, the pending one runs. No timer debounce, and nothing rides the repeating sync.
3. **Epoch.** If the store holds no epoch for the source (first apply, after a generation change, or
   after `sourceGrades` showed a different live epoch), send `query-source-grade`. `present:false`
   answers `.refused("Source has no video yet; grade not applied.")` (spec §5.10, new copy). The
   query's `revision` replaces the store's revision.
4. **Send** `set-source-grade` (built by `CoreCommands.setSourceGrade`) with the held epoch and
   revision. Before sending, the grade is checked with `validated(range: wireRange)`.
5. **Acknowledged** (`accepted:true`): record `revision`, `sourceEpoch`, the grade as the source's
   **session grade**, and its binding; persist under `stableKey` (if any) via
   `SourceGradeFile.remember`, schedule a save `saveDelay` after the last change
   (`StudioViewModel.Grading.cs:38-47`), and append any retirement notice to `lastNotice`. A neutral
   grade (D6) is recorded as the session grade but **forgets** the saved key instead.
6. **Refused** with `source-identity-changed` or `grade-revision-conflict`: re-query once, redo step
   1 against the current identity, and retry once (D7). Any other refusal, or a second refusal,
   answers `.refused("Grade was not applied: \(reason). Previous live grade retained.")`
   (`Apply.cs:50`). A thrown error answers `.refused("Grade was not applied: \(message)")` (`Apply.cs:55`).
7. **Generation fence.** Every apply captures the store's generation counter.
   `coreGenerationChanged()` increments it; an acknowledgement that returns under an older
   generation is discarded and answers
   `.stale("Source changed during apply. Grade acknowledgement is no longer current; review the draft again.")`
   (`Apply.cs:49`). It is never recorded as a session grade.

Re-assertion (spec §5.3, CLAUDE.md "A ONE-SHOT COMMAND MUST BE RE-APPLIED ON EVERY CORE GENERATION"):
- `coreGenerationChanged()` (called from `AppModel.onConnected()`, which runs on the initial
  handshake **and** every respawn) forgets every held epoch and revision (Windows does the same,
  `StudioViewModel.Grading.cs:35`) and marks every session grade, plus every saved **capture**
  grade not yet applied this launch (D2), as pending re-assert.
- `observe(…)` runs from `applySnapshot` with `snapshot["sourceGrades"]`, `gradeSources` and `Date()`
  (through `now`). For each pending source whose identity is present and whose binding still
  matches, it sends `query-source-grade` no more often than `reassertInterval` (then
  `reassertSlowInterval` after 10 attempts); on `present:true` it applies the session grade through
  the same flow. A pending source whose binding no longer matches is dropped from pending and
  flagged `identityChanged`.
- A `sourceGrades` row with `active:false` and `livePresent:true` for a source with a session grade
  forgets that source's epoch and, if the binding still matches (D4), marks it pending. Rows with
  `active:true` change nothing.
- D5: when a session grade's participant id is absent from `sources` and exactly one present Zoom
  identity has the same `person:<persistentId>` binding under a different id, the session grade
  moves to that id and is marked pending. With zero or several candidates, nothing moves.
- `meetingEnded()` (called where `AppModel` already detects a meeting ending, beside
  `endMeetingAudioSession()`, never on an unknown state) drops every Zoom session grade and its
  pending re-assert. Capture grades are untouched. Saved grades are untouched.
- `seed(for:)`: the session grade if its binding matches the live identity; else the saved grade
  under the identity's `stableKey`, with `draftNotice` =
  `"Saved grade loaded as a draft. Apply Live confirms this source binding."` for Zoom
  (`StudioViewModel.Grading.cs:89`); else a neutral grade and no notice.
- AppModel's quit path calls `sourceGrades.flush()` before the core is stopped.

**Test fixture** (complete; top of the Task 5 block in `ShellTestsGrade.swift`):

```swift
/// Mirrors native SourceGradeState (apply refuses identity before revision,
/// SourceGradeState.h:36-48) and register-grade-document (JsonRpcServer.cpp:278-290).
/// Fixture invariants: epochs[id] != nil ⇔ the core saw a frame for id in the LAST tick
/// (observe replaces the map every tick); revisions start at 0 in every core process;
/// respawn() is a new process: no frames, no grades, no documents.
final class FakeGradeCore: @unchecked Sendable {
    private let lock = NSLock()
    private var _epochs: [String: UInt64] = [:]
    private var _revisions: [String: Int64] = [:]
    private var _documents: Set<String> = []
    private var _bodies: [JSONObject] = []
    private var _hold = false
    private var _refusals: [String] = []
    private func locked<T>(_ body: () -> T) -> T { lock.lock(); defer { lock.unlock() }; return body() }
    func setEpoch(_ id: String, _ epoch: UInt64?) { locked { _epochs[id] = epoch } }
    func setRevision(_ id: String, _ revision: Int64) { locked { _revisions[id] = revision } }
    var hold: Bool { get { locked { _hold } } set { locked { _hold = newValue } } }
    func refuseNext(_ reasons: String...) { locked { _refusals.append(contentsOf: reasons) } }
    func respawn() { locked { _epochs = [:]; _revisions = [:]; _documents = [] } }
    var bodies: [JSONObject] { locked { _bodies } }
    var documents: Set<String> { locked { _documents } }
    func requests(_ type: String) -> [JSONObject] { bodies.filter { $0["type"] as? String == type } }
    static func int(_ value: Any?) -> Int64? { (value as? NSNumber)?.int64Value }

    func handle(_ body: JSONObject) async throws -> JSONObject {
        locked { _bodies.append(body) }
        let id = body["sourceId"] as? String ?? ""
        switch body["type"] as? String {
        case "query-source-grade":
            return locked { ["ok": true, "present": _epochs[id] != nil,
                             "sourceEpoch": NSNumber(value: _epochs[id] ?? 0),
                             "revision": NSNumber(value: _revisions[id] ?? 0)] }
        case "set-source-grade":
            while hold { try? await Task.sleep(nanoseconds: 5_000_000) }
            return locked { () -> JSONObject in
                if !_refusals.isEmpty { return ["ok": true, "accepted": false, "reason": _refusals.removeFirst()] }
                guard let epoch = Self.int(body["sourceEpoch"]), let expected = Self.int(body["expectedRevision"]) else {
                    return ["ok": true, "accepted": false, "reason": "invalid-source-grade"]
                }
                guard let live = _epochs[id], live == UInt64(epoch) else {
                    return ["ok": true, "accepted": false, "reason": "source-identity-changed"]
                }
                let current = _revisions[id] ?? 0
                guard expected == current else {
                    return ["ok": true, "accepted": false, "reason": "grade-revision-conflict", "revision": NSNumber(value: current)]
                }
                if let advanced = (body["grade"] as? JSONObject)?["advanced"] as? JSONObject,
                   let ref = advanced["documentRef"] as? String, !_documents.contains(ref) {
                    return ["ok": true, "accepted": false, "reason": "invalid-advanced-grade"]
                }
                _revisions[id] = current + 1
                return ["ok": true, "accepted": true, "revision": NSNumber(value: current + 1), "sourceEpoch": NSNumber(value: epoch)]
            }
        case "register-grade-document":
            let ref = body["documentRef"] as? String ?? ""
            if locked({ _documents.contains(ref) }) { return ["ok": true, "present": true] }
            guard let text = body["documentJson"] as? String else { return ["ok": true, "present": false] }
            guard GradeDocumentRef.sha256Hex(text) == ref else { throw BridgeError.remote("Invalid grade document content.") }
            locked { _ = _documents.insert(ref) }
            return ["ok": true, "present": true]
        default:
            return ["ok": true]
        }
    }
}

final class IdentityBox { var ids: [String: GradeSourceIdentity]; init(_ ids: [String: GradeSourceIdentity]) { self.ids = ids } }

@MainActor static func tempGradeFile() -> String {
    NSTemporaryDirectory() + "grade-store-\(UUID().uuidString)/source-grades.json"
}
@MainActor static func gradeStore(_ core: FakeGradeCore, _ box: IdentityBox, file: String? = nil) -> SourceGradeStore {
    let store = SourceGradeStore(send: { try await core.handle($0) }, filePath: file ?? tempGradeFile())
    store.identityProvider = { box.ids[$0] }
    return store
}
@MainActor static func awaitApply(_ store: SourceGradeStore, _ grade: SourceColorGrade,
                                  _ sourceId: String, binding: String) -> GradeApplyOutcome? {
    var outcome: GradeApplyOutcome?
    Task { outcome = await store.apply(grade, sourceId: sourceId, authoredFor: binding) }
    spin(until: { outcome != nil }, limit: 3)
    return outcome
}
/// A sourceGrades row. `active` is DERIVED with the core's rule so no fixture can contradict it.
static func gradeRow(_ id: String, revision: Int64, applied: UInt64, live: UInt64, present: Bool = true) -> JSONObject {
    ["sourceId": id, "revision": NSNumber(value: revision), "appliedEpoch": NSNumber(value: applied),
     "liveEpoch": NSNumber(value: present ? live : 0), "livePresent": present, "active": present && applied == live]
}
```

ContractParity scans every Swift file except `ShellTests.swift` (fact 8), so
`ShellTestsGrade.swift` is scanned too: no test may contain a `"type": "<name>"` dictionary literal
for a name the core does not handle (the fake reads `body["type"]`, which the scan does not match).

`GradeDocumentRef.sha256Hex` is introduced in Task 9. Until then, replace the
`register-grade-document` case with `default` behaviour and add it back in Task 9 (one line in the
report).

**Tests** (complete; register each in `gradeCases`):

```swift
@MainActor static func testGradeApplyQueriesOnceThenUsesTheCoresEpochAndRevision() {
    let core = FakeGradeCore(); core.setEpoch("16778240", 7)          // fixture: the engine's epoch is 7
    let guest = GradeSourceIdentity.zoom(participantId: "16778240", persistentId: "P-a", sourceGeneration: 1)
    let store = gradeStore(core, IdentityBox(["16778240": guest]))
    expectEqual(awaitApply(store, SourceColorGrade(exposure: 3), "16778240", binding: guest.binding),
                .applied(revision: 1), "first apply acknowledged at revision 1")
    expectEqual(core.requests("query-source-grade").count, 1, "the epoch was unknown, so the store asked once")
    let first = core.requests("set-source-grade").first ?? [:]
    expectEqual(FakeGradeCore.int(first["sourceEpoch"]), 7, "the core's epoch, not 0")
    expectEqual(FakeGradeCore.int(first["expectedRevision"]), 0, "a fresh core starts at revision 0")
    expect(first["payload"] == nil, "root-level request (JsonRpcServer.cpp:291-292)")
    expectEqual(awaitApply(store, SourceColorGrade(exposure: 4), "16778240", binding: guest.binding),
                .applied(revision: 2), "second apply")
    expectEqual(core.requests("query-source-grade").count, 1, "a known epoch is not re-queried")
    expectEqual(core.requests("set-source-grade").count, 2, "no conflict round-trip: the stored revision was right")
}

@MainActor static func testGradeApplyWithNoFramesSaysSoAndSendsNothing() {
    let core = FakeGradeCore()
    let cam = GradeSourceIdentity.capture(deviceId: "cam")
    let store = gradeStore(core, IdentityBox(["capture:cam": cam]))
    expectEqual(awaitApply(store, SourceColorGrade(exposure: 3), "capture:cam", binding: cam.binding),
                .refused("Source has no video yet; grade not applied."), "spec §5.10")
    expectEqual(core.requests("set-source-grade").count, 0, "nothing sent for an absent source")
}

@MainActor static func testCaptureEpochZeroIsAppliedNotTreatedAsAbsent() {
    let core = FakeGradeCore(); core.setEpoch("capture:cam", 0)
    let cam = GradeSourceIdentity.capture(deviceId: "cam")
    let store = gradeStore(core, IdentityBox(["capture:cam": cam]))
    expectEqual(awaitApply(store, SourceColorGrade(exposure: 3), "capture:cam", binding: cam.binding),
                .applied(revision: 1), "Mac capture frames carry epoch 0 and are present")
    expectEqual(FakeGradeCore.int(core.requests("set-source-grade").first?["sourceEpoch"]), 0, "sent with epoch 0")
}

@MainActor static func testOneApplyInFlightAndTheNewestPendingWins() {
    let core = FakeGradeCore(); core.setEpoch("capture:cam", 0)
    let cam = GradeSourceIdentity.capture(deviceId: "cam")
    let store = gradeStore(core, IdentityBox(["capture:cam": cam]))
    core.hold = true
    var outcomes: [Int: GradeApplyOutcome] = [:]
    for exposure in 1...3 {
        Task { outcomes[exposure] = await store.apply(SourceColorGrade(exposure: exposure), sourceId: "capture:cam", authoredFor: cam.binding) }
        RunLoop.current.run(until: Date().addingTimeInterval(0.05))
    }
    spin(until: { core.requests("set-source-grade").count == 1 }, limit: 2)
    RunLoop.current.run(until: Date().addingTimeInterval(0.1))
    expectEqual(core.requests("set-source-grade").count, 1, "one apply in flight per source")
    core.hold = false
    spin(until: { outcomes.count == 3 }, limit: 3)
    let sets = core.requests("set-source-grade")
    expectEqual(sets.count, 2, "exactly one follow-up apply")
    expectEqual(FakeGradeCore.int((sets.last?["grade"] as? JSONObject)?["exposure"]), 3, "the follow-up carries the newest edit")
    expectEqual(outcomes[2], .superseded, "the replaced edit says so")
    expectEqual(outcomes[3], .applied(revision: 2), "the newest edit is acknowledged")
}

@MainActor static func testAConflictIsRequeriedAndRetriedExactlyOnce() {
    let core = FakeGradeCore(); core.setEpoch("capture:cam", 0)
    let cam = GradeSourceIdentity.capture(deviceId: "cam")
    let store = gradeStore(core, IdentityBox(["capture:cam": cam]))
    _ = awaitApply(store, SourceColorGrade(exposure: 1), "capture:cam", binding: cam.binding)
    core.setRevision("capture:cam", 5)                   // the store's revision 1 is now stale
    expectEqual(awaitApply(store, SourceColorGrade(exposure: 2), "capture:cam", binding: cam.binding),
                .applied(revision: 6), "re-queried and retried on the core's revision (D7)")
    expectEqual(core.requests("set-source-grade").map { FakeGradeCore.int($0["expectedRevision"]) }, [0, 1, 5],
                "stale 1 refused, then 5")
    core.refuseNext("grade-revision-conflict", "grade-revision-conflict")
    expectEqual(awaitApply(store, SourceColorGrade(exposure: 3), "capture:cam", binding: cam.binding),
                .refused("Grade was not applied: grade-revision-conflict. Previous live grade retained."),
                "a second refusal is shown verbatim (Apply.cs:50)")
    expectEqual(core.requests("set-source-grade").count, 5, "exactly one retry, never a loop")
    core.refuseNext("grade-capacity")
    expectEqual(awaitApply(store, SourceColorGrade(exposure: 4), "capture:cam", binding: cam.binding),
                .refused("Grade was not applied: grade-capacity. Previous live grade retained."), "not retryable")
    expectEqual(core.requests("set-source-grade").count, 6, "no retry for a non-identity refusal")
}

@MainActor static func testTheWrongPersonIsRefusedBeforeAnythingIsSent() {
    let core = FakeGradeCore(); core.setEpoch("16778240", 7)
    let box = IdentityBox(["16778240": .zoom(participantId: "16778240", persistentId: "P-a", sourceGeneration: 1)])
    let store = gradeStore(core, box)
    _ = awaitApply(store, SourceColorGrade(exposure: 1), "16778240", binding: "person:P-a")
    box.ids["16778240"] = .zoom(participantId: "16778240", persistentId: "P-b", sourceGeneration: 2)
    expectEqual(awaitApply(store, SourceColorGrade(exposure: 2), "16778240", binding: "person:P-a"),
                .refused("Source identity changed. Close and reopen its grade editor before applying."), "Apply.cs:26")
    expectEqual(core.requests("set-source-grade").count, 1, "nothing sent for the wrong person")
    box.ids["16778240"] = .zoom(participantId: "16778240", persistentId: "P-a", sourceGeneration: 2)
    core.setEpoch("16778240", 8)                          // same person, new stream
    expectEqual(awaitApply(store, SourceColorGrade(exposure: 3), "16778240", binding: "person:P-a"),
                .applied(revision: 2), "identity-changed is re-queried and retried for the same person")
    expectEqual(core.requests("set-source-grade").map { FakeGradeCore.int($0["sourceEpoch"]) }, [7, 7, 8],
                "stale epoch refused, then the live one")
}

@MainActor static func testACoreRespawnDuringAPendingApplyCannotRecordItsAcknowledgement() {
    let core = FakeGradeCore(); core.setEpoch("capture:cam", 0)
    let cam = GradeSourceIdentity.capture(deviceId: "cam")
    let store = gradeStore(core, IdentityBox(["capture:cam": cam]))
    core.hold = true
    var outcome: GradeApplyOutcome?
    Task { outcome = await store.apply(SourceColorGrade(exposure: 9), sourceId: "capture:cam", authoredFor: cam.binding) }
    spin(until: { core.requests("set-source-grade").count == 1 }, limit: 2)
    store.coreGenerationChanged()
    core.hold = false
    spin(until: { outcome != nil }, limit: 2)
    expectEqual(outcome, .stale("Source changed during apply. Grade acknowledgement is no longer current; review the draft again."),
                "Windows Apply.cs:49; port of AdvancedGradeEditorTests.cs:63")
    expect(!store.isGraded("capture:cam"), "an old generation's acknowledgement is never the session grade")
    store.observe(sourceGrades: [], sources: [cam], now: Date(timeIntervalSince1970: 3_000))
    RunLoop.current.run(until: Date().addingTimeInterval(0.3))
    expectEqual(core.requests("set-source-grade").count, 1, "and it is not re-asserted on the new core")
}

@MainActor static func testCoreRespawnReassertsSessionGradesOnceFramesReturn() {
    let core = FakeGradeCore(); core.setEpoch("capture:cam", 0)
    let cam = GradeSourceIdentity.capture(deviceId: "cam")
    let store = gradeStore(core, IdentityBox(["capture:cam": cam]))
    _ = awaitApply(store, SourceColorGrade(lut: "warm-film", exposure: 5), "capture:cam", binding: cam.binding)
    core.respawn()
    store.coreGenerationChanged()
    let t0 = Date(timeIntervalSince1970: 1_000)
    store.observe(sourceGrades: [], sources: [cam], now: t0)
    spin(until: { core.requests("query-source-grade").count >= 2 }, limit: 2)
    expectEqual(core.requests("set-source-grade").count, 1, "no frames yet (present:false): nothing sent")
    store.observe(sourceGrades: [], sources: [cam], now: t0.addingTimeInterval(0.2))
    RunLoop.current.run(until: Date().addingTimeInterval(0.15))
    expectEqual(core.requests("query-source-grade").count, 2, "re-queries are paced at 500 ms")
    core.setEpoch("capture:cam", 0)
    store.observe(sourceGrades: [], sources: [cam], now: t0.addingTimeInterval(0.6))
    spin(until: { core.requests("set-source-grade").count >= 2 }, limit: 2)
    let reassert = core.requests("set-source-grade").last ?? [:]
    expectEqual(FakeGradeCore.int(reassert["expectedRevision"]), 0, "the new core's revision, not the old 1")
    expectEqual((reassert["grade"] as? JSONObject)?["lut"] as? String, "warm-film", "the session grade itself")
    expectEqual(FakeGradeCore.int((reassert["grade"] as? JSONObject)?["exposure"]), 5, "every axis")
}

@MainActor static func testAnEpochChangeReassertsForTheSamePersonOnly() {
    let core = FakeGradeCore(); core.setEpoch("16778240", 7)
    let box = IdentityBox(["16778240": .zoom(participantId: "16778240", persistentId: "P-a", sourceGeneration: 1)])
    let store = gradeStore(core, box)
    _ = awaitApply(store, SourceColorGrade(exposure: 6), "16778240", binding: "person:P-a")
    let t0 = Date(timeIntervalSince1970: 2_000)
    store.observe(sourceGrades: [gradeRow("16778240", revision: 1, applied: 7, live: 7)], sources: Array(box.ids.values), now: t0)
    RunLoop.current.run(until: Date().addingTimeInterval(0.15))
    expectEqual(core.requests("set-source-grade").count, 1, "an active grade is never re-sent")
    core.setEpoch("16778240", 8)                          // join-ramp size change, same person
    store.observe(sourceGrades: [gradeRow("16778240", revision: 1, applied: 7, live: 8)], sources: Array(box.ids.values),
                  now: t0.addingTimeInterval(1))
    spin(until: { core.requests("set-source-grade").count >= 2 }, limit: 2)
    let reassert = core.requests("set-source-grade").last ?? [:]
    expectEqual(FakeGradeCore.int(reassert["sourceEpoch"]), 8, "re-asserted on the live epoch (D4)")
    expectEqual(FakeGradeCore.int(reassert["expectedRevision"]), 1, "against the core's revision")
    box.ids["16778240"] = .zoom(participantId: "16778240", persistentId: "P-b", sourceGeneration: 2)
    core.setEpoch("16778240", 9)
    store.observe(sourceGrades: [gradeRow("16778240", revision: 2, applied: 8, live: 9)], sources: Array(box.ids.values),
                  now: t0.addingTimeInterval(2))
    RunLoop.current.run(until: Date().addingTimeInterval(0.3))
    expectEqual(core.requests("set-source-grade").count, 2, "P-a's grade never lands on P-b")
    expectEqual(store.summary(for: "16778240")?.identityChanged, true, "the store says the binding broke")
}

@MainActor static func testMeetingEndDropsZoomSessionGradesButNotCapture() {
    let core = FakeGradeCore(); core.setEpoch("101", 3); core.setEpoch("capture:cam", 0)
    let guest = GradeSourceIdentity.zoom(participantId: "101", persistentId: "P-a", sourceGeneration: 1)
    let cam = GradeSourceIdentity.capture(deviceId: "cam")
    let store = gradeStore(core, IdentityBox(["101": guest, "capture:cam": cam]))
    _ = awaitApply(store, SourceColorGrade(exposure: 2), "101", binding: guest.binding)
    _ = awaitApply(store, SourceColorGrade(exposure: 4), "capture:cam", binding: cam.binding)
    store.meetingEnded()
    expect(!store.isGraded("101"), "Zoom reuses ids per meeting: the session grade goes")
    expect(store.isGraded("capture:cam"), "a capture grade is not a meeting's")
    core.respawn(); core.setEpoch("101", 3); core.setEpoch("capture:cam", 0)
    store.coreGenerationChanged()
    store.observe(sourceGrades: [], sources: [guest, cam], now: Date(timeIntervalSince1970: 4_000))
    spin(until: { core.requests("set-source-grade").count >= 3 }, limit: 2)
    RunLoop.current.run(until: Date().addingTimeInterval(0.2))
    let resent = core.requests("set-source-grade").dropFirst(2).map { $0["sourceId"] as? String }
    expectEqual(resent, ["capture:cam"], "only the capture grade is re-asserted")
}

@MainActor static func testGradeFollowsThePersonToANewParticipantId() {      // D5
    let core = FakeGradeCore(); core.setEpoch("101", 3)
    let box = IdentityBox(["101": .zoom(participantId: "101", persistentId: "P-a", sourceGeneration: 1)])
    let store = gradeStore(core, box)
    _ = awaitApply(store, SourceColorGrade(exposure: 7), "101", binding: "person:P-a")
    box.ids = ["205": .zoom(participantId: "205", persistentId: "P-a", sourceGeneration: 2)]
    core.setEpoch("101", nil); core.setEpoch("205", 4)
    store.observe(sourceGrades: [gradeRow("101", revision: 1, applied: 3, live: 0, present: false)],
                  sources: Array(box.ids.values), now: Date(timeIntervalSince1970: 5_000))
    spin(until: { core.requests("set-source-grade").count >= 2 }, limit: 2)
    let moved = core.requests("set-source-grade").last ?? [:]
    expectEqual(moved["sourceId"] as? String, "205", "the grade followed P-a to the new id")
    expectEqual(FakeGradeCore.int((moved["grade"] as? JSONObject)?["exposure"]), 7, "the same grade")
    expect(store.isGraded("205") && !store.isGraded("101"), "the session grade moved, not copied")
}

@MainActor static func testAcknowledgedGradesPersistByStableKeyAfterTheDebounce() {
    let core = FakeGradeCore(); core.setEpoch("capture:cam", 0); core.setEpoch("777", 2)
    let cam = GradeSourceIdentity.capture(deviceId: "cam")
    let anonymous = GradeSourceIdentity.zoom(participantId: "777", persistentId: "", sourceGeneration: 1)
    let file = tempGradeFile()
    let store = gradeStore(core, IdentityBox(["capture:cam": cam, "777": anonymous]), file: file)
    _ = awaitApply(store, SourceColorGrade(exposure: 5), "capture:cam", binding: cam.binding)
    _ = awaitApply(store, SourceColorGrade(exposure: 8), "777", binding: anonymous.binding)
    RunLoop.current.run(until: Date().addingTimeInterval(0.3))
    expect(!FileManager.default.fileExists(atPath: file), "not written before the 750 ms debounce")
    spin(until: { FileManager.default.fileExists(atPath: file) }, limit: 2)
    let (saved, _) = SourceGradeFile.load(from: file)
    expectEqual(saved.grades["capture:cam"]?.exposure, 5, "saved under the stable key")
    expectEqual(saved.grades.count, 1, "a Zoom source without a persistentId is never persisted")
    // D6: Reset (neutral) keeps the session grade but forgets the saved one.
    _ = awaitApply(store, SourceColorGrade(), "capture:cam", binding: cam.binding)
    store.flush()
    expect(SourceGradeFile.load(from: file).file.grades["capture:cam"] == nil, "a neutral grade is not saved")
    expect(store.isGraded("capture:cam"), "but stays the session grade so a respawn stays consistent")
}

@MainActor static func testRelaunchReappliesCaptureButZoomOnlyAsDraft() {   // D2
    let file = tempGradeFile()
    var prior = SourceGradeFile()
    _ = prior.remember(SourceColorGrade(exposure: 4), key: "capture:cam")
    _ = prior.remember(SourceColorGrade(exposure: 8), key: "zoom-person:P-a")
    try! prior.save(to: file)
    let core = FakeGradeCore(); core.setEpoch("capture:cam", 0); core.setEpoch("101", 3)
    let cam = GradeSourceIdentity.capture(deviceId: "cam")
    let guest = GradeSourceIdentity.zoom(participantId: "101", persistentId: "P-a", sourceGeneration: 1)
    let store = gradeStore(core, IdentityBox(["capture:cam": cam, "101": guest]), file: file)
    store.coreGenerationChanged()                         // the launch handshake
    store.observe(sourceGrades: [], sources: [cam, guest], now: Date(timeIntervalSince1970: 6_000))
    spin(until: { core.requests("set-source-grade").count >= 1 }, limit: 2)
    RunLoop.current.run(until: Date().addingTimeInterval(0.2))
    let sets = core.requests("set-source-grade")
    expectEqual(sets.map { $0["sourceId"] as? String }, ["capture:cam"], "only the capture grade returns by itself")
    expectEqual(FakeGradeCore.int((sets.first?["grade"] as? JSONObject)?["exposure"]), 4, "the saved values")
    let seed = store.seed(for: "101")
    expectEqual(seed.grade.exposure, 8, "the Zoom grade seeds the editor")
    expectEqual(seed.draftNotice, "Saved grade loaded as a draft. Apply Live confirms this source binding.",
                "StudioViewModel.Grading.cs:89")
}

/// Drives the REAL AppModel hooks (rule 10): onConnected and applySnapshot.
@MainActor static func testOnConnectedAndSnapshotsReassertThroughTheModel() {
    let core = FakeGradeCore(); core.setEpoch("capture:cam", 0)
    let model = gradeModel()
    model.quitRequestForTesting = { body, _ in try await core.handle(body) }
    let devices: JSONObject = ["captureDevices": [["id": "cam", "name": "Cam", "connectionState": "connected"]]]
    model.applySnapshotForTesting(devices)
    let cam = GradeSourceIdentity.capture(deviceId: "cam")
    expectEqual(model.gradeIdentity(for: "capture:cam"), cam, "capture identity from the device list")
    _ = awaitApply(model.sourceGrades, SourceColorGrade(exposure: 7), "capture:cam", binding: cam.binding)
    core.respawn()
    var clock = Date(timeIntervalSince1970: 7_000)
    model.sourceGrades.now = { clock }
    model.onConnected()
    core.setEpoch("capture:cam", 0)
    model.applySnapshotForTesting(devices.merging(["sourceGrades": []]) { $1 })
    clock = clock.addingTimeInterval(0.6)
    model.applySnapshotForTesting(devices.merging(["sourceGrades": []]) { $1 })
    spin(until: { core.requests("set-source-grade").count >= 2 }, limit: 3)
    let reassert = core.requests("set-source-grade").last ?? [:]
    expectEqual(FakeGradeCore.int(reassert["expectedRevision"]), 0, "re-asserted on the respawned core")
    expectEqual(FakeGradeCore.int((reassert["grade"] as? JSONObject)?["exposure"]), 7, "the session grade")
}

@MainActor static func testMeetingEndThroughTheModelForgetsZoomGrades() {
    let core = FakeGradeCore(); core.setEpoch("101", 3)
    let model = gradeModel()
    model.quitRequestForTesting = { body, _ in try await core.handle(body) }
    model.applyZoomForTesting(["meetingState": "in-meeting",
                               "participants": [["userId": "101", "displayName": "G", "videoOn": true,
                                                 "persistentId": "P-a", "sourceGeneration": 1]]])
    let guest = model.gradeIdentity(for: "101")
    expectEqual(guest, .zoom(participantId: "101", persistentId: "P-a", sourceGeneration: 1), "Zoom identity from the roster")
    _ = awaitApply(model.sourceGrades, SourceColorGrade(exposure: 2), "101", binding: guest?.binding ?? "")
    model.applyZoomForTesting(["meetingState": ""])
    expect(model.sourceGrades.isGraded("101"), "an UNKNOWN meeting state never clears (CLAUDE.md #478 rule)")
    model.applyZoomForTesting(["meetingState": "idle"])
    expect(!model.sourceGrades.isGraded("101"), "meeting end forgets the guest's session grade")
}
```

**Mutations to run** (each must go red; name the test that catches it):
1. Send `sourceEpoch: 0` always → `testGradeApplyQueriesOnce…` (`sourceEpoch` 7).
2. Query before every apply → same test, query count 1.
3. Never update the stored revision after an acknowledgement → `set-source-grade` count 2 in the same test.
4. Treat `present` as `sourceEpoch != 0` → `testCaptureEpochZeroIsApplied…`.
5. Send every edit without the in-flight gate → `testOneApplyInFlight…`.
6. Keep the oldest pending instead of the newest → same test, exposure 3.
7. Retry in a loop until accepted → `testAConflictIsRequeried…`, count 5.
8. Skip the person check → `testTheWrongPersonIsRefused…`, count 1.
9. Drop the generation fence → `testACoreRespawnDuringAPendingApply…`.
10. Delete the `coreGenerationChanged()` call from `onConnected` → `testOnConnectedAndSnapshots…`.
11. Re-assert on every snapshot row (ignore `active`) → `testAnEpochChange…`, count 1 after the active row.
12. Bind Zoom grades by `gen:` only (Task 4 mutation 2) → `testAnEpochChange…` must then **re-assert
    for P-b**: confirm red on "P-a's grade never lands on P-b".
13. Clear Zoom grades on an unknown meeting state → `testMeetingEndThroughTheModel…`.
14. Save immediately (no debounce) → `testAcknowledgedGradesPersist…`, "not written before".
15. Re-apply saved Zoom grades at launch → `testRelaunchReapplies…`.
16. Remove the D5 follow → `testGradeFollowsThePerson…`.

- [ ] Step 1: tests red → Step 2: implement and wire into AppModel → Step 3: harness green; run
      ContractParity on `build-metal` (the new Swift literals must be recognised) → Step 4: mutations
      → Step 5: commit `mac: SourceGradeStore — fenced per-source apply, re-assert on every core generation (G1)`.

**Done when:** harness green, ContractParity green, mutation results recorded.

---

### Task 6 (shell, G1): the per-source editor, the Grade button, and the Program Look relabel

**Files:**
- Create: `mac-shell/Sources/CoreVideoProShell/GradeEditorModel.swift` (NEW)
- Create: `mac-shell/Sources/CoreVideoProShell/GradeEditorWindow.swift` (NEW)
- Modify: `mac-shell/Sources/CoreVideoProShell/AppModel.swift` (editor registry, slot → grade id, D6/D3 copy constants)
- Modify: `mac-shell/Sources/CoreVideoProShell/ShellApp.swift` (`WindowGroup` for editors; `SlotRow` Grade button `:1713-1773`; `PopoutWindow` constant; Program Look title `:146`)
- Modify: `mac-shell/Sources/CoreVideoProShell/ColorGrade.swift` (header and footer copy; fix the stale "±10" comment at `:46-48`: the core clamps to ±100, the Program Look's slider range stays ±10 by choice)
- Modify: `mac-shell/Sources/CoreVideoProShell/ShellTestsGrade.swift`

**Interfaces.**

```swift
@MainActor
final class GradeEditorModel: ObservableObject, Identifiable {
    let sourceId: String
    let sourceName: String
    let binding: String                         // the identity the draft was authored for
    var id: String { sourceId }
    var headerTitle: String { "Color grade - \(sourceName)" }      // ColorGradeEditorViewModel.cs:26
    @Published var lut: String
    @Published var exposure: Int
    @Published var contrast: Int
    @Published var saturation: Int
    @Published var temperature: Int
    @Published var liveEditing: Bool            // "Apply edits live", default true (CGE:46)
    @Published private(set) var status: String  // EditStatus, Windows strings
    @Published private(set) var isApplying: Bool
    static let previewNotice = "No private preview on this Mac build yet. Applied edits go to air."   // D3, spec §5.4
    static let resetNotice = "Reset leaves this source with its own neutral grade: it ignores the Program Look until the next launch."  // D6
    var currentGrade: SourceColorGrade
    init(sourceId: String, sourceName: String, binding: String,
         seed: SourceColorGrade, draftNotice: String?, store: SourceGradeStore)
    func edited()                       // after any control change; live → apply via the store
    func applyLive() async              // "Apply Live"
    func applyAndDone() async -> Bool   // "Apply and Done": true only when acknowledged
    func reset()                        // all axes 0, look "none"; then edited()
}

enum GradeEditorOpenResult { case opened(GradeEditorModel); case refused(String) }

extension AppModel {
    @Published var gradeEditors: [String: GradeEditorModel]   // at most 3, keyed by sourceId
    @Published var gradeNotice: String
    func gradeSourceId(forSlot slotId: Int) -> String?        // zoom → raw pid; capture → "capture:<id>"; else nil
    func gradeSourceName(for sourceId: String) -> String      // slot name, else roster/device name, else the id
    func openGradeEditor(sourceId: String?) -> GradeEditorOpenResult
    func closeGradeEditor(sourceId: String)
}

enum PopoutWindow { static let sourceGrade = "popout-source-grade" /* added */ }
```

SwiftUI:
- `WindowGroup("Color grade", id: PopoutWindow.sourceGrade, for: String.self) { $sourceId in SourceGradeEditorWindow(sourceId: sourceId ?? "") … }`
  (a value window: `openWindow(id:value:)` focuses the existing window for the same source).
- `SourceGradeEditorWindow` on appear calls `openGradeEditor`; when refused (for example a window
  restored by macOS at launch for a fourth source) it shows the refusal and a Close button; on
  disappear it calls `closeGradeEditor`.
- `SlotRow` gets `Button("Grade")` (ghost style, accessibility label `"Grade input \(slot.id)"`,
  disabled when the slot is unassigned). It calls `model.openGradeEditor(sourceId: model.gradeSourceId(forSlot: slot.id))`;
  `.opened(let editor)` → `openWindow(id: PopoutWindow.sourceGrade, value: editor.sourceId)`;
  `.refused(let text)` → `model.gradeNotice = text`, shown under the slot list.

**Behaviour** (Windows rulings, with sources):
- No source: `"Select a source before editing its color grade"` (`StudioViewModel.Grading.cs:68`).
- Fourth editor: `"Close one of the three grade editors before opening another"` (`:74`).
  Re-opening a source already open returns the same model (no count change).
- Seed and draft: from `store.seed(for:)`. A `draftNotice` turns `liveEditing` off and becomes the
  status (`:87-90`).
- Initial status `"Basic controls retain their established scale."`
  (`ColorGradeEditorViewModel.Advanced.cs:44`). The Mac `previewNotice` is a separate line, always
  shown until G5.
- Controls: "Built-in look" picker over `SourceColorGrade.looks`; Exposure, Contrast, Saturation,
  Temperature sliders −50…+50 step 1 with the Windows help lines (`CGE:52-55`); "Apply edits live"
  toggle; buttons "Apply Live", "Apply and Done", "Close / discard draft", "Reset" (`CGE:104,109-110`).
  The resetNotice sits under Reset.
- Live editing on: every edit calls the store (which keeps one in flight per source). Live off:
  edits stay local; "Apply Live" sends the current draft and stays open; "Apply and Done" sends and
  closes only on `.applied`; "Close / discard draft" sends nothing (`VM.cs:129-134`).
- Turning live editing back on sends the current grade once (`ColorGradeEditorViewModel.cs:57`).
- Statuses by outcome: `"Applying grade…"` while waiting (`Apply.cs:46`); `.applied(n)` →
  `"Live grade acknowledged · revision \(n)"` (`:52`); `.refused(t)` → `t`; `.stale(t)` → `t`;
  `.superseded` → unchanged. Buttons that send are disabled while `isApplying`.
- Program Look (D1): window title "Program Look"; header `"PROGRAM LOOK"`; footer line
  `"Sources without their own grade. Recording, stream and the multiview inherit it."`. Slider range
  and behaviour unchanged.

**Tests** (complete):

```swift
@MainActor static func testGradeEditorOpenRulesAreWindows() {
    let model = gradeModel()
    if case .refused(let text) = model.openGradeEditor(sourceId: nil) {
        expectEqual(text, "Select a source before editing its color grade", "Grading.cs:68")
    } else { expect(false, "a nil source must refuse") }
    for id in ["capture:a", "capture:b", "capture:c"] {
        if case .refused = model.openGradeEditor(sourceId: id) { expect(false, "\(id) should open") }
    }
    if case .opened(let again) = model.openGradeEditor(sourceId: "capture:b") {
        expect(again === model.gradeEditors["capture:b"], "re-opening returns the existing editor")
    } else { expect(false, "re-opening an open source must succeed") }
    if case .refused(let text) = model.openGradeEditor(sourceId: "capture:d") {
        expectEqual(text, "Close one of the three grade editors before opening another", "Grading.cs:74")
    } else { expect(false, "a fourth editor must refuse") }
    model.closeGradeEditor(sourceId: "capture:a")
    if case .refused = model.openGradeEditor(sourceId: "capture:d") { expect(false, "closing one frees a place") }
}

@MainActor static func testGradeTargetsTheFrameKeyOfTheSlotsSource() {
    let model = gradeModel()
    model.assignSlot(1, kind: "zoom", sourceId: "16778240", name: "Guest")
    model.assignSlot(2, kind: "capture", sourceId: "cam-1", name: "Cam")
    expectEqual(model.gradeSourceId(forSlot: 1), "16778240", "raw pid: the key Zoom frames carry (fact 4)")
    expectEqual(model.gradeSourceId(forSlot: 2), "capture:cam-1", "capture frame key")
    expectEqual(model.gradeSourceId(forSlot: 3), nil, "an unassigned slot has no grade target")
}

@MainActor static func testEditorLiveEditsApplyAndDraftEditsDoNot() {
    let core = FakeGradeCore(); core.setEpoch("capture:cam", 0)
    let model = gradeModel()
    model.quitRequestForTesting = { body, _ in try await core.handle(body) }
    model.applySnapshotForTesting(["captureDevices": [["id": "cam", "name": "Cam", "connectionState": "connected"]]])
    guard case .opened(let editor) = model.openGradeEditor(sourceId: "capture:cam") else { expect(false, "opens"); return }
    expect(editor.liveEditing, "Apply edits live is on by default")
    expectEqual(editor.status, "Basic controls retain their established scale.", "Windows initial status")
    expectEqual(editor.headerTitle, "Color grade - Cam", "Windows header")
    editor.exposure = 4; editor.edited()
    spin(until: { core.requests("set-source-grade").count == 1 }, limit: 2)
    expectEqual(FakeGradeCore.int((core.requests("set-source-grade").first?["grade"] as? JSONObject)?["exposure"]), 4,
                "a live edit goes to air")
    editor.liveEditing = false
    editor.exposure = 9; editor.edited()
    RunLoop.current.run(until: Date().addingTimeInterval(0.3))
    expectEqual(core.requests("set-source-grade").count, 1,
                "a draft edit never reaches set-source-grade (ColorGradeEditorViewModelTests.cs:68)")
    Task { await editor.applyLive() }
    spin(until: { core.requests("set-source-grade").count == 2 }, limit: 2)
    expectEqual(FakeGradeCore.int((core.requests("set-source-grade").last?["grade"] as? JSONObject)?["exposure"]), 9,
                "Apply Live sends the draft")
    spin(until: { editor.status.hasPrefix("Live grade acknowledged") }, limit: 2)
    expectEqual(editor.status, "Live grade acknowledged · revision 2", "Apply.cs:52")
}

@MainActor static func testApplyAndDoneClosesOnlyOnAcceptanceAndDiscardSendsNothing() {
    let core = FakeGradeCore(); core.setEpoch("capture:cam", 0)
    let model = gradeModel()
    model.quitRequestForTesting = { body, _ in try await core.handle(body) }
    model.applySnapshotForTesting(["captureDevices": [["id": "cam", "name": "Cam", "connectionState": "connected"]]])
    guard case .opened(let editor) = model.openGradeEditor(sourceId: "capture:cam") else { expect(false, "opens"); return }
    editor.liveEditing = false
    editor.exposure = 6
    core.refuseNext("grade-capacity")
    var done: Bool?
    Task { done = await editor.applyAndDone() }
    spin(until: { done != nil }, limit: 2)
    expectEqual(done, false, "a refused apply does not close")
    expect(model.gradeEditors["capture:cam"] != nil, "the editor stays open")
    expectEqual(editor.status, "Grade was not applied: grade-capacity. Previous live grade retained.", "Apply.cs:50")
    done = nil
    Task { done = await editor.applyAndDone() }
    spin(until: { done != nil }, limit: 2)
    expectEqual(done, true, "an acknowledged apply closes")
    expect(model.gradeEditors["capture:cam"] == nil, "closed")
    guard case .opened(let reopened) = model.openGradeEditor(sourceId: "capture:cam") else { expect(false, "reopens"); return }
    expectEqual(reopened.exposure, 6, "the acknowledged session grade seeds the next editor")
    reopened.liveEditing = false
    reopened.exposure = 30
    let sent = core.requests("set-source-grade").count
    model.closeGradeEditor(sourceId: "capture:cam")      // "Close / discard draft"
    RunLoop.current.run(until: Date().addingTimeInterval(0.2))
    expectEqual(core.requests("set-source-grade").count, sent, "discard sends nothing")
}

@MainActor static func testASavedZoomGradeOpensAsADraft() {
    let file = tempGradeFile()
    var saved = SourceGradeFile(); _ = saved.remember(SourceColorGrade(exposure: 8), key: "zoom-person:P-a")
    try! saved.save(to: file)
    let model = gradeModel(file: file)
    model.applyZoomForTesting(["meetingState": "in-meeting",
                               "participants": [["userId": "101", "displayName": "G", "videoOn": true,
                                                 "persistentId": "P-a", "sourceGeneration": 1]]])
    guard case .opened(let editor) = model.openGradeEditor(sourceId: "101") else { expect(false, "opens"); return }
    expect(!editor.liveEditing, "a saved Zoom grade is a draft (D2)")
    expectEqual(editor.exposure, 8, "seeded from the saved grade")
    expectEqual(editor.status, "Saved grade loaded as a draft. Apply Live confirms this source binding.", "Grading.cs:89")
}
```

`testASavedZoomGradeOpensAsADraft` requires `AppModel()` to load its saved-grade file at
construction (as it loads prefs), from `gradeFilePathForTesting` when set. Fixture invariant: each
case gets its own temp file through `gradeModel`, so no case can see another's saves.

**Visible proof:** `COREVIDEO_SHELL_TAB=sources COREVIDEO_SHELL_SNAPSHOT=/tmp/grade-sources.png`
(Grade buttons, minimum window), `COREVIDEO_OPEN_POPOUTS=grade` with a snapshot of the Program Look
window, and a staged editor shot: add `COREVIDEO_SHELL_GRADE=staged`, which (like
`COREVIDEO_SHELL_VCAM`, `AppModel.swift:799-813`) builds a never-started model and an editor for a
fake source and photographs it. `tier0/sources-rows-fit-the-page` must stay green with the new
button.

**Mutations to run:**
1. `gradeSourceId` returns `slot.sourceKey` → `testGradeTargetsTheFrameKey…`.
2. Count editors after insert (allow four) → open rules.
3. Send on every edit regardless of `liveEditing` → live/draft test, count 1.
4. Close on any outcome in `applyAndDone` → "a refused apply does not close".
5. Ignore `draftNotice` (always live) → saved-draft test.

- [ ] Steps: tests red → implement → harness + `SELFCHECK` + `lint-mac-design.sh` green → three PNGs
      attached → mutations → commit `mac: per-source grade editor, slot-row Grade, Program Look (G1)`.

**Done when:** all green, PNGs attached, mutation results recorded.

---

### Task 7 (native, G1): proof on the real core with real Zoom epochs, and the drill under grades

**Files:**
- Create: `scripts/qa/mac-source-grade-proof.py` (NEW)
- Create: `scripts/qa/test_mac_source_grade_proof.py` (NEW)
- Modify: `scripts/mac-show-drill.py` (optional flag `--source-grades`, default off)

**Interfaces.**

```
python3 scripts/qa/mac-source-grade-proof.py [--mode basic|advanced] [--seconds 3] [--out DIR]
  exit 0 = pass; 1 = fail; 2 = harness could not prove anything (missing ffmpeg, no roster, dark "before")
python3 scripts/mac-show-drill.py --seconds 40 --load 8 --source-grades
```

Python, in the proof script (importable by the unit test):

```python
BLACK_Y = 24          # limited-range black is 16; leave room for encoder noise
def black_fraction(y_plane: bytes, threshold: int = BLACK_Y) -> float: ...
def judge(before_left, before_right, after_left, after_right) -> dict:
    """Each argument is a list of Y planes (bytes). Returns {"verdict": "pass"|"fail"|"harness", "reasons": [str]}."""
```

**Behaviour.**
- Reuses `Core`, `load_env` and `FAKE_ENGINE` from `scripts/mac-show-drill.py` (load it with
  `importlib.util.spec_from_file_location`; the file name has hyphens). Two fake participants at
  `COREVIDEO_FAKE_ENGINE_FPS=60` pinned and printed (CLAUDE.md fake-engine rule).
- Joins exactly as the drill does, waits for participants `101` and `102` with video, and loads one
  scene with two `fixed` routes: `101` at `{x:0, y:0, width:0.5, height:1}`, `102` at
  `{x:0.5, y:0, width:0.5, height:1}`.
- Records window A (ungraded) for `--seconds` using the drill's recording recipe
  (`mac-show-drill.py:437-455`), then grades `101`:
  - `basic`: `query-source-grade` → `set-source-grade` with `{"lut":"none","exposure":-100}` (the
    shader adds −10, which is black for any input).
  - `advanced`: `register-grade-document` (query, then upload) with
    `{"version":2,"colorSpace":"rec709-sdr","intensity":1,"bypass":false,"operations":[{"id":"black","kind":"primaries","enabled":true,"intensity":1,"exposureStops":0,"contrast":1,"pivot":0.5,"saturation":1,"temperature":0,"tint":0,"lift":0,"gamma":1,"gain":0,"curves":[[{"x":0,"y":0},{"x":1,"y":1}],[{"x":0,"y":0},{"x":1,"y":1}],[{"x":0,"y":0},{"x":1,"y":1}],[{"x":0,"y":0},{"x":1,"y":1}]],"cubeText":"","cubeSha256":"","name":""}]}`
    (gain 0 makes `pow(max(v*0,0),1)` = 0, `MetalCompositorShaders.h:104`), then `set-source-grade`
    with `advanced: {documentRef}`.
- Polls `snapshot` until `sourceGrades` lists `101` with `active:true` and does not list `102`
  (2 s budget, else fail), then records window B.
- Decodes the last 30 frames of each recording with `ffmpeg -vf crop=iw/2:ih:0:0,format=gray` (and
  `crop=iw/2:ih:iw/2:0`) to raw Y planes, and calls `judge`.
- `judge` rules: `harness` if any list is empty, or the mean black fraction of either "before" half
  is ≥ 0.05 (the fake pattern's luma is ≥ 40, `fake-engine.cpp:409-421`, so a dark "before" means the
  run proved nothing); `fail` "graded source did not change" if any after-left frame's black
  fraction is < 0.98; `fail` "ungraded neighbour changed" if any after-right frame's black fraction
  is ≥ 0.05; else `pass`.
- Prints one JSON line with the verdict, the epochs used, the `sourceGrades` rows and the
  recordings' paths. Missing `ffmpeg` exits 2 loudly, never a silent skip (the drill's rule,
  `mac-show-drill.py:510-515`).
- `--source-grades` on the drill: after `configure`, for each wall participant, query and apply
  `{"lut":"warm-film","exposure":2}`, and print how many were accepted; the run then gates exactly
  as today. Default off, so Windows and existing CI runs are unchanged.

**Tests** (`scripts/qa/test_mac_source_grade_proof.py`, complete):

```python
import importlib.util, os, unittest
HERE = os.path.dirname(__file__)
spec = importlib.util.spec_from_file_location("proof", os.path.join(HERE, "mac-source-grade-proof.py"))
proof = importlib.util.module_from_spec(spec); spec.loader.exec_module(proof)

def plane(y, n=1000):
    return bytes([y]) * n

class Judge(unittest.TestCase):
    def test_pass_when_only_the_graded_half_goes_black(self):
        v = proof.judge([plane(120)] * 3, [plane(130)] * 3, [plane(16)] * 3, [plane(131)] * 3)
        self.assertEqual(v["verdict"], "pass", v)

    def test_fail_when_the_neighbour_also_goes_black(self):
        v = proof.judge([plane(120)] * 3, [plane(130)] * 3, [plane(16)] * 3, [plane(16)] * 3)
        self.assertEqual(v["verdict"], "fail")
        self.assertIn("ungraded neighbour changed", " ".join(v["reasons"]))

    def test_fail_when_the_graded_source_did_not_change(self):
        v = proof.judge([plane(120)] * 3, [plane(130)] * 3, [plane(120)] * 3, [plane(130)] * 3)
        self.assertEqual(v["verdict"], "fail")
        self.assertIn("graded source did not change", " ".join(v["reasons"]))

    def test_one_bright_frame_in_the_graded_window_fails(self):
        v = proof.judge([plane(120)] * 3, [plane(130)] * 3, [plane(16), plane(16), plane(120)], [plane(130)] * 3)
        self.assertEqual(v["verdict"], "fail")

    def test_harness_when_the_before_window_is_dark(self):
        v = proof.judge([plane(16)] * 3, [plane(130)] * 3, [plane(16)] * 3, [plane(130)] * 3)
        self.assertEqual(v["verdict"], "harness")

    def test_harness_when_a_window_is_empty(self):
        self.assertEqual(proof.judge([], [plane(130)], [plane(16)], [plane(130)])["verdict"], "harness")

    def test_black_fraction_threshold_is_inclusive(self):
        self.assertEqual(proof.black_fraction(plane(24)), 1.0)
        self.assertEqual(proof.black_fraction(plane(25)), 0.0)

if __name__ == "__main__":
    unittest.main()
```

**Mutations to run:**
1. `judge` uses the mean over the after-left window instead of every frame →
   `test_one_bright_frame…`.
2. Drop the precondition → `test_harness_when_the_before_window_is_dark`.
3. `<` instead of `<=` in `black_fraction` → threshold test.
4. In the core (temporarily, never committed): make `applyTo` stamp every layer → the real run
   fails with "ungraded neighbour changed". Record the run's output.

- [ ] Step 1: unit tests red → Step 2: implement → Step 3: real runs on this Mac:
      `--mode basic` and `--mode advanced` both pass; record the JSON lines → Step 4: drill
      `--load 8` with and without `--source-grades`, same binary, report both (fps, delivery,
      underruns, coreMutex over-budget) → Step 5: mutations → commit
      `qa: real-core proof that a source grade changes only its source (G1)`.

**Done when:** both modes pass on the real core with the fake engine; the drill with
`--source-grades` passes and matches the run without it within run-to-run noise (state the
numbers; a regression is a failure, never waived).

---

### Task 8 (native, G2): shared grade fixtures read by the core and C#

**Files:**
- Create: `contracts/grade.fixtures.json` (NEW)
- Modify: `native/tests/AdvancedGradeTest.cpp` (one case)
- Create: `native-shell/CoreVideoPro.MediaCore.Tests/GradeFixtureTests.cs` (NEW)

**Interfaces.** Fixture file shape (state it at the top of the file as a `"_doc"` string):

```json
{
  "_doc": "Shared verdicts for advanced grade documents (spec §7). portable = C# AdvancedGradeDocument and Swift after decode-with-defaults; coreRaw = native readAdvancedGrade on the raw JSON (defaults to portable). needsCubeParser fixtures are skipped by Swift until G3 (D8).",
  "documents": [ { "name": "...", "json": { }, "portable": "valid|invalid", "coreRaw": "valid|invalid", "needsCubeParser": false } ],
  "macEncoded": [ { "name": "...", "text": "<exact JSON text the Swift encoder writes>" } ],
  "presets": [ { "name": "...", "origin": "windows|mac", "text": "<preset JSON text>", "expect": { "lut": "...", "exposure": 0, "contrast": 0, "saturation": 0, "temperature": 0, "operations": 0 } } ]
}
```

**The fixtures** (complete list; `P(x)` below means a primaries operation with id `x` and every
field at its default: `enabled true, intensity 1, exposureStops 0, contrast 1, pivot 0.5,
saturation 1, temperature 0, tint 0, lift 0, gamma 1, gain 1`, four identity curves
`[{x:0,y:0},{x:1,y:1}]`, `cubeText ""`, `cubeSha256 ""`, `name ""`; write them out in full in the
file):

| name | document | portable | coreRaw |
|---|---|---|---|
| minimal | `{version:2, colorSpace:"rec709-sdr", intensity:1, operations:[]}` | valid | valid |
| primaries | `P("p1")` with `exposureStops 1.5, contrast 1.2, saturation 0.8, gamma 1.1, gain 0.9` | valid | valid |
| curve-off-grid | one `curves` op whose master channel is `[{0,0},{0.333,0.7},{1,1}]` | valid | valid |
| gamma-boundary | `P` with `gamma 0.1` | valid | valid |
| version-3 | minimal with `version 3` | invalid | invalid |
| hdr | minimal with `colorSpace "hdr"` | invalid | invalid |
| intensity-2 | minimal with `intensity 2` | invalid | invalid |
| nine-operations | `P("o1")` … `P("o9")` | invalid | invalid |
| duplicate-ids | `P("dup")`, `P("dup")` | invalid | invalid |
| unknown-kind | `P` with `kind "lift-gamma-gain"` | invalid | invalid |
| gamma-below | `P` with `gamma 0.05` | invalid | invalid |
| curve-unordered | master `[{0,0},{0.5,0.5},{0.4,0.6},{1,1}]` | invalid | invalid |
| curve-no-start | master `[{0.1,0},{1,1}]` | invalid | invalid |
| curve-17-points | master with 17 strictly increasing points from 0 to 1 | invalid | invalid |
| id-65-bytes | `P` with a 65-character id | invalid | invalid |
| enabled-string | `P` with `enabled "yes"` | invalid | invalid |
| operations-null | minimal with `operations: null` | invalid | invalid |
| operation-null | minimal with `operations: [null]` | invalid | invalid |
| missing-curves | `P` with no `curves` key | **valid** | **invalid** |
| missing-version | minimal without `version` | **valid** | **invalid** |
| missing-colorspace | minimal without `colorSpace` | **valid** | **invalid** |
| cube-identity | one `cube` op, `cubeText` = `"LUT_3D_SIZE 2\n0 0 0\n1 0 0\n0 1 0\n1 1 0\n0 0 1\n1 0 1\n0 1 1\n1 1 1\n"`, `cubeSha256 "c8bce4299c8606d5ca59a4724f46e484e430c42d506cfc2a3f30bbe84d5199cc"` | valid | valid |
| cube-bad-hash | cube-identity with `cubeSha256` = 64 zeros | invalid | invalid |
| cube-malformed | `cubeText "LUT_3D_SIZE 2\n0 0 0\n1 0 0\n"`, `cubeSha256 "d81447307a88d51b9d22bdb1d4e21fce030b5a0bbeeb9a11715e5b24fec7c8f0"`, `needsCubeParser: true` | invalid | invalid |

The three bold rows are the known divergence: C# and Swift fill defaults on decode and always
encode every field, so the core only ever receives complete documents; the core's raw parser is
strict (`AdvancedGrade.h:33,46-52`). The fixture states it rather than hiding it.

`macEncoded` (one entry, `"name": "primaries-gain-zero"`): the advanced document from Task 7's
`--mode advanced`, written as the Swift encoder writes it (keys sorted, every field present). Task 9
asserts Swift's encoding of that document is **semantically** equal to this text (parsed objects
equal), so byte formatting of numbers cannot make it flaky.

`presets` (two entries):
- `windows-warm-film`, origin `windows`, the text C#'s `CopyGradeJson` writes for
  `{Lut:"warm-film", Exposure:2}` with one default `P("w1")` document, including the get-only
  `summary` property and System.Text.Json's default escaping (`·` as `·`, `+` as `+`):
  `{"presetVersion":1,"grade":{"lut":"warm-film","exposure":2,"contrast":0,"saturation":0,"temperature":0,"advanced":{…P("w1") in full…,"version":2,"colorSpace":"rec709-sdr","bypass":false,"intensity":1},"summary":"Look warm-film · exposure +2 · contrast 0 · saturation 0"}}`
  (write the `advanced` object out in full, keys in C#'s declaration order). Expect
  `lut warm-film, exposure 2, operations 1`.
- `mac-punch`, origin `mac`, the text the Swift preset encoder writes for
  `{lut:"punch", exposure:-5, saturation:10}` with no document. Expect `lut punch, exposure -5,
  saturation 10, operations 0`.

**Tests.** Native (append to `native/tests/AdvancedGradeTest.cpp`; `readRepoFile` exists in
`ContractParityTest.cpp`: move it into a shared test header or duplicate the five-line helper, and
say which):

```cpp
TEST(AdvancedGrade, SharedFixturesHaveTheDeclaredVerdictInTheCore) {
  const auto fixtures = rpc::Json::parse(readRepoFile("contracts/grade.fixtures.json"));
  ASSERT_TRUE(fixtures && fixtures->isObject());
  int checked = 0;
  for (const auto& entry : fixtures->get("documents")->asArray()) {
    const auto name = entry.getString("name");
    const auto* expect = entry.get("coreRaw") ? entry.get("coreRaw") : entry.get("portable");
    std::shared_ptr<const modules::AdvancedGradeDocument> doc;
    const bool valid = modules::readAdvancedGrade(*entry.get("json"), doc) && doc;
    EXPECT_EQ(valid, expect->asString() == "valid") << name;
    ++checked;
  }
  EXPECT_EQ(checked, 24) << "fixture count: update this when adding fixtures";
  for (const auto& entry : fixtures->get("macEncoded")->asArray()) {
    const auto node = rpc::Json::parse(entry.getString("text"));
    ASSERT_TRUE(node) << entry.getString("name");
    std::shared_ptr<const modules::AdvancedGradeDocument> doc;
    EXPECT_TRUE(modules::readAdvancedGrade(*node, doc) && doc)
        << "the core must accept what the Mac shell writes: " << entry.getString("name");
  }
}
```

C# (`GradeFixtureTests.cs`; reuse the directory walk from `LifecycleContractTests.cs:11-17`):

```csharp
using System.Text.Json;
using CoreVideoPro.MediaCore.Models;
using Xunit;
namespace CoreVideoPro.MediaCore.Tests;
public sealed class GradeFixtureTests
{
    private static string Fixture()
    {
        var directory = new DirectoryInfo(AppContext.BaseDirectory);
        while (directory is not null)
        {
            var path = Path.Combine(directory.FullName, "contracts", "grade.fixtures.json");
            if (File.Exists(path)) return path;
            directory = directory.Parent;
        }
        throw new FileNotFoundException("contracts/grade.fixtures.json");
    }
    [Fact] public void PortableVerdictsMatchAdvancedGradeDocumentValidate()
    {
        using var file = JsonDocument.Parse(File.ReadAllText(Fixture()));
        var checkedCount = 0;
        foreach (var entry in file.RootElement.GetProperty("documents").EnumerateArray())
        {
            var name = entry.GetProperty("name").GetString();
            var expected = entry.GetProperty("portable").GetString() == "valid";
            bool valid;
            try {
                var doc = entry.GetProperty("json").Deserialize<AdvancedGradeDocument>(new JsonSerializerOptions(JsonSerializerDefaults.Web));
                doc!.Validate(); valid = true;
            } catch (Exception ex) when (ex is JsonException or ArgumentException or InvalidOperationException or NullReferenceException) { valid = false; }
            Assert.True(valid == expected, $"{name}: expected {(expected ? "valid" : "invalid")}");
            ++checkedCount;
        }
        Assert.Equal(24, checkedCount);
    }
    [Fact] public void TheMacEncodedDocumentValidatesInCSharp()
    {
        using var file = JsonDocument.Parse(File.ReadAllText(Fixture()));
        foreach (var entry in file.RootElement.GetProperty("macEncoded").EnumerateArray())
        {
            var doc = JsonSerializer.Deserialize<AdvancedGradeDocument>(entry.GetProperty("text").GetString()!, new JsonSerializerOptions(JsonSerializerDefaults.Web));
            doc!.Validate();
        }
    }
}
```

A Windows-only preset test (in `native-shell/CoreVideoPro.WinUI.Tests/AdvancedGradeEditorTests.cs`,
runs on Windows CI, cannot run here):

```csharp
[Fact] public void MacPresetsPasteOnWindows()
{
    // Fixture path: same directory walk as GradeFixtureTests.
    using var file = JsonDocument.Parse(File.ReadAllText(GradeFixturePath()));
    foreach (var entry in file.RootElement.GetProperty("presets").EnumerateArray().Where(e => e.GetProperty("origin").GetString() == "mac"))
    {
        var editor = Editor(); editor.LiveEditing = false;
        Assert.True(editor.PasteGradeJson(entry.GetProperty("text").GetString()!), entry.GetProperty("name").GetString());
        var expect = entry.GetProperty("expect");
        Assert.Equal(expect.GetProperty("lut").GetString(), editor.Lut);
        Assert.Equal(expect.GetProperty("exposure").GetInt32(), editor.Exposure);
        Assert.Equal(expect.GetProperty("saturation").GetInt32(), editor.Saturation);
        Assert.Equal(expect.GetProperty("operations").GetInt32(), editor.Document.Operations.Length);
    }
}
```

**Mutations to run:**
1. Flip one fixture's `portable` → the C# test red (proves the loop reads it).
2. Delete the `coreRaw` override on `missing-curves` → native red (proves the divergence is real, not
   a typo).
3. Change `checked` count expectation → red (guards silently dropped fixtures).
4. In `AdvancedGrade.h`, relax `gamma` to `>= 0.05` (never committed) → native `gamma-below` red.

- [ ] Steps: fixtures + tests → run native (`build-metal` and `build-stub`) and C# → mutations →
      commit `contracts: shared advanced-grade fixtures for core, C# and Swift (G2)`.

**Done when:** native and C# green (C# failing set still the baseline 23); WinUI test compiles on
the Windows rig (CI).

---

### Task 9 (shell, G2): the advanced document, register-before-apply, and presets

**Files:**
- Create: `mac-shell/Sources/CoreVideoProShell/AdvancedGrade.swift` (NEW)
- Modify: `mac-shell/Sources/CoreVideoProShell/SourceGrade.swift` (`advanced` field)
- Modify: `mac-shell/Sources/CoreVideoProShell/SourceGradeStore.swift` (register flow)
- Modify: `mac-shell/Sources/CoreVideoProShell/CoreCommands.swift` (`registerGradeDocument`)
- Modify: `mac-shell/Sources/CoreVideoProShell/ShellTestsGrade.swift` (restore the fake's register case)

**Interfaces.**

```swift
struct GradeCurvePoint: Codable, Equatable { var x: Double; var y: Double }

struct GradeOperation: Codable, Equatable, Identifiable {
    var id: String              // default: a new UUID, 32 lowercase hex, no dashes (C# Guid "N")
    var kind = "primaries"      // primaries | curves | cube
    var enabled = true
    var intensity = 1.0
    var exposureStops = 0.0, contrast = 1.0, pivot = 0.5, saturation = 1.0
    var temperature = 0.0, tint = 0.0, lift = 0.0, gamma = 1.0, gain = 1.0
    var curves: [[GradeCurvePoint]] = GradeOperation.identityCurves
    var cubeText = "", cubeSha256 = "", name = ""
    static let identityCurves: [[GradeCurvePoint]]
    var displayName: String     // name, else kind (C# DisplayName)
}

struct AdvancedGradeDocument: Codable, Equatable {
    var version = 2
    var colorSpace = "rec709-sdr"
    var bypass = false
    var intensity = 1.0
    var operations: [GradeOperation] = []
    func validate() throws                  // GradeValidationError, C# messages (AdvancedGradeDocument.cs:62-105)
    var indicator: String                   // C# AdvancedIndicator (ColorGradeEditorViewModel.Advanced.cs:62-64)
    func canonicalJSON() throws -> String   // JSONEncoder, .sortedKeys: the exact text hashed and sent
    static let maxCubeTextBytes = 1_500_000
}

enum GradeDocumentRef { static func sha256Hex(_ text: String) -> String }   // CryptoKit, lowercase hex

enum GradePreset {
    static let maxBytes = 16_500_000
    static func encode(_ grade: SourceColorGrade) throws -> String          // {"grade":…,"presetVersion":1}
    enum Pasted: Equatable { case grade(SourceColorGrade), document(AdvancedGradeDocument) }
    static func decode(_ text: String) throws -> Pasted                      // GradeValidationError on refusal
}

extension CoreCommands {
    static func registerGradeDocument(ref: String, json: String?) -> JSONObject   // root-level; documentJson only when uploading
}
```

**Behaviour.**
- Decoding is tolerant like C#: every key is optional and falls back to the C# default, **except** an
  explicit JSON `null` for `operations`, any operation, `curves` or a curve point, which throws
  (C# leaves `null` there and `Validate` refuses it; the contracts README says explicit null is
  invalid). Unknown keys are ignored (`summary` from a Windows preset).
- `validate()` checks, in C#'s order and with C#'s messages: version and colorSpace
  (`"Unsupported grade version or color space."`); intensity 0…1; at most 8 operations with unique
  non-empty ids of at most 64 UTF-8 bytes and a known kind; the 1.5 MB cube-text total; per-op
  ranges (`"Grade parameter is outside its supported range."`); cube text present and hash matching
  (D8: no parse); exactly four curves, each 2…16 points, endpoints at x = 0 and x = 1, x strictly
  increasing as `Float`, every coordinate 0…1.
- `SourceColorGrade.isNeutral` also requires `advanced == nil`. `summary(for:)` in the store now
  fills `indicator` from the document.
- The store, before any `set-source-grade` whose grade has a document: `validate()`;
  `canonicalJSON()` (refuse above 2,000,000 UTF-8 bytes with
  `"Grade was not applied: Grade document exceeds its 2 MB transport budget."`); ref = SHA-256 of
  that exact text; `register-grade-document {documentRef}`; if `present:false`, the same with
  `documentJson`; then `set-source-grade` with `advanced: {documentRef}`. **Every** apply registers,
  including re-assertions (the core cache is weak plus 16 recent, `AdvancedGrade.h:13-29`, and a
  respawned core has none).
- `GradePreset.decode`: over `maxBytes` → `"Grade document is too large."`; with `presetVersion` it
  must be 1 and carry `grade` (`"Unsupported grade preset."`), the grade is checked with
  `wireRange` and its document validated; without `presetVersion` the text is a bare document
  (`VM.Advanced.cs:158-177`).

**Tests** (complete):

```swift
static func loadGradeFixtures() -> [String: Any]? {
    let root = URL(fileURLWithPath: #filePath).deletingLastPathComponent()
        .deletingLastPathComponent().deletingLastPathComponent().deletingLastPathComponent()
    guard let data = try? Data(contentsOf: root.appendingPathComponent("contracts/grade.fixtures.json")) else { return nil }
    return try? JSONSerialization.jsonObject(with: data) as? [String: Any]
}

static func testAdvancedDocumentsHaveThePortableVerdicts() {
    guard let fixtures = loadGradeFixtures(), let documents = fixtures["documents"] as? [[String: Any]] else {
        expect(false, "contracts/grade.fixtures.json must be readable from the harness"); return
    }
    var checked = 0, skipped = 0
    for entry in documents {
        let name = entry["name"] as? String ?? "?"
        if entry["needsCubeParser"] as? Bool == true { skipped += 1; continue }   // D8, until G3
        let data = try! JSONSerialization.data(withJSONObject: entry["json"]!, options: [.fragmentsAllowed])
        let valid: Bool
        do { try JSONDecoder().decode(AdvancedGradeDocument.self, from: data).validate(); valid = true } catch { valid = false }
        expectEqual(valid, (entry["portable"] as? String) == "valid", "portable verdict: \(name)")
        checked += 1
    }
    expectEqual(checked, 23, "every non-cube-parser fixture was checked")
    expectEqual(skipped, 1, "exactly the cube-parser fixtures are deferred to G3")
}

static func testTheSwiftEncodingIsWhatTheCoreAccepts() {
    guard let fixtures = loadGradeFixtures(), let encoded = fixtures["macEncoded"] as? [[String: Any]],
          let text = encoded.first?["text"] as? String else { expect(false, "macEncoded fixture"); return }
    var op = GradeOperation(); op.id = "black"; op.gain = 0
    let document = AdvancedGradeDocument(operations: [op])
    let mine = try! document.canonicalJSON()
    let a = try! JSONSerialization.jsonObject(with: Data(mine.utf8)) as! NSDictionary
    let b = try! JSONSerialization.jsonObject(with: Data(text.utf8)) as! NSDictionary
    expect(a.isEqual(b), "Swift's encoding equals the fixture the native test feeds readAdvancedGrade")
    for key in ["version", "colorSpace", "bypass", "intensity", "operations"] {
        expect(a[key] != nil, "the core requires \(key) (AdvancedGrade.h:33-42)")
    }
    let curves = ((a["operations"] as? [NSDictionary])?.first?["curves"] as? [[Any]])
    expectEqual(curves?.count, 4, "four curve channels are always written (AdvancedGrade.h:51-52)")
}

static func testPresetsMoveBetweenPlatforms() {
    guard let fixtures = loadGradeFixtures(), let presets = fixtures["presets"] as? [[String: Any]] else {
        expect(false, "presets fixture"); return
    }
    for entry in presets {
        let name = entry["name"] as? String ?? "?"
        let expectValues = entry["expect"] as? [String: Any] ?? [:]
        guard case .grade(let grade)? = try? GradePreset.decode(entry["text"] as? String ?? "") else {
            expect(false, "\(name) must paste on the Mac"); continue
        }
        expectEqual(grade.lut, expectValues["lut"] as? String, "\(name) lut")
        expectEqual(grade.exposure, expectValues["exposure"] as? Int, "\(name) exposure")
        expectEqual(grade.saturation, expectValues["saturation"] as? Int ?? 0, "\(name) saturation")
        expectEqual(grade.advanced?.operations.count ?? 0, expectValues["operations"] as? Int, "\(name) operations")
        if entry["origin"] as? String == "mac" {
            let mine = try! GradePreset.encode(grade)
            let a = try! JSONSerialization.jsonObject(with: Data(mine.utf8)) as! NSDictionary
            let b = try! JSONSerialization.jsonObject(with: Data((entry["text"] as! String).utf8)) as! NSDictionary
            expect(a.isEqual(b), "the Mac preset fixture is exactly what the Mac writes")
        }
    }
    let tooBig = String(repeating: " ", count: GradePreset.maxBytes + 1)
    do { _ = try GradePreset.decode(tooBig); expect(false, "over the cap must refuse") }
    catch let e as GradeValidationError { expectEqual(e.message, "Grade document is too large.", "VM.Advanced.cs:160") }
    catch { expect(false, "unexpected error type") }
    do { _ = try GradePreset.decode("{\"presetVersion\":2,\"grade\":{\"lut\":\"none\"}}"); expect(false, "version 2 must refuse") }
    catch let e as GradeValidationError { expectEqual(e.message, "Unsupported grade preset.", "VM.Advanced.cs:163") }
    catch { expect(false, "unexpected error type") }
}

@MainActor static func testEveryApplyRegistersItsDocumentAndARespawnReuploads() {
    let core = FakeGradeCore(); core.setEpoch("capture:cam", 0)
    let cam = GradeSourceIdentity.capture(deviceId: "cam")
    let store = gradeStore(core, IdentityBox(["capture:cam": cam]))
    var op = GradeOperation(); op.id = "p1"; op.saturation = 0
    let grade = SourceColorGrade(lut: "none", advanced: AdvancedGradeDocument(operations: [op]))
    expectEqual(awaitApply(store, grade, "capture:cam", binding: cam.binding), .applied(revision: 1), "applied")
    let registers = core.requests("register-grade-document")
    expectEqual(registers.count, 2, "query, then upload")
    expect(registers[0]["documentJson"] == nil, "the first request only asks")
    let text = registers[1]["documentJson"] as? String ?? ""
    expectEqual(registers[1]["documentRef"] as? String, GradeDocumentRef.sha256Hex(text), "ref = SHA-256 of the exact text sent")
    let ref = ((core.requests("set-source-grade").last?["grade"] as? JSONObject)?["advanced"] as? JSONObject)?["documentRef"] as? String
    expectEqual(ref, GradeDocumentRef.sha256Hex(text), "the set carries the registered reference")
    _ = awaitApply(store, grade, "capture:cam", binding: cam.binding)
    expectEqual(core.requests("register-grade-document").count, 3, "a cached document is asked about, not re-uploaded")
    core.respawn(); store.coreGenerationChanged(); core.setEpoch("capture:cam", 0)
    store.observe(sourceGrades: [], sources: [cam], now: Date(timeIntervalSince1970: 8_000))
    spin(until: { core.requests("set-source-grade").count >= 3 }, limit: 2)
    expectEqual(core.requests("register-grade-document").count, 5, "the respawned core gets the document again")
    expectEqual(core.requests("set-source-grade").last?["sourceId"] as? String, "capture:cam", "and the grade")
    expect(core.documents.contains(GradeDocumentRef.sha256Hex(text)), "registered on the new core")
}

static func testExplicitNullsAreRefused() {
    for text in ["{\"version\":2,\"operations\":null}", "{\"version\":2,\"operations\":[null]}"] {
        let decoded = try? JSONDecoder().decode(AdvancedGradeDocument.self, from: Data(text.utf8))
        expect(decoded == nil || (try? decoded!.validate()) == nil,
               "\(text) is refused (AdvancedGradeEditorTests.cs UnsupportedAndMalformedDocuments…)")
    }
}
```

**Mutations to run:**
1. Decode `operations: null` as `[]` → `testExplicitNullsAreRefused` and the `operations-null`
   fixture.
2. Hash a re-encoded copy instead of the exact text sent → register test (`ref = SHA-256 of the
   exact text`) — the fake throws "Invalid grade document content.".
3. Register only on the first apply per source → the respawn re-upload assertion.
4. Omit `curves` when identity in `canonicalJSON` → `testTheSwiftEncodingIsWhatTheCoreAccepts`.
5. Compare curve x as `Double` instead of `Float` → add the fixture
   `{0,0},{0.1000000001,…},{0.1000000002,…},{1,1}` only if this mutation stays green; record.

- [ ] Steps: tests red → implement → harness green → mutations → commit
      `mac: advanced grade document, register-before-apply, cross-platform presets (G2)`.

**Done when:** harness green; the fixture count assertions match Task 8; mutation results recorded.

---

### Task 10 (shell, G2): the advanced panel, undo and redo, copy and paste, preset files

**Files:**
- Create: `mac-shell/Sources/CoreVideoProShell/GradeHistory.swift` (NEW)
- Modify: `mac-shell/Sources/CoreVideoProShell/GradeEditorModel.swift`
- Modify: `mac-shell/Sources/CoreVideoProShell/GradeEditorWindow.swift`
- Modify: `mac-shell/Sources/CoreVideoProShell/ShellTestsGrade.swift`

**Interfaces.**

```swift
/// Pure undo/redo. One entry per gesture (VM.Advanced.cs:16-23), 100 deep (:20, TrimHistory).
struct GradeHistory {
    static let depth = 100
    mutating func beginGesture()
    mutating func endGesture()
    mutating func remember(_ current: SourceColorGrade)   // no-op if already recorded in this gesture
    mutating func undo(from current: SourceColorGrade) -> SourceColorGrade?
    mutating func redo(from current: SourceColorGrade) -> SourceColorGrade?
    var canUndo: Bool { get }; var canRedo: Bool { get }
}

extension GradeEditorModel {
    @Published var advancedExpanded: Bool        // "Expand / collapse grade"
    @Published var document: AdvancedGradeDocument?
    @Published var selectedAdjustmentId: String?
    func setAdvancedExpanded(_ on: Bool)          // on → liveEditing = false, status "Draft — Apply Live sends this grade to the show."
    func addPrimaries()                           // "+ Primaries"; at 8 → status "A grade can contain up to eight adjustments."
    func moveSelected(_ direction: Int)           // "Up" (-1) / "Down" (+1)
    func duplicateSelected()                      // new id; respects the 8 limit
    func removeSelected()
    func editSelected(_ change: (inout GradeOperation) -> Void)   // validates, one history entry per gesture
    func setBypass(_ on: Bool); func setIntensity(_ value: Double)
    func undo(); func redo(); func resetAdvanced()   // resetAdvanced = empty document, one history entry
    func copyGrade() -> String                    // GradePreset.encode(currentGrade)
    func paste(_ text: String) -> Bool            // GradePreset.decode; one undo step; no live leak
    var indicator: String                         // document?.indicator ?? "No advanced adjustments"
}
```

Window additions: "ADVANCED GRADE · REC.709 SDR" header, "Bypass grade", "Overall grade intensity"
0…100 %, "+ Primaries", adjustment picker by `displayName`, "Up" "Down" "Duplicate" "Remove",
primaries controls with the Windows labels, ranges and help lines
(`Controls/GradeAdjustmentControl.cs:29-32`), "Adjustment enabled", "Adjustment intensity", "Undo"
"Redo" "Reset", "Copy grade" / "Paste grade" (plain-text pasteboard), "Save preset…" /
"Load preset…" (`NSSavePanel` / `NSOpenPanel`, extension `cvgrade`, 16.5 MB cap). For a `curves`
or `cube` operation the controls show enable and intensity and the line
"Curves and LUT editing arrive in a later update." (D9). Command replaces Control in shortcuts
(spec §5.4). Every control has an accessibility label.

**Behaviour.**
- Expanding turns live editing off and sets the Windows draft status (`VM.Advanced.cs:73-77`).
- Every structural edit validates the resulting document first; a refusal sets the status to the
  validation message and leaves the draft unchanged (`VM.Advanced.cs:80-85`).
- Undo restores the whole grade (basic and document) as one step; redo is cleared by a new edit;
  history keeps 100 entries.
- Paste of a preset is one undo step and, if live editing was on, is applied once with the final
  values: live editing is turned off for the restore and back on after (`VM.Advanced.cs:169-172`).
  A bare document paste replaces only the document.

**Tests** (complete):

```swift
static func testGradeHistoryIsOneStepPerGestureAndHundredDeep() {
    var history = GradeHistory()
    var current = SourceColorGrade()
    history.beginGesture()
    for value in 1...20 { history.remember(current); current.exposure = value }   // one slider drag
    history.endGesture()
    expectEqual(history.undo(from: current)?.exposure, 0, "a whole drag is one undo step")
    var history2 = GradeHistory()
    var grade = SourceColorGrade()
    for value in 1...150 { history2.remember(grade); grade.exposure = value % 50 }
    var undone = 0
    while let previous = history2.undo(from: grade) { grade = previous; undone += 1 }
    expectEqual(undone, GradeHistory.depth, "history keeps exactly 100 entries")
}

@MainActor static func testExpandingIsADraftAndUndoRestoresAWholePreset() {
    let core = FakeGradeCore(); core.setEpoch("capture:cam", 0)
    let model = gradeModel()
    model.quitRequestForTesting = { body, _ in try await core.handle(body) }
    model.applySnapshotForTesting(["captureDevices": [["id": "cam", "name": "Cam", "connectionState": "connected"]]])
    guard case .opened(let editor) = model.openGradeEditor(sourceId: "capture:cam") else { expect(false, "opens"); return }
    editor.lut = "warm-film"; editor.exposure = 2; editor.liveEditing = false; editor.edited()
    editor.setAdvancedExpanded(true)
    expect(!editor.liveEditing, "expanding makes edits a draft")
    expectEqual(editor.status, "Draft — Apply Live sends this grade to the show.", "VM.Advanced.cs:75")
    editor.addPrimaries()
    editor.editSelected { $0.saturation = 0 }
    RunLoop.current.run(until: Date().addingTimeInterval(0.2))
    expectEqual(core.requests("set-source-grade").count, 0, "nothing reaches air from a draft")
    expectEqual(editor.indicator, "Advanced adjustments active", "C# AdvancedIndicator")
    // Paste a preset as ONE undo step (port of AdvancedGradeEditorTests.cs:28).
    let preset = try! GradePreset.encode(SourceColorGrade(lut: "punch", exposure: 15))
    expect(editor.paste(preset), "paste accepted")
    expectEqual(editor.exposure, 15, "pasted values")
    expect(editor.document == nil, "a preset without a document clears the draft's document")
    editor.undo()
    expectEqual(editor.exposure, 2, "undo restores the basic values")
    expectEqual(editor.lut, "warm-film", "and the look")
    expectEqual(editor.document?.operations.count, 1, "and the document, in one step")
    editor.redo()
    expectEqual(editor.exposure, 15, "redo")
}

@MainActor static func testAPasteWithLiveEditingLeaksNoIntermediateValues() {
    let core = FakeGradeCore(); core.setEpoch("capture:cam", 0)
    let model = gradeModel()
    model.quitRequestForTesting = { body, _ in try await core.handle(body) }
    model.applySnapshotForTesting(["captureDevices": [["id": "cam", "name": "Cam", "connectionState": "connected"]]])
    guard case .opened(let editor) = model.openGradeEditor(sourceId: "capture:cam") else { expect(false, "opens"); return }
    expect(editor.liveEditing, "precondition: live")
    let preset = try! GradePreset.encode(SourceColorGrade(lut: "cool-broadcast", exposure: -7, contrast: 3, saturation: 4, temperature: -2))
    expect(editor.paste(preset), "paste accepted")
    spin(until: { core.requests("set-source-grade").count >= 1 }, limit: 2)
    RunLoop.current.run(until: Date().addingTimeInterval(0.3))
    let sets = core.requests("set-source-grade")
    expectEqual(sets.count, 1, "exactly one apply for a paste (VM.Advanced.cs:169-172)")
    let sent = sets.first?["grade"] as? JSONObject ?? [:]
    expectEqual(sent["lut"] as? String, "cool-broadcast", "with the final look")
    expectEqual(FakeGradeCore.int(sent["exposure"]), -7, "and the final exposure")
    expectEqual(FakeGradeCore.int(sent["temperature"]), -2, "and the final temperature")
}

@MainActor static func testTheEightAdjustmentLimitAndAnInvalidEditAreRefusedWithoutChange() {
    let model = gradeModel()
    guard case .opened(let editor) = model.openGradeEditor(sourceId: "capture:x") else { expect(false, "opens"); return }
    editor.setAdvancedExpanded(true)
    for _ in 0..<8 { editor.addPrimaries() }
    editor.addPrimaries()
    expectEqual(editor.document?.operations.count, 8, "at most eight")
    expectEqual(editor.status, "A grade can contain up to eight adjustments.", "VM.Advanced.cs:119")
    editor.duplicateSelected()
    expectEqual(editor.document?.operations.count, 8, "duplicate respects the limit")
    let before = editor.document
    editor.editSelected { $0.gamma = 0.05 }
    expectEqual(editor.document, before, "an out-of-range edit leaves the draft unchanged")
    expectEqual(editor.status, "Grade parameter is outside its supported range.", "C# Validate message")
    let ids = editor.document?.operations.map(\.id) ?? []
    editor.selectedAdjustmentId = ids[1]
    editor.moveSelected(-1)
    expectEqual(editor.document?.operations.first?.id, ids[1], "Up moves the selection one place")
}
```

**Visible proof:** the staged editor snapshot (`COREVIDEO_SHELL_GRADE=staged-advanced`) with three
adjustments, one a pasted curves operation showing the D9 line.

**Mutations to run:**
1. Record history on every `remember` (ignore the gesture) → history test, first assertion.
2. Unbounded history → "exactly 100".
3. Paste without suspending live editing → `testAPasteWithLiveEditing…`, count 1.
4. Expanding leaves live editing on → draft test.
5. Apply the edit before validating → "an out-of-range edit leaves the draft unchanged".

- [ ] Steps: tests red → implement → harness, `SELFCHECK`, lint green → snapshot → mutations →
      commit `mac: advanced grade panel, undo, copy/paste and .cvgrade presets (G2)`.

**Done when:** all green; snapshot attached; mutation results recorded.

---

## Seam for the Sources inspector (G4)

The Sources spec's S5 reserves a Grade section below Key on the Picture tab
(`docs/superpowers/specs/2026-11-15-mac-sources-rebuild-design.md:432-434`) and a row Grade action
(`:389`). G4 plugs in through exactly these, all introduced in G1 and used by the slot row first:

| Need (spec §5.8) | Interface |
|---|---|
| The slot's grade target | `AppModel.gradeSourceId(forSlot:) -> String?` |
| Open the editor from row, toolbar or inspector header | `AppModel.openGradeEditor(sourceId:) -> GradeEditorOpenResult`, then `openWindow(id: PopoutWindow.sourceGrade, value:)` |
| One-line current grade, indicator, live state | `SourceGradeStore.summary(for:) -> SourceGradeSummary?` (`line`, `indicator` from G2, `active`, `identityChanged`) |
| "graded" mark on the row | `SourceGradeStore.isGraded(_:)` |
| Inspector Reset | `GradeEditorModel.reset()` through an opened editor, or the store's `apply(SourceColorGrade(), …)` with the live binding |
| Zoom persistentId for slots | `RosterParticipant.persistentId` / `sourceGeneration` (Task 4) and `AppModel.gradeIdentity(for:)` |

G4 retires the slot-row button by moving the same call into the Sources table's row action; no
store change.

## Integration and owner checks

After Task 7 (G1) and again after Task 10 (G2), on the signed, installed build (a simulated check is
not proof, master plan §7):

1. **Two sources, one graded.** A capture camera and a Zoom guest on Program side by side. Grade the
   guest warmer. Program, the recording (decode it) and the stream show only the guest changed.
2. **Same person, new stream** (spec live check 2). Ask the guest to stop and restart video, then
   leave and rejoin. The grade returns each time (watch `GET /snapshot` → `sourceGrades` go
   `active:false` then `true`), and never lands on anyone else.
3. **Kill the core mid-show** (spec live check 3): `pkill corevideo-native`. After the respawn,
   every grade returns without operator action.
4. **Relaunch** (D2): quit and relaunch. The camera grade returns; the guest's grade opens as a
   draft with the Windows notice.
5. **Presets** (G2, spec live check 4): save a `.cvgrade` on the Mac, load it on Windows, and the
   reverse.
6. **Drill:** `python3 scripts/mac-show-drill.py --seconds 40 --load 8` and the same with
   `--source-grades` on the owner's Apple Silicon machine, judged on per-frame delivery. Record
   hardware, load and duration.

## Risks carried from the spec, and new ones

- **R3 (spec), confirmed in code:** a Zoom epoch change paints the grade neutral
  (`SourceGradeState.h:52,58`), and during that window the source shows **neither** its grade nor the
  Program Look (fact 5). The Mac re-asserts within one snapshot round trip plus one query
  (Task 5); live check 2 measures the gap. Windows still has the gap; file a Windows issue after the
  owner confirms on a live meeting.
- **R7 (spec):** the protocol list, the observation contract and the generated C# model change.
  All additive; Task 2's C# test and mutation 5 guard the Windows typed path. The Windows dev suite
  runs in CI only.
- **New, D6:** without a core `clear`, a reset source ignores the Program Look until relaunch. Copy
  says so; the override is cheap.
- **New, fixture divergence:** C# and Swift accept documents the core's raw parser refuses (missing
  defaults). Harmless because both encoders always write every field; the fixtures pin it so it
  cannot widen silently.
- **New, re-assert traffic:** a missing source is re-queried every 500 ms for 5 s, then every 2 s.
  A camera unplugged for an hour costs one cheap request every 2 s; acceptable, and it stops when
  the session grade or the source's identity goes away.
- **R1, R2, R4-R6, R8 (spec)** belong to G3, G5-G7 and are not touched here.

## Out of scope

- Curves editing, `.cube` import and the full Swift cube parser (G3); the Sources inspector hook
  (G4); private preview, compare, scopes and ROI (G5-G7).
- A core `clear` for source grades (D6 override only).
- Remote-control grade actions (Windows has none).
- Changing the Program Look's range or behaviour (D1 is a relabel only).
- Any Windows shell change beyond the two tests and the regenerated, additive observation model.
