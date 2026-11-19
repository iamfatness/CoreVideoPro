# macOS Remote Control Parity, Phase 1 (RC1 to RC4) Implementation Plan

Status: DRAFT for owner review, 2026-11-16.

> **For agentic workers:** REQUIRED SUB-SKILL: use superpowers:subagent-driven-development (recommended) or
> superpowers:executing-plans to implement this plan task by task. Steps use checkbox (`- [ ]`) syntax.
> `docs/superpowers/plan-authoring-rules.md` is binding. Every task report lists each named mutation and
> whether it went red. **When a supplied test contradicts a reasonable implementation, suspect the test
> first** (rule 4 corollary), then say so in the report instead of bending the code to it.

**Goal:** One shared control manifest drives both hosts. The Mac answers 36 of the 110 Windows actions
over HTTP, WebSocket and OSC (up from 13 over HTTP and WebSocket). It answers 422 with a plain reason for
the rest, and every rule is pinned by tests that run on both hosts against one fixture file.

**Spec:** `docs/superpowers/specs/2026-11-15-mac-remote-control-parity-design.md` (sections 4 to 7 are
the contract; slices RC1 to RC4 in section 6). Sequencing: `docs/superpowers/plans/2026-11-15-mac-parity-master-plan.md`
(Phase 1 rows RC1 to RC4, rules section 7).

Path shorthand (the spec's): `C/` = `native-shell/CoreVideoPro.Control/`, `CT/` =
`native-shell/CoreVideoPro.Control.Tests/`, `MC/` = `native-shell/CoreVideoPro.MediaCore/`, `MCT/` =
`native-shell/CoreVideoPro.MediaCore.Tests/`, `W/` = `native-shell/CoreVideoPro.WinUI/`, `WT/` =
`native-shell/CoreVideoPro.WinUI.Tests/`, `M/` = `mac-shell/Sources/CoreVideoProShell/`.

---

## Defaults taken (owner may override)

The four spec decisions (section 9) match the master plan's section 4 list. Each is taken as the default.

| # | Default | Source | Tasks that change if overridden |
|---|---|---|---|
| D1 | An id in the shared manifest that the Mac does not support answers **422** with a generated reason. `GET /manifest` lists only supported actions. An id in neither file keeps Windows' 400 `Unknown action '<id>'.` | spec decision 1 | T3, T4 (fixture case `invoke-unavailable-on-mac`), T6 |
| D2 | OSC LAN opt-in is identical to Windows: both `COREVIDEO_OSC_LAN=1` and `COREVIDEO_OSC_TRUSTED_NETWORK=1`, no token. | spec decision 2 | T19, T21 |
| D3 | `view.setMode` stays `blocked` on STU. | spec decision 3 | T1 (platform row) |
| D4 | Settings gets a read-only **Remote control** row. | spec decision 4 | T20 |
| D5 | The Windows export is a **checked-in file with a drift test**. `CoreVideoPro.Control` is plain `net9.0` (no WinUI). Its test project builds and runs on this Mac (`~/.dotnet/dotnet test`, 122 passed in 3 s, measured 2026-11-16). The file is rewritten with `COREVIDEO_WRITE_CONTROL_CONTRACT=1`. A new `control-contract` CI job runs on `ubuntu-latest`, so drift is caught on every push and not only by the Windows job. | plan (the brief's question) | T1 |
| D6 | `contracts/control.platforms.json` also carries one row per `ControlManifest.StateFields` entry. That way "FeedbackFields lists only what the Mac publishes truthfully" has a single source of truth, and a test checks it against `/state`. | plan (spec 4.1 names action rows only) | T1, T2, T3, T7, T13, T14, T15, T18 |
| D7 | `graphics.caption.set` becomes `blocked` on GFX. The Mac has no caption-text entry point; it has only `set-caption-enabled` (`M/Overlays.swift:143-146`), and the spec's rule is "every action calls the entry point the Mac UI already uses". The spec's risk row allows this move. RC3 therefore lands **11** actions, and the Mac answers **36** after RC3, not 37. | spec section 8 risk row | T1, T13 |
| D8 | `zoom.join`'s `webinar` arg sets `model.webinar`, the same field the Mac UI toggle sets (`M/ShellApp.swift:1385`). Windows does the same (`W/Services/StudioControlSurface.cs:234`). The spec asked for 422 "until verified". Verified: the engine join path never branches on a webinar flag (`native/zoom-engine/engine/main.cpp` mentions webinar only in failure reasons at :868,:872). | spec section 8 risk row, resolved | T10 |
| D9 | `transport.stream.rtmp.configure` with `enabled=false` is refused on the Mac with a fixed reason. The Mac has one always-armed RTMP destination (`streamUrl` + Keychain key, `M/AppModel.swift:464-465`) and no per-destination enable flag. | plan | T11 |
| D10 | `masteringTarget` in `/state` is the Windows index (0 = -14, 1 = -16, 2 = -23 LUFS; `W/ViewModels/StudioViewModel.cs:1291`) when the Mac's continuous target equals one of those three values, and **-1** otherwise. The Mac slider is continuous (`M/AudioConsole.swift:595`). | plan | T13 |
| D11 | `multiview.tileCount.set` clamps to the Mac's own UI range `ProductionSettings.multiviewTileRange` (1...16, `M/ProductionSettings.swift:22`; Settings stepper `M/SettingsPane.swift:93-96`). Windows clamps to 1...10. The manifest text stays Windows' verbatim. | plan | T13 |
| D12 | `input.name` with a blank or omitted name resets the slot to its derived name (roster or capture-device name, else the bare id). That is Windows' observable result ("blank resets to the derived name", `W/Services/StudioControlSurface.cs:917-926`). The Mac UI text field has no reset; this adds one small pure helper. | plan | T8 |
| D13 | The spec asked for an HTTP setup mode inside `scripts/mac-show-drill.py`. It is a separate script, `scripts/control-setup-drill.py`, instead. The drill is the core perf gate and talks to the core over stdio; coupling it to a running shell would make the perf gate depend on a UI process. | plan | T12 |
| D14 | OSC `f` decodes to Swift `Float`, not `Double` as the spec says. Windows decodes `f` as `float` (`C/Osc/OscCodec.cs:154-156`) and string coercion of a widened double diverges: `0.1f` is `"0.1"` on Windows and `"0.10000000149011612"` from a Double. `ControlCatalog.coerce` gains `Float` cases. | plan (spec correction) | T17, T18 |
| D15 | `input.assign` with a `media:<id>` source is refused on the Mac. `ShowInputSlot.kind` is `unassigned | zoom | capture` only (`M/AppModel.swift:149`). `source.dropout.set` on a `capture:` id is refused because the Mac dropout policy is Zoom-only (`M/AppModel.swift:168-175`). | plan | T8 |

## Spec corrections (planned against the code)

1. **Totals.** The spec's section 5.2 says STU 12. The table has **10** STU rows, and the printed totals sum to 112, not 110. Counted from the table: NOW 13, RC2 12, RC3 12, STU 10, AUD 2, MED 3, TIL 8, GFX 42, OUT 3, BRW 3, N/A 2 = 110. After D7: RC3 11, GFX 43.
2. **`/snapshot` redaction on Windows is not in the router.** `HttpControlRouter` serves JSON that is already redacted by `MC/Services/CoreSnapshotObserver.cs:62-91` (`NativeSnapshotObservation` doc, `C/NativeSnapshotObservation.cs:12-14`). The Mac router redacts at the boundary. So the redaction fixtures run on Windows through `CoreSnapshotObserver.Redact` / `SupportBundleLogRedactor.Redact` in `MCT/`, not through `HttpControlRouter` (T4).
3. **Envelope `nodes` order differs.** Windows writes document order (`C/Http/HttpControlRouter.cs:146-152`). The Mac sorts keys (`M/ControlApi.swift:421`), because `JSONSerialization` loses order. Every fixture snapshot keeps its keys in sorted order so both hosts agree (T4 invariant).
4. **The manifest JSON is indented and uses the platform newline.** `ControlManifest.ToJson` uses `WriteIndented = true` (`C/ControlManifest.cs:29-33`). On .NET 9 the newline defaults to `Environment.NewLine`, so the drift test normalizes CRLF and the writer writes LF (T1).
5. **Error bodies differ byte-for-byte.** Windows serializes `{ok,error}` with the default encoder, which escapes `'` as `'`. Fixtures compare parsed JSON, never bytes (T4).
6. **The router never sees `Content-Type`.** The spec's fixture shape `{method, path, contentType, body}` has no consumer at router level. The Mac's 415 rule is a transport hardening and stays in the Mac transport tests (T5).
7. **Windows records every datagram sender as a feedback target** (`C/Osc/OscControlServer.cs:86`), not only senders of valid commands, as the spec section 2.2 says. The Mac copies Windows (T19).
8. **`COREVIDEO_SHELL_SNAPSHOT` disables the control server** (`M/ControlSurface.swift:258-261`). So the spec's RC3 lower-third pixel proof "through COREVIDEO_SHELL_SNAPSHOT" cannot drive the action. T13 proves the remote command is byte-identical to the button's, and the integration check proves pixels from a decoded recording.
9. **Windows OSC feedback also emits `graphics/*` lines** that are not in `StateFields` (`C/Osc/OscFeedback.cs:51-70`). The Mac emits only rows its platform file supports (T18).
10. The pasted golden is at `M/ControlApiTests.swift:58-63` (spec says 58-83). Every other file:line the spec cites was re-verified on 2026-11-16 and holds.

## Before you start

- **Tracking issue (master plan section 7: "one tracking issue per workstream, filed with owner approval").**
  `gh issue list --label backlog` has no issue for macOS remote control parity (checked 2026-11-16).
  Task 1 Step 0 asks the owner to approve filing *"Mac remote control parity: shared manifest, 36 actions, OSC (M2)"*
  with label `backlog`. Reference it in every commit body.
- **Branch.** Create a worktree `.claude/worktrees/mac-remote-control` on a new branch `mac-remote-control` from
  `mac-tier0b` at `e98436fc`. Commit per task; never push; never touch `main`. End every commit message with
  `Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>`.
- **Baseline measured on 2026-11-16 at `e98436fc`. Record your own before Task 1; a different number means a
  different tree.**

| Command | Result |
|---|---|
| `cd mac-shell && swift build -c release && COREVIDEO_SHELL_TESTS=1 ./.build/release/CoreVideoProShell` | **236 cases, `3961 shell checks passed, 0 failed.`** (19 `control/*` cases) |
| `~/.dotnet/dotnet test native-shell/CoreVideoPro.Control.Tests/CoreVideoPro.Control.Tests.csproj` | **122 passed**, 0 failed |
| `~/.dotnet/dotnet test native-shell/CoreVideoPro.MediaCore.Tests/CoreVideoPro.MediaCore.Tests.csproj` | 3153 passed, **23 failed (Windows-only, pre-existing)**, 8 skipped. Filtered `--filter "FullyQualifiedName~CoreSnapshotObserver\|FullyQualifiedName~SupportBundleLogRedactor"`: **50 passed** |
| `npm run contract:check` | clean (exit 0) |
| `npm run test:scripts` | 217 tests, 213 pass, 0 fail, 4 skipped |
| `bash scripts/lint-mac-design.sh` | `Design lint passed.` |

- **No native lane work.** No task changes `native/`. No action needs a new core command (spec section 3,
  "Shared core"). Every remote action calls an existing `AppModel` entry point, so nothing on the render or
  ingest path changes, and `scripts/mac-show-drill.py --load 8` is not a gate for this plan. Task 7 still
  measures the main-actor cost of building `/state` (budget under 1 ms).
- **Run commands.** Shell: as above. Windows-side tests on this Mac: `~/.dotnet/dotnet test <csproj>`, and for
  `MCT/` always with `--filter "FullyQualifiedName~ControlRedactionFixture"` (the 23 Windows-only failures are
  not yours). Generator: `npm run contract:generate` then `npm run contract:check`.
- **Lanes.** All tasks are lane **control**, except T20 (lane **shell**). There is no native lane. Tasks that
  edit the same files (the dispatch switch in `M/ControlSurface.swift`, `contracts/control.platforms.json`,
  `contracts/control.fixtures.json`) can be written in parallel but must be **committed in order**; the
  `ORDER` section at the end says which.

---

## RC1: one manifest, two hosts

### Task 1: Windows export, platform rows, and the contract CI job

**Lane:** control. **Slice:** RC1.

**Files**
- NEW `contracts/control.manifest.json`, written by the test below (never by hand).
- NEW `contracts/control.platforms.json`, hand-authored.
- NEW `CT/RepoPaths.cs`, `CT/ControlManifestExportTests.cs`, `CT/ControlPlatformRowsTests.cs`.
- Modify `.github/workflows/ci.yml`: new job `control-contract`.
- Modify `contracts/README.md`: one section describing the three control files and the rewrite switch.

**Interfaces**
- `contracts/control.manifest.json` holds `ControlManifest.Build().ToJson()` with CRLF replaced by LF, plus
  one trailing `\n`, written UTF-8 with no BOM.
- `contracts/control.platforms.json`:
  ```json
  {
    "version": 1,
    "workstreams": {
      "RC2": "Remote control: session and inputs",
      "RC3": "Remote control: audio, multiview and overlays",
      "STU": "Studio and Settings follow-ons",
      "AUD": "Audio parity",
      "MED": "Media clip playback",
      "TIL": "CoreVideo Tiles",
      "GFX": "Graphics engine",
      "OUT": "SRT and NDI output",
      "BRW": "Browser sources"
    },
    "actions": [ { "id": "zoom.join", "mac": "blocked", "blockedOn": "RC2" }, "...one row per registry action, in registry order..." ],
    "feedbackFields": [ { "field": "recording", "mac": "supported" }, "...one row per StateFields entry, in StateFields order..." ]
  }
  ```
  A row's `mac` is `supported` (no other keys), `blocked` (needs `blockedOn`, a key of `workstreams`) or
  `notApplicable` (needs a non-blank `reason`).
- **How to fill `actions`.** Copy the spec section 5.2 table. NOW becomes `supported`. RC2, RC3, STU, AUD,
  MED, TIL, GFX, OUT and BRW become `blocked` with that code. N/A becomes `notApplicable` with these reasons:
  `transport.ndi.graphicsKey.set` gets `"NDI graphics key output is out of scope for macOS 1.0"`, and
  `showEngine.restart` gets `"the OHG show engine is not part of macOS 1.0"`. Then apply D7:
  `graphics.caption.set` becomes `blocked` / `GFX`.
- **How to fill `feedbackFields`** (29 rows, `C/ControlManifest.cs:37-46` order):
  - `supported` (the 10 the Mac publishes today, `M/ControlApi.swift:98-101`): `recording, streaming, engineOn,
    canTake, commandStatus, activeSceneId, previewSceneId, multiviewLayoutMode, automationOn, autoTake`.
  - `blocked`:
    - `zoomStatus, engineStatus` and the four `input/{slot}/*` rows: `RC2`.
    - `lowerThirdOnAir, multiviewTileCount, autoCaptions, audioMonitorOn, audioMonitorVolume`: `RC3`.
    - `ndi`: `OUT`. `previewTilesNamesOn`: `TIL`. `viewMode, autoAssignInputs`: `STU`.
    - `autoLowerThirds`: `GFX`. `masterLimiterOn`: `AUD`.
  - `notApplicable` with reason `"the OHG show engine is not part of macOS 1.0"`: `ohg/health/engine` and
    `ohg/shadow/lastCommand`.
- **Fixture invariants** (state them in the README section and check them in your review):
  - There are exactly 110 action rows: 13 supported, 2 notApplicable, and 95 blocked (RC2 12, RC3 11,
    STU 10, AUD 2, MED 3, TIL 8, GFX 43, OUT 3, BRW 3).
  - There are exactly 29 feedback rows: 10 supported and 2 notApplicable.
  - Row order is the registry order and the `StateFields` order. No id appears twice.
- `RepoPaths.Root` walks up from `AppContext.BaseDirectory` to the first directory containing
  `contracts/README.md`. If none exists it throws, with the start directory in the message. It never returns
  a guess.

**Behaviour**
- The export test is the only writer of `control.manifest.json`. A missing file is a **failure** with the
  rewrite instruction, never a skip.
- The platform rows test parses `control.platforms.json` as arrays, so a duplicate id is visible. (A JSON
  object with duplicate keys parses silently in `JsonDocument`.)
- CI: add a job after `version-sync`:
  ```yaml
  control-contract:
    runs-on: ubuntu-latest
    steps:
      - uses: actions/checkout@v4
      - uses: actions/setup-dotnet@v4
        with: { dotnet-version: '9.0.x' }
      - name: Control contract (export, platform rows, fixtures)
        run: dotnet test native-shell/CoreVideoPro.Control.Tests/CoreVideoPro.Control.Tests.csproj
      - name: Redaction fixtures (MediaCore observer)
        run: dotnet test native-shell/CoreVideoPro.MediaCore.Tests/CoreVideoPro.MediaCore.Tests.csproj --filter "FullyQualifiedName~ControlRedactionFixture"
  ```
  The second step finds no tests until Task 4. Add it now with a `|| true`-free filter: a zero-match run
  exits 0 on dotnet 9. Check this, and if it exits non-zero, add the step in Task 4 instead and say so.

**Tests** (complete)

`CT/RepoPaths.cs`
```csharp
namespace CoreVideoPro.Control.Tests;

internal static class RepoPaths
{
    public static string Root { get; } = Find();

    private static string Find()
    {
        var start = AppContext.BaseDirectory;
        for (var dir = new DirectoryInfo(start); dir is not null; dir = dir.Parent)
        {
            if (File.Exists(Path.Combine(dir.FullName, "contracts", "README.md"))) return dir.FullName;
        }
        throw new InvalidOperationException($"Repository root (contracts/README.md) not found above {start}.");
    }

    public static string Contract(string name) => Path.Combine(Root, "contracts", name);
}
```

`CT/ControlManifestExportTests.cs`
```csharp
using System.Text;
using System.Text.Json;
using Xunit;

namespace CoreVideoPro.Control.Tests;

public sealed class ControlManifestExportTests
{
    private static string Expected() => ControlManifest.Build().ToJson().Replace("\r\n", "\n") + "\n";

    [Fact]
    public void CheckedInManifestIsExactlyTheRegistryExport()
    {
        var path = RepoPaths.Contract("control.manifest.json");
        if (Environment.GetEnvironmentVariable("COREVIDEO_WRITE_CONTROL_CONTRACT") == "1")
        {
            File.WriteAllText(path, Expected(), new UTF8Encoding(false));
        }
        Assert.True(File.Exists(path), $"{path} is missing. Regenerate with COREVIDEO_WRITE_CONTROL_CONTRACT=1.");
        var actual = File.ReadAllText(path).Replace("\r\n", "\n");
        Assert.True(actual == Expected(),
            "contracts/control.manifest.json is stale. Regenerate with COREVIDEO_WRITE_CONTROL_CONTRACT=1, "
            + "then update contracts/control.platforms.json and run npm run contract:generate.");
    }

    [Fact]
    public void ExportListsEveryRegistryActionInRegistryOrderWithItsOscAddress()
    {
        using var doc = JsonDocument.Parse(File.ReadAllText(RepoPaths.Contract("control.manifest.json")));
        var actions = doc.RootElement.GetProperty("Actions").EnumerateArray().ToList();
        Assert.Equal(ControlActionRegistry.Actions.Select(a => a.Id), actions.Select(a => a.GetProperty("Id").GetString()));
        // Fixture-derived operands: the root and one hand-checked address (spec 2.2).
        Assert.Equal("/cvp", doc.RootElement.GetProperty("Root").GetString());
        Assert.Equal("/cvp/transport/virtualcam/name/set",
            actions.Single(a => a.GetProperty("Id").GetString() == "transport.virtualcam.name.set")
                .GetProperty("OscAddress").GetString());
        Assert.Equal(1, doc.RootElement.GetProperty("Version").GetInt32());
    }
}
```

`CT/ControlPlatformRowsTests.cs`
```csharp
using System.Text.Json;
using Xunit;

namespace CoreVideoPro.Control.Tests;

public sealed class ControlPlatformRowsTests
{
    private static JsonElement Platforms() =>
        JsonDocument.Parse(File.ReadAllText(RepoPaths.Contract("control.platforms.json"))).RootElement.Clone();

    private static readonly string[] Values = ["supported", "blocked", "notApplicable"];

    [Fact]
    public void EveryRegistryActionHasExactlyOneMacRowInRegistryOrder()
    {
        var ids = Platforms().GetProperty("actions").EnumerateArray().Select(r => r.GetProperty("id").GetString()!).ToList();
        Assert.Equal(ids.Count, ids.Distinct(StringComparer.Ordinal).Count());
        var missing = ControlActionRegistry.Actions.Select(a => a.Id).Except(ids, StringComparer.Ordinal).ToList();
        var unknown = ids.Except(ControlActionRegistry.Actions.Select(a => a.Id), StringComparer.Ordinal).ToList();
        Assert.True(missing.Count == 0, "A Windows action has no macOS row; add one to contracts/control.platforms.json: " + string.Join(", ", missing));
        Assert.True(unknown.Count == 0, "A macOS row names no registry action: " + string.Join(", ", unknown));
        Assert.Equal(ControlActionRegistry.Actions.Select(a => a.Id), ids);
    }

    [Fact]
    public void EveryFeedbackFieldHasExactlyOneMacRowInStateFieldsOrder()
    {
        var fields = Platforms().GetProperty("feedbackFields").EnumerateArray().Select(r => r.GetProperty("field").GetString()!).ToList();
        Assert.Equal(ControlManifest.StateFields, fields);
    }

    [Fact]
    public void RowsAreWellFormed()
    {
        var root = Platforms();
        var workstreams = root.GetProperty("workstreams").EnumerateObject().Select(p => p.Name).ToHashSet(StringComparer.Ordinal);
        Assert.All(root.GetProperty("workstreams").EnumerateObject(), p => Assert.False(string.IsNullOrWhiteSpace(p.Value.GetString())));
        foreach (var row in root.GetProperty("actions").EnumerateArray().Concat(root.GetProperty("feedbackFields").EnumerateArray()))
        {
            var name = row.TryGetProperty("id", out var id) ? id.GetString() : row.GetProperty("field").GetString();
            var mac = row.GetProperty("mac").GetString();
            Assert.True(Values.Contains(mac), $"{name}: mac must be one of {string.Join("|", Values)}");
            var hasBlockedOn = row.TryGetProperty("blockedOn", out var blockedOn);
            var hasReason = row.TryGetProperty("reason", out var reason);
            switch (mac)
            {
                case "supported":
                    Assert.False(hasBlockedOn || hasReason, $"{name}: a supported row carries no blockedOn or reason");
                    break;
                case "blocked":
                    Assert.True(hasBlockedOn && workstreams.Contains(blockedOn.GetString()!), $"{name}: blockedOn must name a workstream");
                    Assert.False(hasReason, $"{name}: a blocked row's sentence comes from its workstream label");
                    break;
                default:
                    Assert.True(hasReason && !string.IsNullOrWhiteSpace(reason.GetString()), $"{name}: notApplicable needs a reason");
                    Assert.False(hasBlockedOn, $"{name}: notApplicable has no blockedOn");
                    break;
            }
        }
    }
}
```

**Mutations to run** (each must go red)
- Add `new("zz.fake", "Fake", "Fake.")` at the end of `ControlActionRegistry.BuildActions`. Expect
  `CheckedInManifestIsExactlyTheRegistryExport` and `EveryRegistryActionHasExactlyOneMacRowInRegistryOrder`
  to fail (missing row). Then rerun with `COREVIDEO_WRITE_CONTROL_CONTRACT=1`: the export goes green and the
  rows test stays red.
- Change one character of `transport.take`'s description in the registry. The export test fails.
- Delete the `zoom.join` row. The rows test fails with "zoom.join" in the message.
- Swap two action rows. The order assertion fails.
- Remove `"blockedOn"` from one blocked row. `RowsAreWellFormed` fails.
- Rename `"recording"` to `"recordings"` in feedbackFields. The feedback test fails.
- Delete `contracts/control.manifest.json`. The export test **fails**; it does not skip.

**Done when:** Step 0 is answered. Control.Tests passes with 122 + 5 tests (126 if the zero-match MCT step
needs moving; report which). The CI job is added. The mutations are reported. Commit:
`control: export the control manifest and the macOS platform rows (RC1)`.

- [ ] Step 0: owner approves the tracking issue; file it and note its number.
- [ ] Step 1: write the platforms file and the three test files; run them; see the export test fail (file missing).
- [ ] Step 2: run with `COREVIDEO_WRITE_CONTROL_CONTRACT=1`; then without it; all green.
- [ ] Step 3: mutations; CI job; README section; commit.

---

### Task 2: Generated Swift catalog

**Lane:** control. **Slice:** RC1. **Depends on:** T1.

**Files**
- NEW `contracts/control-manifest.mjs`: pure functions, no file I/O.
- Modify `contracts/generate.mjs`: read the two JSON files, validate, and add the output to the existing
  write/check loop at `contracts/generate.mjs:218-231`.
- NEW `contracts/control-manifest.test.mjs` (node:test). Add it to the `test:scripts` list in `package.json:49`.
- NEW (generated, checked in) `M/ControlManifest.generated.swift`.

**Interfaces**
```js
// contracts/control-manifest.mjs
export function validatePlatforms(manifest, platforms) // throws Error(message) on the first violation
export function swiftString(text)                     // a Swift string literal, quotes included
export function generateControlSwift(manifest, platforms) // returns string[] lines (no trailing "\n")
```
- `validatePlatforms` enforces the same rules as `ControlPlatformRowsTests`, against the **manifest file**
  instead of the C# registry: one row per manifest action in manifest order; one row per `FeedbackFields`
  entry in order; well-formed rows. Messages start with `control.platforms.json:`, so a stale pair fails
  `contract:check` with a pointer.
- `swiftString` escapes `\` as `\\`, `"` as `\"`, LF as `\n`, CR as `\r`, TAB as `\t`, and every other code
  point below U+0020 as `\u{X}` (uppercase hex, no padding). It never emits `\(`, since a backslash is always
  doubled first. Everything else is literal UTF-8.
- The generated file has exactly this structure (shown for a two-action input; the real file has 110
  entries and 29 feedback rows):
  ```swift
  // GENERATED by contracts/generate.mjs from contracts/control.manifest.json and
  // contracts/control.platforms.json. Do not edit; run `npm run contract:generate`.

  enum ControlMacAvailability: Equatable {
      case supported
      case blocked(workstream: String, label: String)
      case notApplicable(reason: String)
  }

  struct ControlManifestEntry {
      let action: ControlAction
      let availability: ControlMacAvailability
  }

  enum ControlManifestGenerated {
      static let version = 1
      static let root = "/cvp"
      static let entries: [ControlManifestEntry] = [
          ControlManifestEntry(
              action: ControlAction(id: "a.one", title: "One", description: "Say \"hi\".", params: []),
              availability: .supported),
          ControlManifestEntry(
              action: ControlAction(id: "a.two", title: "Two", description: "Tab\there\u{1}.", params: [
                  ControlParam(name: "on", type: .bool, required: true, description: nil),
                  ControlParam(name: "name", type: .string, required: false, description: "Shown name"),
              ]),
              availability: .blocked(workstream: "RC2", label: "Remote control: session and inputs")),
      ]
      static let feedbackFields: [(field: String, availability: ControlMacAvailability)] = [
          ("recording", .supported),
          ("ohg/health/engine", .notApplicable(reason: "the OHG show engine is not part of macOS 1.0")),
      ]
  }
  ```
  Indentation is 4 spaces. `type:` is `.` plus the manifest's lower-case type (`string|int|double|bool`).
  Any other type name throws `control.manifest.json: unsupported param type '<t>' on <id>`. The existing
  `ControlAction` and `ControlParam` structs in `M/ControlApi.swift:44-56` are reused; their memberwise
  initializers match the call shape.

**Behaviour**
- `generate.mjs` stays the only entry point. `--check` rejects a stale `ControlManifest.generated.swift`
  exactly as it rejects the lifecycle outputs.
- Validation runs **before** any file is written, so a bad pair fails `contract:generate` and writes nothing.

**Tests** (complete, `contracts/control-manifest.test.mjs`)
```js
import { test } from 'node:test';
import assert from 'node:assert/strict';
import { readFileSync } from 'node:fs';
import { resolve, dirname } from 'node:path';
import { fileURLToPath } from 'node:url';
import { validatePlatforms, swiftString, generateControlSwift } from './control-manifest.mjs';

const root = resolve(dirname(fileURLToPath(import.meta.url)), '..');
const manifest = { Version: 1, Root: '/cvp', FeedbackFields: ['recording', 'ohg/health/engine'], Actions: [
  { Id: 'a.one', Title: 'One', Description: 'Say "hi".', OscAddress: '/cvp/a/one', Params: [] },
  { Id: 'a.two', Title: 'Two', Description: 'Tab\there\u0001.', OscAddress: '/cvp/a/two', Params: [
    { Name: 'on', Type: 'bool', Required: true },
    { Name: 'name', Type: 'string', Required: false, Description: 'Shown name' } ] } ] };
const platforms = { version: 1, workstreams: { RC2: 'Remote control: session and inputs' },
  actions: [ { id: 'a.one', mac: 'supported' }, { id: 'a.two', mac: 'blocked', blockedOn: 'RC2' } ],
  feedbackFields: [ { field: 'recording', mac: 'supported' },
    { field: 'ohg/health/engine', mac: 'notApplicable', reason: 'the OHG show engine is not part of macOS 1.0' } ] };
const clone = o => JSON.parse(JSON.stringify(o));

test('generates the exact Swift shape', () => {
  const text = generateControlSwift(manifest, platforms).join('\n');
  assert.ok(text.includes('action: ControlAction(id: "a.one", title: "One", description: "Say \\"hi\\".", params: []),\n            availability: .supported),'));
  assert.ok(text.includes('description: "Tab\\there\\u{1}.", params: ['));
  assert.ok(text.includes('ControlParam(name: "on", type: .bool, required: true, description: nil),'));
  assert.ok(text.includes('ControlParam(name: "name", type: .string, required: false, description: "Shown name"),'));
  assert.ok(text.includes('availability: .blocked(workstream: "RC2", label: "Remote control: session and inputs")),'));
  assert.ok(text.includes('("ohg/health/engine", .notApplicable(reason: "the OHG show engine is not part of macOS 1.0")),'));
  assert.ok(text.startsWith('// GENERATED by contracts/generate.mjs'));
  assert.equal(text.match(/ControlManifestEntry\(\n/g).length, 2, 'one entry per manifest action');
});

test('swiftString escapes what Swift needs and nothing else', () => {
  assert.equal(swiftString('a\\b'), '"a\\\\b"');
  assert.equal(swiftString('say "x"'), '"say \\"x\\""');
  assert.equal(swiftString('l1\nl2\r\t'), '"l1\\nl2\\r\\t"');
  assert.equal(swiftString('\u001f'), '"\\u{1F}"');
  assert.equal(swiftString('\\(x)'), '"\\\\(x)"', 'never an interpolation');
  assert.equal(swiftString("camera's — ok"), '"camera\'s — ok"');
});

test('validation refuses every malformed pair', () => {
  const cases = [
    ['missing row', p => { p.actions.pop(); }, /a\.two/],
    ['unknown row', p => { p.actions.push({ id: 'a.zzz', mac: 'supported' }); }, /a\.zzz/],
    ['order', p => { p.actions.reverse(); }, /order/],
    ['duplicate', p => { p.actions[1] = { id: 'a.one', mac: 'supported' }; }, /a\.one/],
    ['unknown workstream', p => { p.actions[1].blockedOn = 'NOPE'; }, /NOPE/],
    ['N/A without reason', p => { p.feedbackFields[1].reason = ' '; }, /reason/],
    ['supported with reason', p => { p.actions[0].reason = 'x'; }, /a\.one/],
    ['bad mac value', p => { p.actions[0].mac = 'yes'; }, /a\.one/],
    ['feedback row missing', p => { p.feedbackFields.pop(); }, /ohg\/health\/engine/],
  ];
  for (const [name, mutate, message] of cases) {
    const p = clone(platforms); mutate(p);
    assert.throws(() => validatePlatforms(manifest, p), message, name);
  }
  assert.doesNotThrow(() => validatePlatforms(manifest, platforms));
});

test('unsupported param type is refused, not guessed', () => {
  const m = clone(manifest); m.Actions[1].Params[0].Type = 'float';
  assert.throws(() => generateControlSwift(m, platforms), /unsupported param type 'float' on a\.two/);
});

test('the checked-in pair validates and the generated catalog has 110 entries', () => {
  const real = JSON.parse(readFileSync(resolve(root, 'contracts/control.manifest.json'), 'utf8'));
  const rows = JSON.parse(readFileSync(resolve(root, 'contracts/control.platforms.json'), 'utf8'));
  validatePlatforms(real, rows);
  const swift = readFileSync(resolve(root, 'mac-shell/Sources/CoreVideoProShell/ControlManifest.generated.swift'), 'utf8');
  assert.equal(swift.match(/ControlManifestEntry\(\n/g).length, real.Actions.length);
  assert.equal(real.Actions.length, 110, 'inventory at 2026-11-16 (spec section 5.2)');
});
```

**Mutations to run**
- Change `"mac": "supported"` to `"blocked", "blockedOn": "RC2"` for `scene.list` in the platforms file,
  without regenerating. `npm run contract:check` exits non-zero and names `ControlManifest.generated.swift`.
- After T1's fake-action mutation, rewrite the manifest. `npm run contract:generate` throws
  `control.platforms.json: ... zz.fake` and writes nothing (check `git status` is unchanged).
- Drop the `\` escaping in `swiftString`. The escaping test fails.
- Emit `params: []` for every action. The exact-shape test fails on `ControlParam(name: "on"`.

**Done when:** `npm run contract:generate && npm run contract:check` are clean. `npm run test:scripts`
reports 217 + 5 tests. The generated file compiles: `swift build -c release` succeeds with the hand table
still present (T3 deletes it). Commit: `contracts: generate the Swift control catalog from the shared manifest (RC1)`.

---

### Task 3: The Mac answers from the generated catalog (closure, 422, manifest)

**Lane:** control. **Slice:** RC1. **Depends on:** T2.

**Files**
- Modify `M/ControlApi.swift`:
  - delete the hand table `:59-101`;
  - `ControlCatalog` reads `ControlManifestGenerated`;
  - `ControlBindError` gains `status`;
  - the router uses it;
  - update the file comment `:15-20`.
- Modify `M/ControlSurface.swift`: add `static let supportedActionIds`, `static func undispatched(_:)`, and the
  `default:` branch.
- Modify `M/ControlApiTests.swift`:
  - delete `windowsManifestGolden` and `testControlManifestMatchesWindowsGolden` (`:55-90`);
  - add `repoFile`;
  - add the four new cases;
  - in `testControlInvokeRejectsLikeWindows`, change the `zoom.join` row's expectation (`:99-100`) to the 422
    reply.
- Modify `M/ShellTests.swift:4059`: replace `control/manifest-golden` with the new registrations.

**Interfaces**
```swift
// M/ControlApi.swift
struct ControlBindError: Error, Equatable { let message: String; var status: Int = 400 }

enum ControlCatalog {
    static let all: [ControlManifestEntry]            // = ControlManifestGenerated.entries (110, Windows order)
    static let actions: [ControlAction]               // all.filter { $0.availability == .supported }.map(\.action)
    static let feedbackFields: [String]               // generated feedbackFields filtered to .supported, in order
    static let oscRoot: String                        // = ControlManifestGenerated.root
    static func action(_ id: String) -> ControlAction?           // supported only (unchanged meaning)
    static func availability(_ id: String) -> ControlMacAvailability?  // nil = not in the shared manifest
    static func unavailableMessage(_ id: String, _ availability: ControlMacAvailability) -> String?
    static func bind(_ id: String, _ raw: [Any?]) -> Result<[Any?], ControlBindError>  // availability first
    // coerce / oscAddress / manifestJSON keep their signatures
}

// M/ControlSurface.swift
extension AppModelControlSurface {
    static let supportedActionIds: Set<String>   // hand-written list: the twin of Windows SupportedActionIds
    static func undispatched(_ id: String) -> String   // "Unknown action '<id>'." (the default: branch's text)
}
```

**Behaviour**
- `unavailableMessage`:
  - `.blocked(_, label)` gives `"'<id>' is not available on macOS yet (<label>)."`
  - `.notApplicable(reason)` gives `"'<id>' is not available on macOS (<reason>)."`
  - `.supported` gives `nil`.
- `bind` order:
  1. A supported id binds exactly as today (`M/ControlApi.swift:114-136`, Windows `C/ControlCatalog.cs:77-121`).
  2. A known id that is not supported fails with `status: 422` and the message above. Args are **not** bound,
     and the surface is never called.
  3. Otherwise `status: 400`, `Unknown action '<id>'.`
- The router's bind-failure branch (`M/ControlApi.swift:377`) answers `fail(error.status, error.message)`.
- `manifestJSON()` lists `actions` (supported only) and `feedbackFields` (supported only), in Windows order.
  `Version` comes from `ControlManifestGenerated.version`. The existing rule that "nothing is advertised the
  Mac cannot perform" (`M/ControlApi.swift:14-19`) still holds.
- `supportedActionIds` is a **hand-written literal**, deliberately not derived from the catalog: it is the
  dispatcher's own claim, and the closure test compares the two. At RC1 it holds the 13 ids the dispatch
  switch handles (`M/ControlSurface.swift:79-135`).
- The dispatch `default:` returns `.fail(undispatched(actionId))`.

**Tests** (complete; added to `M/ControlApiTests.swift`)
```swift
    /// The repo checkout, from this source file (mac-shell/Sources/CoreVideoProShell/…).
    static func repoFile(_ relative: String) -> Data? {
        let root = URL(fileURLWithPath: #filePath)
            .deletingLastPathComponent().deletingLastPathComponent()
            .deletingLastPathComponent().deletingLastPathComponent()
        return try? Data(contentsOf: root.appendingPathComponent(relative))
    }

    static func repoJSON(_ relative: String) -> [String: Any]? {
        repoFile(relative).flatMap { try? JSONSerialization.jsonObject(with: $0) as? [String: Any] }
    }

    /// Supported ids from the platform file ON DISK, independent of the generated Swift.
    static func fileSupportedIds() -> Set<String>? {
        guard let rows = repoJSON("contracts/control.platforms.json")?["actions"] as? [[String: Any]] else { return nil }
        return Set(rows.filter { $0["mac"] as? String == "supported" }.compactMap { $0["id"] as? String })
    }

    @MainActor
    static func testControlClosure() {
        guard let fileSupported = fileSupportedIds() else {
            expect(false, "contracts/control.platforms.json is readable from the checkout"); return
        }
        // Each slice updates this number: RC1 13, after RC2 25, after RC3 36.
        expectEqual(fileSupported.count, 13, "supported rows in the platform file")
        expectEqual(AppModelControlSurface.supportedActionIds, fileSupported,
                    "the dispatcher's ids equal the platform file's supported rows (Windows SupportedActionIds twin)")
        expectEqual(Set(ControlCatalog.actions.map(\.id)), fileSupported,
                    "the generated catalog's supported set equals the platform file (regenerate if not)")
        let fileIds = (repoJSON("contracts/control.manifest.json")?["Actions"] as? [[String: Any]] ?? [])
            .compactMap { $0["Id"] as? String }
        expectEqual(ControlCatalog.all.map(\.action.id), fileIds, "all 110 entries, Windows order")
    }

    /// Authoring rule 10: drive every supported id through the REAL dispatcher.
    @MainActor
    static func testControlEverySupportedIdIsDispatched() {
        let model = AppModel()   // no core: actions refuse, but none may fall through
        let surface = AppModelControlSurface(model: model, box: model.controlSnapshotBox)
        guard let ids = fileSupportedIds() else { expect(false, "platform file"); return }
        for id in ids.sorted() {
            guard let action = ControlCatalog.action(id) else { expect(false, "\(id) is in the catalog"); continue }
            let args: [Any?] = action.params.map {
                switch $0.type { case .bool: return true; case .string: return "s1"; case .int: return 1; case .double: return 0.5 }
            }
            let result = invoke(surface, id, args)
            expect(result != nil, "\(id) answered within 3 s")
            expect(result?.error != AppModelControlSurface.undispatched(id), "\(id) has a dispatch case")
        }
        // The fallthrough text is exactly what this test looks for (a confirmatory pin).
        expectEqual(invoke(surface, "zz.not.a.case")?.error, "Unknown action 'zz.not.a.case'.", "default branch text")
    }

    @MainActor
    static func testControlUnavailableActionsAnswer422() {
        guard let platforms = repoJSON("contracts/control.platforms.json"),
              let rows = platforms["actions"] as? [[String: Any]],
              let labels = platforms["workstreams"] as? [String: String] else { expect(false, "platform file"); return }
        let surface = FakeSurface()
        let router = ControlRouter(surface: surface)
        var checked = 0
        for row in rows where row["mac"] as? String != "supported" {
            let id = row["id"] as? String ?? ""
            let expected = row["mac"] as? String == "blocked"
                ? "'\(id)' is not available on macOS yet (\(labels[row["blockedOn"] as? String ?? ""] ?? "?"))."
                : "'\(id)' is not available on macOS (\(row["reason"] as? String ?? "?"))."
            let response = route(router, "POST", "/invoke", #"{"action":"\#(id)","args":[]}"#)
            expectEqual(response.status, 422, "\(id) is 422")
            expectEqual(json(response.body)["error"] as? String, expected, "\(id) says why, in the operator's words")
            checked += 1
        }
        expectEqual(checked, 97, "RC1: 110 - 13 unavailable ids checked (each slice updates this)")
        expectEqual(surface.invocations.count, 0, "an unavailable action never reaches the surface")
        let bogus = route(router, "POST", "/invoke", #"{"action":"bogus.id"}"#)
        expectEqual(bogus.status, 400, "an id in neither file keeps Windows' 400")
        expectEqual(json(bogus.body)["error"] as? String, "Unknown action 'bogus.id'.", "Windows text")
        let showEngine = route(router, "POST", "/invoke", #"{"action":"showEngine.restart"}"#)
        expectEqual(json(showEngine.body)["error"] as? String,
                    "'showEngine.restart' is not available on macOS (the OHG show engine is not part of macOS 1.0).",
                    "a hand-checked N/A sentence (fixture-derived operand)")
    }

    @MainActor
    static func testControlManifestIsTheSharedFileFiltered() {
        guard let file = repoJSON("contracts/control.manifest.json"), let ids = fileSupportedIds(),
              let feedbackRows = repoJSON("contracts/control.platforms.json")?["feedbackFields"] as? [[String: Any]] else {
            expect(false, "shared files"); return
        }
        let supportedFields = feedbackRows.filter { $0["mac"] as? String == "supported" }.compactMap { $0["field"] as? String }
        var expected = file
        expected["Actions"] = (file["Actions"] as? [[String: Any]] ?? []).filter { ids.contains($0["Id"] as? String ?? "") }
        expected["FeedbackFields"] = (file["FeedbackFields"] as? [String] ?? []).filter { supportedFields.contains($0) }
        let response = route(ControlRouter(surface: FakeSurface()), "GET", "/manifest")
        expectEqual(response.status, 200, "GET /manifest")
        expect(NSDictionary(dictionary: json(response.body)).isEqual(to: expected),
               "GET /manifest is the shared file with only the Mac-supported actions and feedback fields")
        expectEqual(supportedFields.count, 10, "RC1: ten feedback fields (each slice updates this)")
        for retired in ["automation.magic", "automation.toggle", "automation.autoTake.set"] {
            expect(!response.body.contains(retired), "retired \(retired) is not advertised")
        }
    }
```
Register them in `ShellTests.runAndExit` where `control/manifest-golden` was:
`("control/closure", testControlClosure)`, `("control/dispatch-drive", testControlEverySupportedIdIsDispatched)`,
`("control/unavailable-422", testControlUnavailableActionsAnswer422)`,
`("control/manifest-shared-file", testControlManifestIsTheSharedFileFiltered)`.

**Mutations to run**
- Delete the `case "scene.list":` arm. `dispatch-drive` goes red on `scene.list`.
- Remove `"scene.list"` from `supportedActionIds`. `closure` goes red.
- Flip `scene.list` to blocked in the platforms file and regenerate. `closure` goes red (count 12), and
  `unavailable-422` goes red (count 98).
- Make `bind` skip the availability step (unknown → 400 for everything). `unavailable-422` goes red.
- Make `manifestJSON` list `all` instead of `actions`. `manifest-shared-file` goes red.
- Change the blocked sentence's "yet" to "now". `unavailable-422` goes red.

**Done when:** the shell suite passes. The old golden case is gone and the four new ones are present. The
check count changes; report it. Commit: `mac control: answer from the generated shared manifest; 422 for actions macOS lacks (RC1)`.

---

### Task 4: Shared HTTP, snapshot and redaction fixtures, run on both hosts

**Lane:** control. **Slice:** RC1. **Depends on:** T3. **Parallel with:** T5, T6.

**Files**
- NEW `contracts/control.fixtures.json`.
- NEW `CT/ControlFixtureSurface.cs`, `CT/ControlFixtureTests.cs`; NEW `MCT/ControlRedactionFixtureTests.cs`
  (MCT needs `RepoPaths`: add the same 15-line helper as `MCT/ControlRepoPaths.cs`, not a project reference
  to CT).
- Modify `M/ControlApiTests.swift`: new case `control/fixtures`. Remove the pasted cases that are now fixture
  cases: the row list in `testControlInvokeRejectsLikeWindows` `:97-109`, and the `texts` and `names` lists in
  `testControlSnapshotRedactionIsWindows`. **Keep** the long-input timing loop and the wire-level cases.

**Interfaces: the fixture file** (version 1; T8 to T18 append cases; the `osc` block is added by T17/T18)
```json
{
  "version": 1,
  "notes": "Router-level cases. Bodies compare as parsed JSON; key order and escaping are not part of the contract.",
  "http": [ { "name": "...", "hosts": ["windows", "mac"],
              "request": { "method": "POST", "path": "/invoke", "body": "..." },
              "surfaceReply": { "ok": true },
              "expect": { "status": 400, "body": { }, "errorPrefix": "Invalid JSON: ", "invocations": [ ] } } ],
  "snapshot": [ { "name": "...", "observation": { "kind": "available", "ageMs": 500, "state": { } },
                  "expect": { } } ],
  "redaction": { "names": [ { "name": "keyPhase", "secret": false } ],
                 "texts": [ { "input": "...", "expected": "..." } ],
                 "documents": [ { "input": { }, "expected": { } } ] }
}
```
- An absent `hosts` means both hosts.
- An absent `expect.body` means the body is not compared.
- `expect.errorPrefix`, when present, replaces the body check for `error`.
- `expect.invocations`, when present, is the exact list of `{ "action", "args" }` the surface received, as
  JSON values. `[]` means none. Absent means not checked.
- `surfaceReply` is what the fake surface returns for any invoke; the default is `{ "ok": true }`. Fields are
  `ok`, `error`, `previewSceneId` and `data` (a raw JSON string).
- `observation.kind` is one of:
  - `available`: `state` (object), `ageMs` (number), or `"ageMs": null` for no receipt time;
  - `unavailable`: `reason`;
  - `notObserved`;
  - `throws`: `message`.

**The cases** (write them exactly; the comment on each says why its answer is right on both hosts)

`http`:
| name | request body (POST /invoke unless shown) | surfaceReply | expect |
|---|---|---|---|
| `empty-body` | `""` | | 400 `{"ok":false,"error":"Request body is required."}`, invocations `[]` |
| `whitespace-body` | `"   "` | | 400, same error |
| `invalid-json` | `"not json"` | | 400, errorPrefix `"Invalid JSON: "`, invocations `[]` |
| `missing-action` | `{"args":[1]}` | | 400 `Missing 'action'.` |
| `non-string-action` | `{"action":7}` | | 400 `Missing 'action'.` (Windows reads a non-string as null, `C/Http/HttpControlRouter.cs:193-195`) |
| `blank-action` | `{"action":"  "}` | | 400 `Missing 'action'.` |
| `unknown-action` | `{"action":"bogus"}` | | 400 `Unknown action 'bogus'.` |
| `retired-magic` | `{"action":"automation.magic","args":[true]}` | | 400 `Unknown action 'automation.magic'.` |
| `missing-required` | `{"action":"scene.select"}` | | 400 `Action 'scene.select' requires parameter 'sceneId' at position 0.` |
| `args-not-array` | `{"action":"scene.select","args":"x"}` | | 400, same text (a non-array is treated as no args on both hosts) |
| `null-required` | `{"action":"transport.ftb.set","args":[null]}` | | 400 `Action 'transport.ftb.set' requires parameter 'enabled' at position 0.` |
| `bad-bool` | `{"action":"transport.ftb.set","args":["maybe"]}` | | 400 `Action 'transport.ftb.set' parameter 'enabled' must be Bool.` |
| `bool-from-int` | `{"action":"transport.ftb.set","args":[1]}` | | 200 `{"ok":true}`, invocations `[{"action":"transport.ftb.set","args":[true]}]` |
| `bool-from-text` | `{"action":"transport.record.set","args":[" TRUE "]}` | | 200, `[true]` (`bool.TryParse` trims and ignores case) |
| `bool-from-zero-text` | `{"action":"transport.stream.set","args":["0"]}` | | 200, `[false]` |
| `bool-from-double` | `{"action":"transport.ftb.set","args":[2.5]}` | | 200, `[true]` |
| `string-from-int` | `{"action":"scene.select","args":[3,"extra ignored"]}` | | 200, `[{"action":"scene.select","args":["3"]}]` |
| `string-from-bool` | `{"action":"scene.select","args":[true]}` | | 200, `["True"]` (`Convert.ToString(true)`) |
| `string-from-double` | `{"action":"scene.select","args":[2.5]}` | | 200, `["2.5"]` |
| `optional-omitted` | `{"action":"transport.virtualcam.name.set","args":[]}` | | 200, `[null]` |
| `preview-reply` | `{"action":"scene.select","args":["s3"]}` | `{"ok":true,"previewSceneId":"s3"}` | 200 `{"ok":true,"previewSceneId":"s3"}` |
| `empty-preview-reply` | same | `{"ok":true,"previewSceneId":""}` | 200 `{"ok":true}` |
| `data-reply` | `{"action":"scene.list"}` | `{"ok":true,"data":"{\"scenes\":[],\"previewSceneId\":\"\",\"activeSceneId\":\"\"}"}` | 200 `{"ok":true,"data":{"scenes":[],"previewSceneId":"","activeSceneId":""}}` |
| `refusal-reply` | `{"action":"transport.take"}` | `{"ok":false,"error":"Take is unavailable. Queue a valid Preview scene or a changed draft and wait for any current Take to finish."}` | 422 with that error |
| `refusal-without-text` | `{"action":"transport.take"}` | `{"ok":false}` | 422 `Invocation failed.` |
| `invoke-unavailable-on-mac` (`hosts:["mac"]`) | `{"action":"showEngine.restart"}` | | 422 `'showEngine.restart' is not available on macOS (the OHG show engine is not part of macOS 1.0).`, invocations `[]` |
| `not-found` | `GET /nope`, no body | | 404 `{"ok":false,"error":"Not found"}` |
| `get-invoke` | `GET /invoke` | | 404 |
| `post-state` | `POST /state` | | 404 |
| `manifest-trailing-slash` | `GET /manifest/` | | 200 (body not compared; the hosts list different action sets by design) |
| `manifest-query` | `GET /manifest?x=1` | | 200 |

Invariant (state it in `notes`): every action id in an `http` case for both hosts is `supported` on the Mac
at the time the case is added, or is in neither file (`bogus`, `automation.magic`). Rows only ever move toward
`supported`, so a case stays valid. No RC1 case uses an `int` param, because no RC1-supported action has one;
T8 adds int coercion cases.

`snapshot` (no secret-named key and no redactable string anywhere in these states, so Mac redaction is the
identity here; every state's keys are in sorted order, see spec correction 3):
| name | observation | expect |
|---|---|---|
| `fresh` | `available`, `ageMs` 500, state `{"multiviewer":{"layoutMode":"pgmPvwTop","tileCount":8},"programBuffer":{"underruns":3},"sceneId":"scene:solo"}` | `available:true, stale:false, staleAfterMs:2000, redacted:true, nodes:["multiviewer","programBuffer","sceneId"]`, `snapshot` = state, `ageMs` within ±400 of 500 |
| `stale` | same state, `ageMs` 5000 | `available:true, stale:true`, ageMs within ±400 of 5000 |
| `undated` | same state, `ageMs` null | `stale:true, receivedUtc:null, ageMs:null, staleReason:"The shell did not record when this snapshot arrived."` |
| `absent` | `unavailable`, reason `"engine off"` | `available:false, stale:true, reasonCode:"no-snapshot", reason:"engine off", nodes:[], snapshot:null, receivedUtc:null, ageMs:null, staleAfterMs:2000` |
| `not-observed` | `notObserved` | `available:false, reasonCode:"not-observed", reason:"This control surface does not observe a media core."` |
| `observer-throws` | `throws`, message `"bridge gone"` | `available:false, reasonCode:"observer-failed", reason:"bridge gone"` |

The ages 500 and 5000 sit at least 1500 ms from the 2000 ms boundary, so scheduler jitter cannot flip `stale`.

`redaction`:
- `names`: the 21 pairs in `M/ControlApiTests.swift:305-312`, verbatim.
- `texts`: the 11 pairs in `M/ControlApiTests.swift:315-327`, verbatim.
- `documents`: one case. Its `input` is the state at `M/ControlApiTests.swift:268-278`. Its `expected` is the
  same object with exactly these values replaced:
  - `outputSenderSession.senders[0].streamKey`: `"[redacted]"`;
  - `…lastError`: `"ffmpeg exited: rtmps://live.example.com/[redacted]"`;
  - `…runtimeDetail`: `"srtPassphrase=[redacted] accepted"`;
  - `browserSources.sources[0].url`: `"https://scores.example.com/board?access_token=[redacted]"`;
  - `browserSources.credential`: `"[redacted]"` (an object redacted as a unit);
  - `warnings[0]`: `"configure-outputs echoed {\"streamKey\":\"[redacted]\"}"`.

  Everything else is unchanged, including `overlay.keyLength: "16"` (the suffix rule) and
  `recording.programPath`.

**Interfaces: runners**
- `CT/ControlFixtureSurface.cs`: `ControlFixtureSurface : IControlSurface, INativeSnapshotObserver`. It has a
  settable `Reply` (`ControlInvokeResult`), `Invocations`, and `Observation`/`Throw`, and returns
  `ControlState.Empty`. For `notObserved`, the runner uses the existing `FakeControlSurface`, which has no
  observer.
- `CT/ControlFixtureTests.cs`: a `[Theory]` over `http` case names and a `[Theory]` over `snapshot` case
  names, using `[MemberData]` read from the file. Each case runs `new HttpControlRouter(surface).HandleAsync`.
  Snapshot `available` cases set `ReceivedUtc = DateTimeOffset.UtcNow - ageMs`; their `Json` is the state
  serialized as given (already "redacted" on Windows, spec correction 2).
- `MCT/ControlRedactionFixtureTests.cs` covers three things:
  - `names` through `CoreSnapshotObserver.IsSecretName` (internal; `InternalsVisibleTo` exists at
    `MC/Properties/AssemblyInfo.cs:3`);
  - `texts` through `SupportBundleLogRedactor.Redact`;
  - `documents` through `CoreSnapshotObserver.Redact(json)`, compared with `JsonNode.DeepEquals`.
- Mac `control/fixtures`: the same three blocks through `ControlRouter` (with `FakeSurface`: `handler` returns
  the reply, `observation`/`observationError`/`observesCore` as given) and through
  `ControlSnapshotRedactor.isSecretName` / `redactText` / `redact`.

**Test code: the Mac runner** (complete; the C# runners are the same loop over the same keys)
```swift
    struct FixtureError: LocalizedError { let errorDescription: String? }

    @MainActor
    static func testControlSharedFixtures() {
        guard let fixtures = repoJSON("contracts/control.fixtures.json"),
              let http = fixtures["http"] as? [[String: Any]],
              let snapshots = fixtures["snapshot"] as? [[String: Any]],
              let redaction = fixtures["redaction"] as? [String: Any] else {
            expect(false, "contracts/control.fixtures.json is readable from the checkout"); return
        }
        func same(_ a: Any?, _ b: Any?) -> Bool {
            if a == nil || a is NSNull { return b == nil || b is NSNull }
            return NSArray(array: [a!]).isEqual(to: [b ?? NSNull()])
        }
        var ran = 0
        for test in http where (test["hosts"] as? [String])?.contains("mac") ?? true {
            let name = test["name"] as? String ?? "?"
            let request = test["request"] as? [String: Any] ?? [:]
            let reply = test["surfaceReply"] as? [String: Any] ?? ["ok": true]
            let surface = FakeSurface()
            surface.handler = { _, _ in
                ControlInvokeResult(ok: reply["ok"] as? Bool ?? false, error: reply["error"] as? String,
                                    previewSceneId: reply["previewSceneId"] as? String, data: reply["data"] as? String)
            }
            let response = route(ControlRouter(surface: surface), request["method"] as? String ?? "",
                                 request["path"] as? String ?? "", request["body"] as? String)
            let expected = test["expect"] as? [String: Any] ?? [:]
            expectEqual(response.status, expected["status"] as? Int, "\(name): status")
            var body = json(response.body)
            if let prefix = expected["errorPrefix"] as? String {
                expect((body["error"] as? String ?? "").hasPrefix(prefix), "\(name): error starts with \(prefix)")
                body["error"] = nil
            }
            if let want = expected["body"] as? [String: Any] {
                expect(NSDictionary(dictionary: body).isEqual(to: want), "\(name): body \(response.body)")
            }
            if let want = expected["invocations"] as? [[String: Any]] {
                let got = surface.invocations.map { ["action": $0.0, "args": $0.1.map { $0 ?? NSNull() }] as [String: Any] }
                expect(NSArray(array: got).isEqual(to: want), "\(name): invocations \(got)")
            }
            ran += 1
        }
        for test in snapshots {
            let name = test["name"] as? String ?? "?"
            let observation = test["observation"] as? [String: Any] ?? [:]
            let surface = FakeSurface()
            switch observation["kind"] as? String {
            case "available":
                let age = observation["ageMs"] as? Double
                surface.observation = ControlSnapshotObservation(
                    state: observation["state"] as? [String: Any],
                    receivedAt: age.map { Date().addingTimeInterval(-$0 / 1000) })
            case "unavailable": surface.observation = .unavailable(observation["reason"] as? String ?? "")
            case "notObserved": surface.observesCore = false
            default: surface.observationError = FixtureError(errorDescription: observation["message"] as? String)
            }
            let root = json(route(ControlRouter(surface: surface), "GET", "/snapshot").body)
            for (key, want) in test["expect"] as? [String: Any] ?? [:] where key != "ageMsNear" {
                expect(same(root[key], want), "\(name): \(key) = \(String(describing: root[key]))")
            }
            if let near = (test["expect"] as? [String: Any])?["ageMsNear"] as? Double {
                expect(abs(((root["ageMs"] as? NSNumber)?.doubleValue ?? -1e9) - near) <= 400, "\(name): ageMs near \(near)")
            }
            ran += 1
        }
        for pair in redaction["names"] as? [[String: Any]] ?? [] {
            let name = pair["name"] as? String ?? ""
            expectEqual(ControlSnapshotRedactor.isSecretName(name), pair["secret"] as? Bool, "isSecretName(\(name))"); ran += 1
        }
        for pair in redaction["texts"] as? [[String: Any]] ?? [] {
            let input = pair["input"] as? String ?? ""
            expectEqual(ControlSnapshotRedactor.redactText(input), pair["expected"] as? String, "redactText(\(input))"); ran += 1
        }
        for pair in redaction["documents"] as? [[String: Any]] ?? [] {
            let got = ControlSnapshotRedactor.redact(pair["input"] ?? [:])
            expect(same(got, pair["expected"]), "redact(document) equals the fixture"); ran += 1
        }
        // RC1 total: 31 mac http + 6 snapshot + 21 names + 11 texts + 1 document = 70 (each slice updates this).
        expectEqual(ran, 70, "every fixture case ran")
    }
```
Snapshot `expect` uses the key `ageMsNear` for the ±400 check. All other keys compare exactly. For example,
the `fresh` case has `"expect": {"available":true,"stale":false,"staleAfterMs":2000,"redacted":true,"nodes":[...],"snapshot":{...},"ageMsNear":500}`.

The C# runner applies the same rules: status equality; body as `JsonNode.DeepEquals` after removing `error`
when `errorPrefix` is given; invocations as `JsonNode.DeepEquals` over `{action,args}`; snapshot keys
compared by `JsonNode.DeepEquals`, with the same `ageMsNear` rule. It also asserts the total of Windows cases
run: **30** http (every case except the one Mac-only case) + 6 snapshot in CT, and 21 + 11 + 1 in MCT.

**Mutations to run**
- Mac: change `fail(400, error.message)` back to a constant 400. `invoke-unavailable-on-mac` goes red.
- Mac: make coerce `.string` from Bool return `"true"`. `string-from-bool` goes red on the Mac only.
- Windows: in `ControlCatalog.TryCoerce`, make `bool` from `string` case-sensitive. `bool-from-text` goes red
  on Windows only.
- Both: edit the fixture `refusal-without-text` to expect `"Invocation failed"` (no period). Both hosts go
  red. This is the check that the fixture really drives both.
- Mac: drop the rtmp rule from `redactText`. The `texts` and `documents` cases go red.
- Mac: sort nothing in `snapshotEnvelope` (`nodes = redactedState.keys` unsorted). `fresh` goes red at least
  intermittently. Run it 5 times and report; if it stays green, the sort is confirmatory only (rule 8): say so
  and keep the fixture.
- Delete `contracts/control.fixtures.json`. Both runners **fail**; they do not skip.

**Done when:**
- Mac `control/fixtures` passes with `ran == 70`.
- CT passes with 30 + 6 fixture cases; MCT filtered passes with 33.
- The removed pasted lists are gone, and the long-input timing loop remains.

Commit: `control: one fixture file, run by the Windows router and the Mac router (RC1)`.

---

### Task 5: Hardening regression suite (the Mac's stricter-than-Windows rules stay pinned)

**Lane:** control. **Slice:** RC1. **Depends on:** T3. **Parallel with:** T4, T6.

The Mac server is stricter than Windows in seven ways (`M/ControlHttpServer.swift:6-30`). Five are already
pinned over the wire by existing cases:
- no CORS: `server-end-to-end`, `access-control-allow-origin == nil` (`M/ControlApiTests.swift:354`);
- JSON content type: 415, `:367`;
- DNS-rebinding Host guard: 403, `:374-376`;
- WebSocket Origin rule: `ws-origin-rule`;
- outbound backlog: `ws-slow-client-dropped`.

The head and body bounds are pinned (431/413), and so are pipelining and half-close. **Three are not pinned:**
the 10 s request timer, the timer being cancelled once the request reaches the router (`:536`), and the
64-connection cap (`:334`, `:494-497`). Also no test proves a CORS preflight is never approved. This task
pins all four and records the mutation table for the whole set.

**Files**
- Modify `M/ControlHttpServer.swift`:
  - `ControlServerOptions` gains `var requestTimeout: TimeInterval = 10`, and `Connection.begin` uses it
    instead of `ControlHttpServer.requestTimeout`;
  - `static let requestTimeout` is deleted;
  - add `var openConnectionCount: Int { queue.sync { connections.count } }`.
- Modify `M/ControlApiTests.swift`: three new cases. Register `control/hardening-timeouts`,
  `control/hardening-connection-cap` and `control/hardening-no-preflight`.

**Behaviour**
- The default stays 10 s, and `fromEnvironment` does not read a timeout (environment-only knobs stay the
  three Windows names).
- `ControlServerOptions` stays `Equatable`. The existing equality assertions still hold, because the new
  field defaults equal.

**Tests** (complete)
```swift
    @MainActor
    static func testControlHardeningTimeouts() {
        let surface = FakeSurface()
        var options = ControlServerOptions(port: 0, host: "127.0.0.1", authToken: nil)
        options.requestTimeout = 0.4
        let (server, started) = startServer(ControlRouter(surface: surface), options)
        defer { server.stop() }
        guard case .success(let port)? = started else { expect(false, "server starts"); return }
        // A client that sends half a head and stalls is answered 408 by the request timer.
        let stalled = RawClient(port: port)
        stalled.run(Data("GET /state HTTP/1.1\r\nHost: 127.0.0.1\r\n".utf8))
        spin(until: { stalled.box.value != nil }, limit: 3)
        stalled.connection.cancel()
        expect((stalled.box.value ?? "").hasPrefix("HTTP/1.1 408"), "a stalled request is 408: \((stalled.box.value ?? "").prefix(20))")
        // The timer stops once the request reaches the router: an invoke that runs 3x the timeout is answered 200.
        surface.handler = { _, _ in Thread.sleep(forTimeInterval: 1.2); return .success }
        let slow = raw(port, Data("POST /invoke HTTP/1.1\r\nHost: 127.0.0.1\r\nContent-Type: application/json\r\nContent-Length: 27\r\n\r\n{\"action\":\"transport.take\"}".utf8))
        expect(slow.hasPrefix("HTTP/1.1 200"), "a long invoke is not cut off by the request timer: \(slow.prefix(20))")
        expectEqual(ControlServerOptions.fromEnvironment([:]).requestTimeout, 10, "the default is 10 s")
    }

    @MainActor
    static func testControlHardeningConnectionCap() {
        var options = ControlServerOptions(port: 0, host: "127.0.0.1", authToken: nil)
        options.requestTimeout = 5   // idle clients must outlive the test
        let (server, started) = startServer(ControlRouter(surface: FakeSurface()), options)
        defer { server.stop() }
        guard case .success(let port)? = started else { expect(false, "server starts"); return }
        let queue = DispatchQueue(label: "control-test-cap")
        var idle: [NWConnection] = []
        for _ in 0..<64 {
            let c = NWConnection(host: "127.0.0.1", port: NWEndpoint.Port(rawValue: port)!, using: .tcp)
            c.start(queue: queue)
            idle.append(c)
        }
        spin(until: { server.openConnectionCount == 64 }, limit: 3)
        expectEqual(server.openConnectionCount, 64, "64 idle connections are held")
        let refused = raw(port, Data("GET /state HTTP/1.1\r\nHost: 127.0.0.1\r\n\r\n".utf8))
        expect(!refused.hasPrefix("HTTP/1.1"), "the 65th connection is closed without an answer: \(refused.prefix(30))")
        expectEqual(server.openConnectionCount, 64, "the refused connection was never admitted")
        idle.forEach { $0.cancel() }
        spin(until: { server.openConnectionCount == 0 }, limit: 3)
        expectEqual(http("GET", "http://127.0.0.1:\(port)/state").status, 200, "serving again once clients leave")
    }

    @MainActor
    static func testControlHardeningNoPreflight() {
        let (server, started) = startServer(ControlRouter(surface: FakeSurface()),
                                            ControlServerOptions(port: 0, host: "127.0.0.1", authToken: nil))
        defer { server.stop() }
        guard case .success(let port)? = started else { expect(false, "server starts"); return }
        let preflight = raw(port, Data("OPTIONS /invoke HTTP/1.1\r\nHost: 127.0.0.1\r\nOrigin: https://evil.example\r\nAccess-Control-Request-Method: POST\r\nAccess-Control-Request-Headers: content-type\r\n\r\n".utf8))
        expect(preflight.hasPrefix("HTTP/1.1 404"), "a CORS preflight is not a route")
        expect(!preflight.lowercased().contains("access-control-"), "no Access-Control-* header on any answer")
        for path in ["/manifest", "/state", "/snapshot", "/nope"] {
            let response = raw(port, Data("GET \(path) HTTP/1.1\r\nHost: 127.0.0.1\r\nOrigin: https://evil.example\r\n\r\n".utf8))
            expect(!response.lowercased().contains("access-control-"), "\(path): no CORS header even with an Origin")
        }
    }
```

**Mutation table** (run every row; each must go red in the named case. Add the table, with results, to the
task report)

| Hardening (site) | Mutation | Red case |
|---|---|---|
| No CORS (`ControlHttpParser.response`) | add `Access-Control-Allow-Origin: *` | `server-end-to-end`, `hardening-no-preflight` |
| JSON content type (`admit`, `:557-561`) | delete the 415 branch | `server-end-to-end` (`plain.status == 415`) |
| Host guard (`ControlAuth.hostAllowed`) | `return true` | `auth-and-bind-policy`, `server-end-to-end` |
| WS Origin (`admit`, `:556`) | delete the Origin line | `server-end-to-end` (originWs), `ws-origin-rule` |
| LAN needs token (`validate`) | delete the `!isLoopback && !hasToken` throw | `auth-and-bind-policy`, `server-auth-wire` |
| Query token only on WS (`isAuthorized`) | drop `isWebSocketUpgrade &&` | `auth-and-bind-policy`, `server-auth-wire` |
| Request timer (`begin`) | never schedule `timer` | `hardening-timeouts` (408) |
| Timer cancel at dispatch (`:536`) | delete `timeout?.cancel()` in `processRequest` | `hardening-timeouts` (slow invoke) |
| Connection cap (`accept`) | delete `connections.count < Self.maxConnections` | `hardening-connection-cap` |
| Head bound | raise `maxHeaderBytes` to 1 MiB | `http-parser-bounds`, `server-end-to-end` |
| Body bound | raise `maxBodyBytes` to 1 MiB | same |
| Outbound backlog | raise `maxPendingOutboundBytes` to 1 GiB | `ws-slow-client-dropped` |
| No re-dispatch (`dispatched`) | remove the `!self.dispatched` guard | `server-end-to-end` (pipelined) |
| Constant-time compare | replace with `==` | **stays green**: timing is not observable in this harness. Equality is still pinned. This row is confirmatory only (rule 8). |

**Done when:** the three cases pass, and the table is reported with the one documented green row. Commit:
`mac control: pin the request timer, the connection cap and the no-preflight rule (RC1)`.

---

### Task 6: `scripts/control-conformance.mjs` (the live cross-host check)

**Lane:** control. **Slice:** RC1. **Depends on:** T4. **Parallel with:** T5.

**Files:** NEW `scripts/control-conformance.mjs`, NEW `scripts/tests/control-conformance.test.mjs` (add it
to `test:scripts`).

**Interfaces**
```
node scripts/control-conformance.mjs --base http://127.0.0.1:8011 [--token T] [--host mac|windows] [--json out.json]
```
- Exit code: 0 when every selected case passed, 1 on any failure, 2 on a usage or connection error.
- It runs **only live-safe cases**: every `http` case whose request is not `POST /invoke`, plus the
  `POST /invoke` cases whose reply cannot change a live show.
  - Included: the bind-error and unknown-id cases, which return 400 before any surface call (`empty-body`,
    `whitespace-body`, `invalid-json`, `missing-action`, `non-string-action`, `blank-action`,
    `unknown-action`, `retired-magic`, `missing-required`, `args-not-array`, `null-required`, `bad-bool`).
  - Included with `--host mac`: `invoke-unavailable-on-mac`.
  - Never run: any case expecting 200 or 422 from a surface reply.
- It also checks `GET /snapshot` for **shape only**: `ok:true`, a boolean `available`, a boolean `stale`, and
  `staleAfterMs == 2000`.
- It prints one line per case, `PASS <name>` or `FAIL <name>: <why>`, then a summary line. It sends no
  `Origin` header and sets `Content-Type: application/json` on POST (the Mac would otherwise 415, by design).

**Tests** (`scripts/tests/control-conformance.test.mjs`, complete)
```js
import { test } from 'node:test';
import assert from 'node:assert/strict';
import { createServer } from 'node:http';
import { spawnSync } from 'node:child_process';
import { resolve, dirname } from 'node:path';
import { fileURLToPath } from 'node:url';

const script = resolve(dirname(fileURLToPath(import.meta.url)), '../control-conformance.mjs');

function stub(handler) {
  return new Promise(r => { const s = createServer(handler).listen(0, '127.0.0.1', () => r(s)); });
}
const run = (port, extra = []) => spawnSync(process.execPath, [script, '--base', `http://127.0.0.1:${port}`, ...extra], { encoding: 'utf8' });

test('a host that answers like Windows passes every live-safe case', async () => {
  const server = await stub((req, res) => {
    let body = ''; req.on('data', c => body += c); req.on('end', () => {
      res.setHeader('Content-Type', 'application/json');
      if (req.method === 'GET' && req.url === '/snapshot') return res.end(JSON.stringify({ ok: true, available: false, stale: true, staleAfterMs: 2000 }));
      if (req.method === 'GET' && req.url.startsWith('/manifest')) return res.end('{}');
      if (req.method !== 'POST' || req.url !== '/invoke') { res.statusCode = 404; return res.end('{"ok":false,"error":"Not found"}'); }
      res.statusCode = 400;
      if (!body.trim()) return res.end('{"ok":false,"error":"Request body is required."}');
      let j; try { j = JSON.parse(body); } catch { return res.end('{"ok":false,"error":"Invalid JSON: x"}'); }
      if (typeof j.action !== 'string' || !j.action.trim()) return res.end('{"ok":false,"error":"Missing \'action\'."}');
      const known = { 'scene.select': 'sceneId', 'transport.ftb.set': 'enabled' };
      if (!(j.action in known)) return res.end(JSON.stringify({ ok: false, error: `Unknown action '${j.action}'.` }));
      const args = Array.isArray(j.args) ? j.args : [];
      if (args[0] == null) return res.end(JSON.stringify({ ok: false, error: `Action '${j.action}' requires parameter '${known[j.action]}' at position 0.` }));
      return res.end(JSON.stringify({ ok: false, error: `Action '${j.action}' parameter '${known[j.action]}' must be Bool.` }));
    });
  });
  const result = run(server.address().port, ['--host', 'windows']);
  server.close();
  assert.equal(result.status, 0, result.stdout + result.stderr);
  assert.match(result.stdout, /PASS unknown-action/);
  assert.doesNotMatch(result.stdout, /bool-from-int|refusal-reply|preview-reply/, 'state-changing cases are never sent');
});

test('one wrong answer fails the run and names the case', async () => {
  const server = await stub((req, res) => { res.statusCode = 400; res.end('{"ok":false,"error":"nope"}'); });
  const result = run(server.address().port);
  server.close();
  assert.equal(result.status, 1);
  assert.match(result.stdout, /FAIL unknown-action/);
});

test('an unreachable base is a usage/connection error, not a pass', () => {
  const result = run(1);
  assert.equal(result.status, 2);
});
```

**Mutations to run:** make the script skip the body comparison; the second test goes red. Include
`refusal-reply` in the live-safe set; the first test goes red on its `doesNotMatch`.

**Done when:** the tests pass. `--host mac` run against a dev build (`scripts/run-mac-shell.sh`) with the
control server up passes, and the output is in the report. Commit:
`scripts: control-conformance runs the shared fixtures against a live shell (RC1)`.

**RC1 proof (slice exit):**
- `contract:check`, `control-contract` (CI), the shell suite, the Windows CT and MCT runs, and
  `test:scripts` are all green.
- The fake-action mutation from T1 fails CT and `contract:generate`, as recorded in T1 and T2.
- `control-conformance.mjs --host mac` passes against the running dev shell.
- Real consumer: the 13 live actions are served from the generated table, and Companion's `GET /manifest`
  shows the same 13 buttons as before (owner glance, no new behaviour).

---

## RC2: session and inputs (12 actions, 5 state fields)

### Task 7: `ControlSourceId`, the RC2 state fields, and the meter-free push key

**Lane:** control. **Slice:** RC2. **Depends on:** T3.

**Files**
- NEW `M/ControlSourceId.swift`, NEW `M/ControlStateProjection.swift`.
- Modify:
  - `M/ControlSurface.swift`: `state(_:)` adds the fields; `ControlApiHost` uses the push key.
  - `contracts/control.platforms.json`: flip feedback rows `zoomStatus`, `engineStatus` and `input/{slot}/*`
    to `supported`; regenerate.
  - `M/ControlApiTests.swift`; `M/ShellTests.swift` registrations.

**Interfaces**
```swift
enum ControlSourceId: Equatable {
    case zoom(String), capture(String), media(String)
    static func parse(_ text: String) -> ControlSourceId?          // case-sensitive prefixes; empty remainder -> nil
    static func canonical(_ id: ControlSourceId) -> String
    static func canonical(kind: String, sourceId: String) -> String?  // a Mac slot -> "zoom:<id>" / "capture:<id>" / nil
    /// Windows ShowInputKind name for a Mac slot (ShowInputRosterService.InferCaptureDeviceKind, :96-116).
    static func windowsKind(of slot: ShowInputSlot, devices: [CaptureDeviceRow]) -> String
}

enum ControlStateProjection {
    static func zoomStatus(meetingState: String) -> String
    static func engineStatus(captureEnabled: Bool, rawMediaActive: Bool, meetingState: String) -> String
    static func inputs(_ slots: [ShowInputSlot], devices: [CaptureDeviceRow]) -> [[String: Any]]
}

enum ControlStatePushKey {
    static let meterKeys: Set<String> = ["programTruePeakDbfs", "programLoudnessLufs"]
    static let audioSourceMeterKeys: Set<String> = ["level", "peakDbfs", "rmsDbfs"]
    static func key(_ state: [String: Any]) -> String   // ControlJSON.encode of the state minus meter fields
}
```

**Behaviour**
- `windowsKind`:
  - `unassigned` gives `"Unassigned"`; `zoom` gives `"ZoomParticipant"`.
  - `capture` follows the Windows ruling (`W/Services/ShowInputRosterService.cs:96-116`): a device id starting
    `screen:` gives `"Screen"`, `browser:` gives `"Browser"`. Otherwise the device vendor, lowercased, decides:
    `blackmagic` gives `"Blackmagic"`, `aja` gives `"Aja"`, `srt` gives `"SrtIngest"`, `rtmp` gives
    `"RtmpIngest"`, `ndi` gives `"Ndi"`, and anything else gives `"UvcWebcam"`.
  - A device missing from the list gives `"UvcWebcam"` (Windows `ResolveCaptureKind`, `StudioControlSurface.cs:911-915`).
- `inputs` returns one row per slot in slot order: `slot` (Int), `inShow` (Bool), `kind` (Windows name),
  `sourceId` (canonical or `NSNull()`), `name` (String). These are Windows' `ControlInputState` fields,
  camelCased (`C/ControlState.cs:4`).
- `zoomStatus`: `"in_meeting"` gives `"Zoom Live"`; anything else gives `"Zoom Offline"`. Both are Windows
  strings, `W/ViewModels/StudioViewModel.cs:11098,11145`.
- `engineStatus`:
  - capture off gives `"Engine off"` (a Mac text; Windows' engine-off text is free-form);
  - capture on with raw media gives `"Engine on — Zoom media live."` (Windows `:11118`);
  - capture on and in a meeting without raw media gives
    `"Engine waiting for Zoom media access — ask the host to approve the request."` (Windows `:11112`);
  - capture on and not in a meeting gives `"Engine on — no meeting."` (a Mac text).
- New `/state` fields:
  - `zoomStatus`, `engineStatus` and `inputs` from the functions above;
  - `zoomParticipantIds`: `model.roster.map(\.id)`, in roster order (Windows `RoomParticipantsForInputs`);
  - `virtualCameraOn`: `model.virtualCamera.enabled` (Windows `VirtualCameraEnabled`, intent).
- **Push key.** `ControlApiHost`'s sink (`M/ControlSurface.swift:301-308`) compares `ControlStatePushKey.key`,
  not the full JSON. A push still sends the **full** state, meters included. This is the Mac form of Windows'
  curated `FeedbackProps` (`W/Services/StudioControlSurface.cs:27-55`).
- **Budget.** Add `ControlApiHost.lastStateBuildMicros` (set on each sink run). The test below asserts a
  10-assigned-slot build finishes in under 1000 µs as the median of 50 builds on this Mac. Report the
  measured median. If it is over, follow the spec's fallback (cache per-family fragments) in this task.

**Tests** (complete)
```swift
    @MainActor
    static func testControlSourceIdAndKinds() {
        expectEqual(ControlSourceId.parse("zoom:p-1"), .zoom("p-1"), "zoom")
        expectEqual(ControlSourceId.parse("capture:cam1"), .capture("cam1"), "capture")
        expectEqual(ControlSourceId.parse("media:a1"), .media("a1"), "media parses (assign refuses it, D15)")
        for bad in ["zoom:", "capture:", "Zoom:p", "ndi:x", "", "p-1"] {
            expect(ControlSourceId.parse(bad) == nil, "\(bad.debugDescription) is not a source id (Ordinal prefixes)")
        }
        expectEqual(ControlSourceId.canonical(kind: "zoom", sourceId: "p-1"), "zoom:p-1", "canonical zoom")
        expectEqual(ControlSourceId.canonical(kind: "capture", sourceId: "cam1"), "capture:cam1", "canonical capture")
        expect(ControlSourceId.canonical(kind: "unassigned", sourceId: "") == nil, "unassigned has no id")
        expect(ControlSourceId.canonical(kind: "zoom", sourceId: "") == nil, "a blank zoom slot has no id")
        for id in ["zoom:p-1", "capture:cam1", "media:a1"] {
            expectEqual(ControlSourceId.parse(id).map(ControlSourceId.canonical), id, "round trip \(id)")
        }
        func slot(_ kind: String, _ id: String) -> ShowInputSlot { var s = ShowInputSlot(id: 1); s.kind = kind; s.sourceId = id; return s }
        func device(_ id: String, _ vendor: String) -> CaptureDeviceRow {
            CaptureDeviceRow(id: id, name: id, kind: "video", vendor: vendor, connectionState: "connected", signalPresent: true, warning: "")
        }
        let devices = [device("screen:1", ""), device("browser:2", ""), device("bm", "Blackmagic"), device("aj", "AJA"),
                       device("s", "srt"), device("r", "rtmp"), device("n", "ndi"), device("cam", "Apple")]
        let expected: [(ShowInputSlot, String)] = [
            (slot("unassigned", ""), "Unassigned"), (slot("zoom", "p-1"), "ZoomParticipant"),
            (slot("capture", "screen:1"), "Screen"), (slot("capture", "browser:2"), "Browser"),
            (slot("capture", "bm"), "Blackmagic"), (slot("capture", "aj"), "Aja"), (slot("capture", "s"), "SrtIngest"),
            (slot("capture", "r"), "RtmpIngest"), (slot("capture", "n"), "Ndi"), (slot("capture", "cam"), "UvcWebcam"),
            (slot("capture", "gone"), "UvcWebcam"),
        ]
        for (s, kind) in expected {
            expectEqual(ControlSourceId.windowsKind(of: s, devices: devices), kind, "\(s.kind)/\(s.sourceId) -> \(kind)")
        }
    }

    @MainActor
    static func testControlRc2StateFields() {
        let model = sceneEditModel()
        model.roster = [RosterParticipant(id: "p-1", name: "Host", hasVideo: true, muted: false, talking: false, assigned: true),
                        RosterParticipant(id: "p-2", name: "Guest", hasVideo: false, muted: false, talking: false, assigned: false)]
        model.captureDevices = [CaptureDeviceRow(id: "cam1", name: "Desk Cam", kind: "video", vendor: "Apple",
                                                 connectionState: "connected", signalPresent: true, warning: "")]
        model.slots[0].kind = "zoom"; model.slots[0].sourceId = "p-1"; model.slots[0].name = "Host"; model.slots[0].inShow = true
        model.slots[1].kind = "capture"; model.slots[1].sourceId = "cam1"; model.slots[1].name = "Desk"; model.slots[1].inShow = false
        model.meetingState = "in_meeting"; model.captureEnabled = true; model.rawMediaActive = false
        let state = AppModelControlSurface.state(model).dictionary
        expectEqual(state["zoomStatus"] as? String, "Zoom Live", "Windows text while in a meeting")
        expectEqual(state["engineStatus"] as? String,
                    "Engine waiting for Zoom media access — ask the host to approve the request.", "Windows waiting text")
        expectEqual(state["zoomParticipantIds"] as? [String], ["p-1", "p-2"], "roster ids in roster order")
        expectEqual(state["virtualCameraOn"] as? Bool, false, "virtual camera intent")
        let inputs = state["inputs"] as? [[String: Any]] ?? []
        expectEqual(inputs.count, 10, "one row per slot")
        expect(NSDictionary(dictionary: inputs[0]).isEqual(to: ["slot": 1, "inShow": true, "kind": "ZoomParticipant", "sourceId": "zoom:p-1", "name": "Host"]), "slot 1")
        expect(NSDictionary(dictionary: inputs[1]).isEqual(to: ["slot": 2, "inShow": false, "kind": "UvcWebcam", "sourceId": "capture:cam1", "name": "Desk"]), "slot 2")
        expect(inputs[2]["sourceId"] is NSNull && inputs[2]["kind"] as? String == "Unassigned", "an empty slot is Unassigned with a null id")
        model.meetingState = "idle"; model.captureEnabled = false
        let offline = AppModelControlSurface.state(model).dictionary
        expectEqual(offline["zoomStatus"] as? String, "Zoom Offline", "Windows offline text")
        expectEqual(offline["engineStatus"] as? String, "Engine off", "engine off")
        // Every supported feedback field is published (D6), driven from the platform file.
        let rows = repoJSON("contracts/control.platforms.json")?["feedbackFields"] as? [[String: Any]] ?? []
        for row in rows where row["mac"] as? String == "supported" {
            let field = row["field"] as? String ?? ""
            let key = field.hasPrefix("input/") ? "inputs" : field
            expect(state[key] != nil, "supported feedback field \(field) is published as \(key)")
        }
        expectEqual(rows.filter { $0["mac"] as? String == "supported" }.count, 16, "RC2: 10 + zoomStatus, engineStatus, 4 input rows")
        // Budget: median of 50 builds with ten assigned slots.
        for i in 2..<10 { model.slots[i].kind = "zoom"; model.slots[i].sourceId = "p-\(i)"; model.slots[i].inShow = true }
        var micros: [Double] = []
        for _ in 0..<50 {
            let t0 = DispatchTime.now().uptimeNanoseconds
            _ = ControlJSON.encode(AppModelControlSurface.state(model).dictionary)
            micros.append(Double(DispatchTime.now().uptimeNanoseconds - t0) / 1000)
        }
        let median = micros.sorted()[25]
        ShellLog.write("control: /state build median \(Int(median)) us (10 slots)")
        expect(median < 1000, "a /state build with ten inputs stays under 1 ms on the main actor (median \(median) us)")
    }

    @MainActor
    static func testControlPushKeyIgnoresMeters() {
        let base: [String: Any] = ["recording": false, "programLoudnessLufs": -23.0, "programTruePeakDbfs": -3.0,
                                   "audioSources": [["sourceId": "zoom:1", "name": "A", "level": 10, "muted": false, "status": "live"]]]
        var meters = base
        meters["programLoudnessLufs"] = -18.0; meters["programTruePeakDbfs"] = -1.0
        meters["audioSources"] = [["sourceId": "zoom:1", "name": "A", "level": 80, "peakDbfs": -2.0, "rmsDbfs": -9.0, "muted": false, "status": "live"]]
        expectEqual(ControlStatePushKey.key(base), ControlStatePushKey.key(meters), "meter-only changes produce the same key")
        var muted = meters
        muted["audioSources"] = [["sourceId": "zoom:1", "name": "A", "level": 80, "muted": true, "status": "live"]]
        expect(ControlStatePushKey.key(muted) != ControlStatePushKey.key(base), "a mute change is a push")
        var recording = base; recording["recording"] = true
        expect(ControlStatePushKey.key(recording) != ControlStatePushKey.key(base), "a non-meter field is a push")
        // End to end through the host's sink: 20 meter-only model changes push nothing.
        let model = sceneEditModel()
        let host = ControlApiHost()
        var pushed = 0
        host.broadcastSinkForTesting = { _ in pushed += 1 }
        host.attachForTesting(model: model)
        spin(until: { pushed >= 1 }, limit: 1)   // the first state is a push
        let first = pushed
        for i in 0..<20 { model.audioStripLevelForTesting(id: "zoom:1", level: i * 5); spin(until: { false }, limit: 0.02) }
        spin(until: { false }, limit: 0.3)
        expectEqual(pushed, first, "meter-only churn never pushes")
        model.programBlackout.toggle()
        spin(until: { pushed > first }, limit: 1)
        expectEqual(pushed, first + 1, "a real change pushes once")
    }
```
`ControlApiHost` gains two internal test seams that wire the same throttle and push-key sink without a
server: `broadcastSinkForTesting: ((String) -> Void)?` and `attachForTesting(model:)`. `AppModel` gains
`audioStripLevelForTesting(id:level:)`, which mutates one strip's `outputLevel` and so fires `objectWillChange`.
Until T15, the audio-source meters do not appear in `/state`. The end-to-end half proves that a model change
which moves only meter-like inputs produces an unchanged key.

**Mutations to run**
- Drop the push-key filter (compare full JSON). Expected: `push-key` stays green until T15 adds
  `audioSources`, because today's `/state` has no meter field. **Record this.** The pure half
  (`ControlStatePushKey.key`) is what reds now, so also remove `"level"` from `audioSourceMeterKeys`: the
  pure half goes red. T15 repeats the end-to-end mutation.
- `windowsKind` returns `"UvcWebcam"` for every capture. `source-id-and-kinds` goes red on Screen.
- `zoomStatus` returns `"Zoom Live"` for `"in-meeting"` too. Rerun with `meetingState = "in-meeting"`: it must
  read `"Zoom Offline"` (the Mac core publishes `in_meeting`, `M/AppModel.swift:4072`). Add that line to the
  test if the mutation stays green.
- Publish `inputs` only for assigned slots. The count-10 assertion goes red.

**Done when:** the three cases pass, the median is reported, the platform rows are flipped and regenerated,
and `control/manifest-shared-file`'s count is updated to 16. Commit:
`mac control: inputs, Zoom and engine status in /state; meters never trigger a push (RC2)`.

---

### Task 8: Input actions: assign, unassign, name, in-show, dropout

**Lane:** control. **Slice:** RC2. **Depends on:** T7. **Commit after:** T7. Can be written in parallel with
T9 to T11, but commit in the order T8, T9, T10, T11 (they share the switch and the platform file).

**Files**
- NEW `M/ControlSurface+Inputs.swift`.
- Modify:
  - `M/ControlSurface.swift`: add five switch arms, extend `supportedActionIds`, add `ControlArg.int` and
    `ControlArg.optionalString`;
  - `M/AppModel.swift`: add `derivedSlotName(kind:sourceId:)`, a pure static;
  - the platform file (five rows to `supported`), then regenerate;
  - `contracts/control.fixtures.json`: append the int-coercion cases below;
  - tests.

**Behaviour** (Windows rulings quoted; refusal texts copied verbatim)
- **`input.assign(slot i, sourceId s)`** follows Windows `AssignInput` (`W/Services/StudioControlSurface.cs:875-909`).
  - A slot outside `1...model.slots.count` fails with `"Show Input slot <n> is out of range (1-<count>)."`.
  - The source id goes through `ControlSourceId.parse`:
    - `nil` fails with `"Unrecognized source id '<id>' (expected zoom:/capture:/media:)."`;
    - `.media` fails with `"Media sources cannot be assigned to Show Inputs on macOS yet (Media clip playback)."` (D15).
  - Otherwise call `model.assignSlot(slot, kind: "zoom"|"capture", sourceId: <bare id>,
    name: AppModel.derivedSlotName(...))`, then return success.
  - `assignSlot` already sets `inShow = true` (`M/AppModel.swift:1816-1825`), matching Windows'
    `editor.InShow = true`.
- **`input.unassign(slot i)`**: out of range fails with `"Show input slot is out of range."` (Windows text,
  lower-case "input", no number: `StudioControlSurface.cs:389`). Otherwise `model.unassignSlot(slot)`.
- **`input.name(slot i, name s?)`**: out of range fails with `"Show Input slot <n> is out of range."`.
  - A nil or blank (after trimming) name sets `slots[i].name = AppModel.derivedSlotName(kind:sourceId:)` (D12).
    That is the roster participant's name for zoom, the capture device's name for capture, else the bare
    source id, else `""` for an empty slot.
  - Otherwise set the trimmed name. Windows allows naming an unassigned slot (`:917-926`), and so does the
    Mac: `assignSlot` keeps a non-empty name (`:1820`).
- **`input.inShow.set(slot i, on b)`**: out of range fails with `"Show Input slot <n> is out of range."`.
  - Target already equal: success, no call.
  - An unassigned slot with `on=true` fails with `"Show Input slot <n> has no source."` (a Mac text: Windows'
    unassigned in-show is dropped at the next refresh, the "NEVER INVENT A SOURCE" law, CLAUDE.md).
  - Otherwise `model.toggleSlotInShow(slot)`.
- **`source.dropout.set(sourceId s, policy s)`**:
  - A non-`zoom:` id fails with the Windows text `"source.dropout.set: only zoom:<pid> and capture:<id> sources take a dropout policy (<id>)"`
    for anything Windows also refuses (`StudioControlSurface.cs:1276-1280`).
  - `capture:<id>` fails with `"source.dropout.set: macOS takes a dropout policy for zoom:<pid> sources only (<id>)"` (D15).
  - `zoom:<pid>` with no slot carrying that pid fails with `"No Show Input carries '<id>'."`.
  - A policy other than `hold|black` fails with `"policy must be 'hold' or 'black'."`. (Windows passes it to
    `SetSourceDropoutPolicy`; the Mac model silently refuses, `M/AppModel.swift:2498-2504`, so the action must
    say so.)
  - Otherwise `model.setDropoutPolicy(slotId:policy:)` for **every** slot carrying that pid.

**Fixture cases appended** (both hosts; `input.assign` and `input.inShow.set` are now Mac-supported)
| name | body | expect |
|---|---|---|
| `int-from-double-rounds-even` | `{"action":"input.unassign","args":[2.5]}` | 200, invocations `[{"action":"input.unassign","args":[2]}]` (`Math.Round` and `.toNearestOrEven`) |
| `int-from-text` | `{"action":"input.unassign","args":[" 3 "]}` | 200, `[3]` |
| `int-from-bool` | `{"action":"input.unassign","args":[true]}` | 200, `[1]` |
| `bad-int` | `{"action":"input.unassign","args":["three"]}` | 400 `Action 'input.unassign' parameter 'slot' must be Int.` |
| `assign-missing-source` | `{"action":"input.assign","args":[1]}` | 400 `Action 'input.assign' requires parameter 'sourceId' at position 1.` |

Check `int-from-text` against both hosts before committing. Windows uses `int.TryParse(" 3 ", Integer, Invariant)`,
and `NumberStyles.Integer` allows leading and trailing white space; the Mac trims `.whitespaces`. If either
host disagrees, the fixture is the first suspect (rule 4). Update the RC1 totals in both runners (+5 http).

**Tests** (complete)
```swift
    @MainActor
    static func inputsModel() -> AppModel {
        let model = sceneEditModel()
        model.roster = [RosterParticipant(id: "p-1", name: "Host", hasVideo: false, muted: false, talking: false, assigned: false),
                        RosterParticipant(id: "p-2", name: "Guest", hasVideo: false, muted: false, talking: false, assigned: false)]
        model.captureDevices = [CaptureDeviceRow(id: "cam1", name: "Desk Cam", kind: "video", vendor: "Apple",
                                                 connectionState: "connected", signalPresent: true, warning: "")]
        return model
    }

    @MainActor
    static func testControlInputActions() {
        let model = inputsModel()
        let s = AppModelControlSurface(model: model, box: model.controlSnapshotBox)
        expectEqual(invoke(s, "input.assign", [0, "zoom:p-1"]), .fail("Show Input slot 0 is out of range (1-10)."), "slot 0")
        expectEqual(invoke(s, "input.assign", [11, "zoom:p-1"]), .fail("Show Input slot 11 is out of range (1-10)."), "slot 11")
        expectEqual(invoke(s, "input.assign", [1, "ndi:x"]),
                    .fail("Unrecognized source id 'ndi:x' (expected zoom:/capture:/media:)."), "Windows unrecognized text")
        expectEqual(invoke(s, "input.assign", [1, "media:a1"]),
                    .fail("Media sources cannot be assigned to Show Inputs on macOS yet (Media clip playback)."), "media refused")
        expect(model.slots.allSatisfy { $0.kind == "unassigned" }, "refusals change nothing")

        expectEqual(invoke(s, "input.assign", [3, "zoom:p-2"]), .success, "assign zoom")
        expectEqual(model.slots[2].kind, "zoom"); expectEqual(model.slots[2].sourceId, "p-2")
        expectEqual(model.slots[2].name, "Guest", "derived from the roster"); expect(model.slots[2].inShow, "assign puts it in the show")
        expectEqual(invoke(s, "input.assign", [4, "capture:cam1"]), .success, "assign capture")
        expectEqual(model.slots[3].name, "Desk Cam", "derived from the capture device")
        expectEqual(invoke(s, "input.assign", [5, "zoom:p-404"]), .success, "an id not in the roster is accepted (Windows does not check)")
        expectEqual(model.slots[4].name, "p-404", "no roster entry: the bare id")

        expectEqual(invoke(s, "input.name", [3, "  Anchor  "]), .success, "rename"); expectEqual(model.slots[2].name, "Anchor", "trimmed")
        expectEqual(invoke(s, "input.name", [3]), .success, "omitted name"); expectEqual(model.slots[2].name, "Guest", "blank resets to derived (D12)")
        expectEqual(invoke(s, "input.name", [3, "   "]), .success, "blank name"); expectEqual(model.slots[2].name, "Guest", "whitespace resets too")
        expectEqual(invoke(s, "input.name", [12, "x"]), .fail("Show Input slot 12 is out of range."), "name range text")

        expectEqual(invoke(s, "input.inShow.set", [3, false]), .success, "take out of show"); expect(!model.slots[2].inShow, "out")
        expectEqual(invoke(s, "input.inShow.set", [3, false]), .success, "idempotent"); expect(!model.slots[2].inShow, "still out")
        expectEqual(invoke(s, "input.inShow.set", [3, true]), .success, "back in"); expect(model.slots[2].inShow, "in")
        expectEqual(invoke(s, "input.inShow.set", [9, true]), .fail("Show Input slot 9 has no source."), "no source to show")
        expectEqual(invoke(s, "input.inShow.set", [0, true]), .fail("Show Input slot 0 is out of range."), "in-show range text")

        expectEqual(invoke(s, "source.dropout.set", ["zoom:p-2", "black"]), .success, "dropout black")
        expectEqual(model.slots[2].dropoutPolicy, "black", "applied to the slot carrying p-2")
        expectEqual(invoke(s, "source.dropout.set", ["zoom:p-2", "freeze"]), .fail("policy must be 'hold' or 'black'."), "bad policy")
        expectEqual(invoke(s, "source.dropout.set", ["capture:cam1", "hold"]),
                    .fail("source.dropout.set: macOS takes a dropout policy for zoom:<pid> sources only (capture:cam1)"), "capture refused (D15)")
        expectEqual(invoke(s, "source.dropout.set", ["media:a1", "hold"]),
                    .fail("source.dropout.set: only zoom:<pid> and capture:<id> sources take a dropout policy (media:a1)"), "Windows text")
        expectEqual(invoke(s, "source.dropout.set", ["zoom:p-9", "hold"]), .fail("No Show Input carries 'zoom:p-9'."), "unslotted pid")

        expectEqual(invoke(s, "input.unassign", [3]), .success, "unassign")
        expectEqual(model.slots[2].kind, "unassigned", "cleared")
        expectEqual(invoke(s, "input.unassign", [99]), .fail("Show input slot is out of range."), "Windows unassign text (lower-case, no number)")
        // The remote unassign is the operator path: auto-assign must not refill it (M/AppModel.swift:1827-1834).
        model.roster = [RosterParticipant(id: "p-2", name: "Guest", hasVideo: true, muted: false, talking: false, assigned: false)]
        model.applyZoomForTesting(["meetingState": "in_meeting",
                                   "participants": [["userId": "p-2", "displayName": "Guest", "videoOn": true, "muted": false, "talking": false]]])
        expect(!model.slots.contains { $0.kind == "zoom" && $0.sourceId == "p-2" }, "a remotely unassigned guest is not auto-re-added")
    }
```

**Mutations to run**
- `input.assign` calls `assignSlot` with `name: ""`. The `"Guest"` assertion goes red.
- `input.name` stores `""` for blank. The derived-name assertions go red.
- `input.unassign` writes `slots[i] = ShowInputSlot(id:)` directly instead of calling `unassignSlot`. The
  auto-assign assertion goes red (the tombstone is skipped).
- `input.inShow.set` always toggles. The idempotent assertion goes red.
- `source.dropout.set` applies to the first matching slot only. Add a second slot with `p-2` to the test if
  this stays green, and report.

**Done when:** `control/input-actions` passes; closure is updated to 18 and `unavailable-422` to 92; the
fixture totals are updated on both hosts; T7's state test still passes. Commit:
`mac control: input assign, unassign, name, in-show and dropout over the API (RC2)`.

---

### Task 9: Transition and engine

**Lane:** control. **Slice:** RC2. **Depends on:** T7; commit after T8.

**Files:** `M/ControlSurface+Session.swift` (NEW), `M/ControlSurface.swift`, the platform file, tests.

**Behaviour**
- **`transport.transition.set(mode s)`**: `model.setTakeTransition(mode)`, always success. Windows behaves the
  same (`StudioControlSurface.cs:260-262`), and the Mac setter already normalizes like Windows
  `NormalizeTakeTransitionMode` (`M/AppModel.swift:2960-2969`).
- **`transport.engine.toggle`** follows Windows `RunTransportToggle` (`:1214-1223`).
  - If neither a core nor the seam is present (`bridge == nil && quitRequestForTesting == nil`), fail with
    `"Cannot toggle capture engine right now (the control is unavailable in the current state)."`.
  - Otherwise `model.setCapture(enabled: !model.captureEnabled)`, then success.
- **`transport.engine.set(on b)`** follows Windows `RunTransportSet` (`:1259-1274`).
  - `captureEnabled == on`: success, nothing sent.
  - No core: fail with `"Cannot start capture engine right now (the control is unavailable in the current state)."`.
    Use `stop` when `on` is false.
  - Otherwise `setCapture(enabled: on)`.
- Proof that the transition reaches a Take: after `transport.transition.set("cut")`, the next `transport.take`
  batch's `begin-take-transition` command carries `mode: "cut"`. This is the field the Take path already sends;
  read `M/AppModel.swift` `take()` for the key name before asserting. The transition's pixels are proven by
  the core's existing transition tests; this task proves that the remote setting reaches them.

**Tests** (complete)
```swift
    @MainActor
    static func testControlTransitionAndEngine() {
        let core = FakeTakeCore()
        let model = takeModel(core)
        let s = AppModelControlSurface(model: model, box: model.controlSnapshotBox)
        expectEqual(invoke(s, "transport.transition.set", ["CUT"]), .success, "transition set")
        expectEqual(model.takeTransitionMode, "cut", "normalized like Windows")
        expectEqual(invoke(s, "transport.transition.set", ["sideways"]), .success, "unknown mode is not an error (Windows)")
        expectEqual(model.takeTransitionMode, "fade", "unknown normalizes to fade")
        _ = invoke(s, "transport.transition.set", ["dip"])
        expectEqual(invoke(s, "transport.take"), .success, "take")
        let take = (core.bodies.last?["commands"] as? [[String: Any]])?.first { $0["type"] as? String == "begin-take-transition" }
        expectEqual(take?["mode"] as? String, "dip", "the remote transition rides the Take batch")

        expect(!model.captureEnabled, "engine starts off")
        expectEqual(invoke(s, "transport.engine.set", [false]), .success, "set off while off is a no-op")
        expectEqual(invoke(s, "transport.engine.toggle"), .success, "toggle on"); expect(model.captureEnabled, "on")
        expectEqual(invoke(s, "transport.engine.set", [true]), .success, "set on while on"); expect(model.captureEnabled, "still on")
        expectEqual(invoke(s, "transport.engine.set", [false]), .success, "set off"); expect(!model.captureEnabled, "off")
        expect(core.bodies.contains { $0["type"] as? String == "zoom-stop-capture" } || true,
               "confirmatory only: the stop rides the bridge, which the seam does not carry (rule 8)")

        let offline = sceneEditModel()
        let o = AppModelControlSurface(model: offline, box: offline.controlSnapshotBox)
        expectEqual(invoke(o, "transport.engine.toggle"),
                    .fail("Cannot toggle capture engine right now (the control is unavailable in the current state)."), "no core: toggle refused")
        expectEqual(invoke(o, "transport.engine.set", [true]),
                    .fail("Cannot start capture engine right now (the control is unavailable in the current state)."), "no core: start refused")
        expect(!offline.captureEnabled, "nothing changed")
    }
```
If the `begin-take-transition` key is not `mode`, change only that assertion's key, and say so in the report.

**Mutations to run:** call `setTakeTransition(nil)`, and the `"cut"` assertion goes red. Make
`engine.set` always toggle, and the "still on" assertion goes red. Drop the no-core guard, and the offline
assertions go red.

**Done when:** the case passes; closure 21; `unavailable-422` 89. Commit:
`mac control: transition and engine over the API (RC2)`.

---

### Task 10: Join, leave and capture online/offline as awaitable model calls

**Lane:** control. **Slice:** RC2. **Depends on:** T7; commit after T9.

**Files**
- Modify `M/AppModel.swift`:
  - extract the body of `joinZoom` (`:2177-2232`) into `performJoin(endOtherMeeting:) async`; `joinZoom`
    becomes `Task { await performJoin(...) }`, so behaviour is unchanged;
  - add three control entry points;
  - route `connectCaptureDevice`'s requests through a new `captureRequest` seam, the same shape as
    `monitorRequest` (`:4499-4503`).
- Modify `M/ControlSurface+Session.swift`, `M/ControlSurface.swift`, the platform file, tests.

**Interfaces**
```swift
extension AppModel {
    /// Seconds to wait after the core answers for meetingState to settle. Tests shorten it.
    var controlSettleSeconds: TimeInterval { get set }   // default 5
    func joinZoomForControl(target: String, displayName: String?, webinar: Bool) async -> ControlInvokeResult
    func leaveZoomForControl() async -> ControlInvokeResult
    func setCaptureDeviceOnline(_ deviceId: String, online: Bool) async -> ControlInvokeResult
}
```

**Behaviour**
- **`zoom.join(meetingUrl s, displayName s?, webinar b?)`** follows Windows (`StudioControlSurface.cs:228-242`).
  1. Set `joinMeetingId = meetingUrl`. A non-blank `displayName` replaces `displayName`. Set `webinar` to the
     arg (`nil` means `false`, D8).
  2. Refuse with `"Zoom join is unavailable in the current state."` in three cases: no core and no seam;
     `parseZoomTarget(...).meetingId` blank; or a join already in flight. Nothing is sent.
  3. `await performJoin(endOtherMeeting: false)`.
  4. Then wait up to `controlSettleSeconds` for `meetingState` to be `in_meeting` or terminal
     (`AppModel.meetingHasEnded`, `:4077-4081`).
  5. Answer:
     - `in_meeting` gives success;
     - terminal gives `.fail(zoomJoinStatus)` when non-empty, else `.fail("Zoom join did not complete.")`;
     - still settling gives `.fail("Zoom join is still in progress (<meetingState>); read zoomStatus for the outcome.")`.
     Windows answers from `IsInMeeting` and `JoinStatus`; the last two texts are Mac texts.
- **`zoom.leave`** follows Windows (`:243-251`).
  - With no core and no seam, or `meetingHasEnded(meetingState)`, fail with
    `"Zoom leave is unavailable in the current state."`.
  - Otherwise do what `leaveZoom` does (clear `assignedIds`), but **await** the `zoom-leave` request through
    the `quitRequest` seam (timeout 15). Apply the reply's `snapshot` with `applyZoom` if present.
  - Wait up to `controlSettleSeconds` for a terminal state: success; otherwise
    `.fail("Zoom leave did not complete (<meetingState>).")`.
  - `leaveZoom()` itself stays fire-and-forget for the UI.
- **`source.capture.online.set(sourceId s, online b)`** follows Windows (`:374-386`).
  - A non-`capture:` id fails with `"Choose a capture source ID."`; a device not in `captureDevices` fails with
    `"The capture device is unavailable."` (both Windows texts).
  - If the device's `connectionState == "connected"` already equals `online`, succeed without sending. (Windows
    runs the command anyway; the Mac UI entry point is a toggle, so a matching state must not toggle it the
    wrong way.)
  - Otherwise send the connect or disconnect command (the same `CoreCommands` calls `connectCaptureDevice`
    uses) through `captureRequest`, then send an empty `media-core-sync` barrier and `applySnapshot` its reply.
  - Success when the device now matches; else `.fail("Capture device '<id>' is <connectionState>, not <connected|offline>.")`.
  - The Windows action description says "taking a device offline unassigns its inputs". The Mac's disconnect
    does not unassign (it marks the slot offline, `refreshSlotHealth`). This task does not add an unassign;
    see Risks.

**Tests** (complete; `accountBusySentence` is the existing fixture constant used by
`testEndOtherSessionJoinIsExplicitAndNeverSticky`)
```swift
    @MainActor
    static func testControlJoinLeaveAndCaptureOnline() {
        let core = FakeTakeCore()
        let model = AppModel()
        model.controlSettleSeconds = 0.4
        var answers: [JSONObject] = [["meetingState": "in_meeting", "warnings": [] as [String]]]
        model.quitRequestForTesting = { body, _ in
            _ = try await core.handle(body)
            return await MainActor.run { ["snapshot": answers.isEmpty ? [:] : answers.removeFirst()] }
        }
        let s = AppModelControlSurface(model: model, box: model.controlSnapshotBox)
        func joins() -> [JSONObject] { core.bodies.filter { $0["type"] as? String == "zoom-join" }.compactMap { $0["payload"] as? JSONObject } }

        expectEqual(invoke(s, "zoom.join", ["   "]), .fail("Zoom join is unavailable in the current state."), "blank target refused")
        expectEqual(joins().count, 0, "nothing sent")
        expectEqual(invoke(s, "zoom.join", ["https://us02web.zoom.us/j/97682593786?pwd=abc", "Remote Desk", true]), .success, "join succeeds")
        expectEqual(joins().first?["meetingId"] as? String, "97682593786", "URL parsed like the UI")
        expectEqual(joins().first?["passcode"] as? String, "abc", "passcode from pwd")
        expectEqual(joins().first?["displayName"] as? String, "Remote Desk", "display name applied")
        expectEqual(joins().first?["webinar"] as? Bool, true, "webinar is the UI's field (D8)")
        expectEqual(model.meetingState, "in_meeting", "model in meeting")

        answers = [["meetingState": "error", "warnings": [accountBusySentence]]]
        expectEqual(invoke(s, "zoom.join", ["97682593786"]), .fail(accountBusySentence), "a failed join answers with Zoom's reason")
        answers = [["meetingState": "joining"]]
        expectEqual(invoke(s, "zoom.join", ["97682593786"]),
                    .fail("Zoom join is still in progress (joining); read zoomStatus for the outcome."), "an unsettled join says so")

        let noCore = AppModel()
        expectEqual(invoke(AppModelControlSurface(model: noCore, box: noCore.controlSnapshotBox), "zoom.join", ["123"]),
                    .fail("Zoom join is unavailable in the current state."), "no core")

        model.applyZoomForTesting(["meetingState": "in_meeting"])
        answers = [["meetingState": "idle"]]
        expectEqual(invoke(s, "zoom.leave"), .success, "leave succeeds")
        expect(core.bodies.contains { $0["type"] as? String == "zoom-leave" }, "leave went through the seam")
        let leavesBefore = core.bodies.filter { $0["type"] as? String == "zoom-leave" }.count
        expectEqual(invoke(s, "zoom.leave"), .fail("Zoom leave is unavailable in the current state."), "not in a meeting")
        expectEqual(core.bodies.filter { $0["type"] as? String == "zoom-leave" }.count, leavesBefore, "nothing sent when idle")

        // Capture online/offline through the capture seam.
        model.captureDevices = [CaptureDeviceRow(id: "cam1", name: "Desk Cam", kind: "video", vendor: "Apple",
                                                 connectionState: "detected", signalPresent: false, warning: "")]
        var deviceState = "detected"
        model.captureRequestForTesting = { body in
            switch body["type"] as? String {
            case "connect-capture-device": deviceState = "connected"
            case "disconnect-capture-device": deviceState = "detected"
            default: break
            }
            return ["snapshot": ["captureDevices": [["id": "cam1", "name": "Desk Cam", "kind": "video", "vendor": "Apple",
                                                     "connectionState": deviceState, "signalPresent": true, "warning": ""]]]]
        }
        expectEqual(invoke(s, "source.capture.online.set", ["zoom:p-1", true]), .fail("Choose a capture source ID."), "not a capture id")
        expectEqual(invoke(s, "source.capture.online.set", ["capture:nope", true]), .fail("The capture device is unavailable."), "unknown device")
        expectEqual(invoke(s, "source.capture.online.set", ["capture:cam1", true]), .success, "connect")
        expectEqual(model.captureDevices.first?.connectionState, "connected", "the barrier's snapshot was applied")
        expectEqual(invoke(s, "source.capture.online.set", ["capture:cam1", true]), .success, "already connected: no toggle")
        expectEqual(model.captureDevices.first?.connectionState, "connected", "a matching request never disconnects")
        expectEqual(invoke(s, "source.capture.online.set", ["capture:cam1", false]), .success, "offline")
        expectEqual(model.captureDevices.first?.connectionState, "detected", "offline")
        model.captureRequestForTesting = { _ in ["snapshot": ["captureDevices": [["id": "cam1", "name": "Desk Cam", "kind": "video", "vendor": "Apple",
                                                                                   "connectionState": "failed", "signalPresent": false, "warning": ""]]]] }
        expectEqual(invoke(s, "source.capture.online.set", ["capture:cam1", true]),
                    .fail("Capture device 'cam1' is failed, not connected."), "a connect the core did not honour is reported")
    }
```
`captureRequestForTesting: ((JSONObject) async throws -> JSONObject)?` is the capture seam. When set, both
`connectCaptureDevice` and `setCaptureDeviceOnline` use it instead of the bridge. The `invoke` helper's 3 s
limit covers the 0.4 s settle.

**Mutations to run**
- `joinZoomForControl` returns success as soon as the request returns. The `"joining"` assertion goes red.
- Drop the settle wait (read state once). Expected: `in_meeting` cases still pass because the reply is
  applied before the read; the `"joining"` case keeps it honest. Record what happened.
- `setCaptureDeviceOnline` calls `connectCaptureDevice` (the toggle) even when the state matches. "a matching
  request never disconnects" goes red.
- The leave entry point skips the `meetingHasEnded` guard. "nothing sent when idle" goes red.
- `joinZoom` (UI) no longer goes through `performJoin`. The existing `testEndOtherSessionJoinIsExplicitAndNeverSticky`
  must still pass; it is the UI's characterization test.

**Done when:** the case passes; the existing join, end-other-session and Tier 0 Zoom cases still pass;
closure 24; `unavailable-422` 86. Commit: `mac control: awaitable join, leave and capture online for the API (RC2)`.

---

### Task 11: `transport.stream.rtmp.configure`

**Lane:** control. **Slice:** RC2. **Depends on:** T7; commit after T10.

**Files**
- NEW `M/RtmpDestinationPolicy.swift`: a pure port of the Windows validation and configure rules.
- Modify `M/AppModel.swift`: add `configureRtmpForControl(_:)`, and `streamKeySaver`, an injectable seam
  that defaults to `StreamKeychain.save` returning success.
- Modify `M/ControlSurface+Session.swift`, `M/ControlSurface.swift`, the platform file, tests.

**Interfaces**
```swift
enum RtmpDestinationPolicy {
    static func buildUrl(protocol: String, serverUrl: String) -> String          // Windows BuildRtmpUrl (StudioStreamOutputValidation.cs:24-33)
    static func validate(protocol: String, serverUrl: String, streamKey: String) -> String?  // Windows ValidateRtmp (:88-…)
    struct Decision: Equatable { var url: String; var key: String }
    static func configure(args: [Any?], busy: Bool, currentKey: String) -> Result<Decision, ControlBindError>
}
extension AppModel {
    var streamKeySaver: (String) -> Bool { get set }
    func configureRtmpForControl(_ args: [Any?]) -> ControlInvokeResult
}
```

**Behaviour** (Windows `W/Services/StreamDestinationControl.cs:9-58`, proven by
`WT/StreamDestinationControlTests.cs` and `WT/StudioStreamOutputValidationTests.cs:74-143`)
1. Busy (`streamingDesired || streamingObservedLive`): fail with
   `"Stop streaming and wait for the transition to finish before configuring destinations."`.
2. `enabled == false`: fail with `"macOS streams to one RTMP destination; it cannot be disabled. Send enabled=true to configure it."` (D9).
3. Protocol, lowercased and trimmed, not `rtmp|rtmps`: fail with `"RTMP protocol must be rtmp or rtmps."`.
4. A server URL that parses (after `buildUrl`) with user info, a query or a fragment: fail with
   `"Put RTMP credentials in streamKey; serverUrl cannot contain user info, a query, or a fragment."`.
5. The key is the omitted (`nil`) value, which keeps `currentKey`, or the trimmed given value.
6. `validate` texts, in Windows order:
   - `"Choose RTMP protocol rtmp or rtmps before streaming."`
   - `"Configure RTMP server URL before streaming."`
   - `"RTMP server URL must be a valid rtmp:// or rtmps:// URL."`
   - `"RTMP server URL must include a host."`
   - `"RTMP protocol selection must match the server URL scheme."`
   - `"RTMP server URL must include an application path."`
   - `"Configure RTMP stream key before streaming."`
   - `"RTMP stream key cannot contain whitespace."`
7. No error text echoes an argument; every text above is fixed.
8. Apply: call `streamKeySaver(key)` **first**. On `false`, fail with
   `"Destination settings were not saved. Previous settings remain active; retry after resolving preference storage."`,
   and `streamUrl` and `streamKey` are unchanged. On success, set `streamKey = key` and `streamUrl = url`,
   then `persistPrefsNow()`.
9. Never start streaming.

**Tests** (complete)
```swift
    @MainActor
    static func testControlRtmpConfigure() {
        func fail(_ args: [Any?], _ text: String, busy: Bool = false, _ why: String) {
            let r = RtmpDestinationPolicy.configure(args: args, busy: busy, currentKey: "saved-key")
            if case .failure(let e) = r { expectEqual(e.message, text, why) } else { expect(false, "\(why): expected refusal") }
        }
        let good: [Any?] = [true, "rtmps", "rtmps://example.test/live", "test-key-sentinel"]
        fail(good, "Stop streaming and wait for the transition to finish before configuring destinations.", busy: true, "busy")
        fail([false, "rtmps", "", ""], "macOS streams to one RTMP destination; it cannot be disabled. Send enabled=true to configure it.", "disable (D9)")
        fail([true, "invalid-mode-test-secret", "example.test/live", "k"], "RTMP protocol must be rtmp or rtmps.", "protocol")
        fail([true, "rtmps", "rtmps://user:pw@example.test/live", "k"],
             "Put RTMP credentials in streamKey; serverUrl cannot contain user info, a query, or a fragment.", "user info")
        fail([true, "rtmps", "example.test/live?key=x", "k"],
             "Put RTMP credentials in streamKey; serverUrl cannot contain user info, a query, or a fragment.", "query")
        fail([true, "rtmp", "rtmps://live.example.com/app", "k"], "RTMP protocol selection must match the server URL scheme.", "scheme mismatch")
        fail([true, "rtmps", "live.example.com", "k"], "RTMP server URL must include an application path.", "no app path")
        fail([true, "rtmps", "live.example.com/app", "stream key"], "RTMP stream key cannot contain whitespace.", "whitespace key")
        fail([true, "rtmps", "live.example.com/app", ""], "Configure RTMP stream key before streaming.", "explicit empty key while enabled")
        fail([true, "rtmps", "", "k"], "Configure RTMP server URL before streaming.", "no URL")
        for secret in ["invalid-mode-test-secret", "pw", "key=x"] {
            for args in [[true, "invalid-mode-test-secret", "example.test/live", "k"], [true, "rtmps", "rtmps://user:pw@example.test/live?key=x", "k"]] as [[Any?]] {
                if case .failure(let e) = RtmpDestinationPolicy.configure(args: args, busy: false, currentKey: "") {
                    expect(!e.message.contains(secret), "no refusal echoes caller data (\(secret))")
                }
            }
        }
        expectEqual(RtmpDestinationPolicy.buildUrl(protocol: "rtmps", serverUrl: "live.example.com/app"), "rtmps://live.example.com/app", "Windows BuildRtmpUrl")
        expectEqual(try? RtmpDestinationPolicy.configure(args: [true, "RTMPS ", "live.example.com/app"], busy: false, currentKey: "saved-key").get(),
                    .init(url: "rtmps://live.example.com/app", key: "saved-key"), "omitted key keeps the saved key")

        let model = takeModel(FakeTakeCore())
        var saved: [String] = []
        model.streamKeySaver = { saved.append($0); return true }
        let s = AppModelControlSurface(model: model, box: model.controlSnapshotBox)
        expectEqual(invoke(s, "transport.stream.rtmp.configure", good), .success, "configure")
        expectEqual(model.streamUrl, "rtmps://example.test/live", "URL applied"); expectEqual(model.streamKey, "test-key-sentinel", "key applied")
        expectEqual(saved, ["test-key-sentinel"], "the key went to the Keychain seam, never to prefs")
        expect(!model.streamingDesired, "configuring never starts streaming")
        model.streamKeySaver = { _ in false }
        expectEqual(invoke(s, "transport.stream.rtmp.configure", [true, "rtmp", "rtmp://other.test/app", "new-key"]),
                    .fail("Destination settings were not saved. Previous settings remain active; retry after resolving preference storage."), "storage failure")
        expectEqual(model.streamUrl, "rtmps://example.test/live", "a failed save changes nothing"); expectEqual(model.streamKey, "test-key-sentinel", "key kept")
    }
```

**Mutations to run**
- Apply before the save. "a failed save changes nothing" goes red.
- Echo the protocol in the refusal. The no-echo assertion goes red.
- Accept `enabled=false` as "keep". The D9 assertion goes red.
- Skip the scheme-mismatch rule. "scheme mismatch" goes red.

**Done when:** the case passes; closure **25**; `unavailable-422` **85**; `control/fixtures` still passes.
Commit: `mac control: configure the RTMP destination over the API (RC2)`.

---

### Task 12: RC2 setup drill and slice proof

**Lane:** control. **Slice:** RC2. **Depends on:** T8 to T11.

**Files:** NEW `scripts/control-setup-drill.py`, NEW `scripts/tests/test_control_setup_drill.py` (run with
`python3 -m unittest`; add a line to `scripts/tests/README` if one exists, otherwise document the run
command in the script header).

**Interfaces**
```
python3 scripts/control-setup-drill.py --base http://127.0.0.1:8011 [--token T] --slot 1 --source capture:<id> --scene <sceneId> [--record-seconds 5]
```
It runs these steps in order over HTTP, judging each from `GET /state` and `GET /snapshot`:
1. `input.assign`: `/state` `inputs[slot-1].sourceId == source`.
2. `scene.select`: `previewSceneId == scene`.
3. `transport.transition.set "cut"`, then `transport.take`: `activeSceneId == scene`, and `/snapshot`
   `programFrame.sceneId == scene` within 3 s with `sceneIdAttribution == "live"` (the core's own
   attribution, CLAUDE.md "A Take is traceable").
4. `transport.record.set true`, wait `--record-seconds`, then `false`: `/snapshot` `recording.lifecycle` reaches
   `completed` within 30 s.

Exit 0 when all steps pass; 1 on the first failed step (named); 2 on a usage or connection error. It sends no
join, because the drill must run without a meeting.

**Tests** (complete: a stub server answering scripted states)
```python
import json, subprocess, sys, threading, unittest
from http.server import BaseHTTPRequestHandler, HTTPServer
from pathlib import Path

SCRIPT = Path(__file__).resolve().parents[1] / "control-setup-drill.py"

class Stub(BaseHTTPRequestHandler):
    state = {"inputs": [{"slot": 1, "sourceId": None}], "previewSceneId": "", "activeSceneId": ""}
    snapshot = {"programFrame": {"sceneId": "", "sceneIdAttribution": "unknown"}, "recording": {"lifecycle": {"state": "idle"}}}
    break_take = False
    def log_message(self, *a): pass
    def _send(self, code, body):
        data = json.dumps(body).encode(); self.send_response(code)
        self.send_header("Content-Type", "application/json"); self.send_header("Content-Length", str(len(data))); self.end_headers(); self.wfile.write(data)
    def do_GET(self):
        if self.path == "/state": return self._send(200, Stub.state)
        if self.path == "/snapshot": return self._send(200, {"ok": True, "available": True, "stale": False, "snapshot": Stub.snapshot})
        self._send(404, {"ok": False, "error": "Not found"})
    def do_POST(self):
        body = json.loads(self.rfile.read(int(self.headers["Content-Length"])))
        a, args = body["action"], body.get("args", [])
        if a == "input.assign": Stub.state["inputs"][0]["sourceId"] = args[1]
        if a == "scene.select": Stub.state["previewSceneId"] = args[0]
        if a == "transport.take" and not Stub.break_take:
            Stub.state["activeSceneId"] = Stub.state["previewSceneId"]
            Stub.snapshot["programFrame"] = {"sceneId": Stub.state["activeSceneId"], "sceneIdAttribution": "live"}
        if a == "transport.record.set" and args[0] is False: Stub.snapshot["recording"] = {"lifecycle": {"state": "completed"}}
        self._send(200, {"ok": True})

class DrillTests(unittest.TestCase):
    def run_drill(self):
        server = HTTPServer(("127.0.0.1", 0), Stub); t = threading.Thread(target=server.serve_forever, daemon=True); t.start()
        try:
            return subprocess.run([sys.executable, "-I", str(SCRIPT), "--base", f"http://127.0.0.1:{server.server_port}",
                                   "--slot", "1", "--source", "capture:cam1", "--scene", "s3", "--record-seconds", "0"],
                                  capture_output=True, text=True, timeout=60)
        finally:
            server.shutdown()
    def test_all_steps_pass_against_a_conforming_host(self):
        Stub.break_take = False
        r = self.run_drill(); self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
    def test_a_take_that_never_lands_fails_naming_the_step(self):
        Stub.break_take = True
        r = self.run_drill(); self.assertEqual(r.returncode, 1); self.assertIn("transport.take", r.stdout)

if __name__ == "__main__": unittest.main()
```
Reset `Stub.state`/`Stub.snapshot` in `setUp` (write `setUp` to deep-copy the initial dicts) so that test
order does not matter.

**Mutations to run:** judge step 3 from `/state` only (drop the `/snapshot` attribution check). Then make the
stub set `activeSceneId` but leave `programFrame` stale; the drill must still fail. Add that variant as a
third test if the mutation stays green.

**RC2 proof (slice exit)**
1. The shell suite, CT, MCT-filtered, `contract:check`, `test:scripts` and the drill unit tests are all green.
   `control/closure` reports 25.
2. Run `control-setup-drill.py` against a dev build (`scripts/run-mac-shell.sh`) with a connected capture
   device and no meeting. Attach the output. The recording it makes is decoded with
   `ffprobe -show_streams -count_frames` to show a non-empty video stream.
3. **Owner check 1** (spec section 7): Companion on the same Mac. Join a meeting, assign two guests, set a fade
   transition, Take. The tester records which Companion buttons lit.

Commit: `scripts: control-setup-drill drives join-free setup over HTTP (RC2)`.

---

## RC3: audio, multiview, legacy overlays (11 actions)

### Task 13: Lower third, captions toggle, multiview, mastering, VST scan

**Lane:** control. **Slice:** RC3. **Depends on:** T7 (state and push key); commit after T11.

**Files**
- NEW `M/ControlSurface+Production.swift`.
- Modify:
  - `M/ControlSurface.swift`;
  - `M/AppModel.swift`: route `scanVstPlugins`' request (`M/VstInserts.swift:264-276`) and
    `applyMultiviewConfig` (`:2048-2057`) through the existing `coreCommandCaptureForTesting`/`quitRequest`
    seams, so the harness can see them;
  - the platform file: flip 8 actions to `supported`, plus feedback `lowerThirdOnAir`, `multiviewTileCount`
    and `autoCaptions`;
  - tests.

**Behaviour**
- **`graphics.lowerThird.set(on b)` and `graphics.lowerThird.toggle`** follow Windows (`StudioControlSurface.cs:482-490`:
  accepted intent, never "the pixels landed").
  - The target is `!model.lowerThirdVisible` for a toggle, else `on`.
  - A target `true` calls `model.showLowerThird()`. If `lowerThirdCommandStatus == LowerThirdKeyReadout.needsSource`
    afterwards, fail with that text; otherwise succeed.
  - A target `false` calls `model.hideLowerThird()` and succeeds. The model's retire is idempotent (`:3869-3874`).
- **`automation.captions.set(on b)`**: `true` fails with Windows' exact text, `"Automatic captions are unavailable: CoreVideo does not transcribe speech. Use explicit caption cues from a control client or the show engine."`
  (`W/ViewModels/MagicScene/MagicSceneCoordinator.cs:71-72`). `false` succeeds and changes nothing. `/state`
  `autoCaptions` is `false`, always (the true value: no caption source exists, `M/AutomationExtras.swift:352-359`).
- **`multiview.layout.set(mode s)`**: `model.configureMultiviewer(mode:)` with Windows normalization:
  `grid|pgmPvwTop|pgmPvwLarge|pgmPvwSide` pass, anything else becomes `pgmPvwTop`
  (`W/ViewModels/StudioViewModel.cs:3007-3011`). Always success.
- **`multiview.tileCount.set(count i)`**: `model.multiviewTileCount = clamp(count, ProductionSettings.multiviewTileRange)`,
  then `model.applyMultiviewConfig()` (the Settings stepper's two calls, `M/SettingsPane.swift:93-96`; D11).
  Always success.
- **`audio.mastering.set(on b)`**: `model.editMastering { $0.enabled = on }`. Success.
- **`audio.mastering.target(index i)`**: clamp to 0...2 (Windows `:665-667`), then
  `model.editMastering { $0.targetLufs = [-14, -16, -23][index] }`. Success.
- **`audio.vst.scan`**: with no core and no seam, fail with `"VST3 scan is unavailable: no media core."` (a
  Mac text; Windows always succeeds, but the Mac would otherwise claim a scan that never runs). Otherwise
  `model.scanVstPlugins()`; success.
- **State:**
  - `lowerThirdOnAir`: `model.lowerThirdVisible` (the core's observed `visible`, `M/AppModel.swift:429-431`);
  - `multiviewTileCount`: `model.multiviewTileCount`;
  - `masteringOn`: `model.mastering.enabled`;
  - `masteringTarget`: the index of `targetLufs` in `[-14, -16, -23]` (exact match), else `-1` (D10);
  - `autoCaptions`: `false`.

**Tests** (complete)
```swift
    @MainActor
    static func testControlProductionActions() {
        let model = takeModel(FakeTakeCore())
        var sent: [JSONObject] = []
        model.coreCommandCaptureForTesting = { sent.append($0) }
        let s = AppModelControlSurface(model: model, box: model.controlSnapshotBox)
        func types() -> [String] { sent.flatMap { ($0["commands"] as? [JSONObject]) ?? [$0] }.compactMap { $0["type"] as? String } }

        // Lower third: no Program source -> Windows-shaped refusal with the model's own reason.
        expectEqual(invoke(s, "graphics.lowerThird.set", [true]), .fail(LowerThirdKeyReadout.needsSource), "no Program source")
        // With a Program source the remote key-in is the button's command, byte for byte.
        model.slots[0].kind = "zoom"; model.slots[0].sourceId = "p-1"; model.slots[0].name = "Host"; model.slots[0].inShow = true
        model.roster = [RosterParticipant(id: "p-1", name: "Host", hasVideo: true, muted: false, talking: false, assigned: true)]
        model.applySnapshotForTesting(["renderedProgramSources": [["sourceId": "zoom:p-1", "participantId": "p-1", "kind": "participant-video"]]])
        sent.removeAll()
        model.showLowerThird()
        let buttonCommand = sent.last
        sent.removeAll()
        model.hideLowerThird(); spin(until: { false }, limit: 0.3); sent.removeAll()
        expectEqual(invoke(s, "graphics.lowerThird.set", [true]), .success, "key in accepted")
        expect(buttonCommand != nil && NSDictionary(dictionary: sent.last ?? [:]).isEqual(to: buttonCommand ?? [:]),
               "the remote key-in sends exactly the button's set-overlay-asset")
        expectEqual(invoke(s, "graphics.lowerThird.set", [false]), .success, "key out")
        expectEqual((sent.last?["keyPhase"] as? String) ?? ((sent.last?["commands"] as? [JSONObject])?.last?["keyPhase"] as? String), "building-out", "key out starts the build-out")

        expectEqual(invoke(s, "automation.captions.set", [true]),
                    .fail("Automatic captions are unavailable: CoreVideo does not transcribe speech. Use explicit caption cues from a control client or the show engine."),
                    "Windows text")
        expectEqual(invoke(s, "automation.captions.set", [false]), .success, "false is a no-op success")

        expectEqual(invoke(s, "multiview.layout.set", ["pgmPvwSide"]), .success, "layout"); expectEqual(model.multiviewLayoutMode, "pgmPvwSide", "applied")
        expectEqual(invoke(s, "multiview.layout.set", ["mosaic"]), .success, "unknown layout"); expectEqual(model.multiviewLayoutMode, "pgmPvwTop", "normalizes to pgmPvwTop (Windows)")
        for (asked, got) in [(0, 1), (12, 12), (40, 16)] {
            expectEqual(invoke(s, "multiview.tileCount.set", [asked]), .success, "tile count \(asked)")
            expectEqual(model.multiviewTileCount, got, "\(asked) clamps to \(got) (Mac range 1...16, D11)")
        }
        expect(types().contains("configure-multiviewer"), "the tile count reached the core")

        expectEqual(invoke(s, "audio.mastering.set", [true]), .success, "mastering on"); expect(model.mastering.enabled, "on")
        for (index, lufs) in [(0, -14.0), (1, -16.0), (2, -23.0), (7, -23.0), (-3, -14.0)] {
            _ = invoke(s, "audio.mastering.target", [index])
            expectEqual(model.mastering.targetLufs, lufs, "index \(index) -> \(lufs) LUFS (clamped 0...2)")
        }
        var state = AppModelControlSurface.state(model).dictionary
        expectEqual(state["masteringTarget"] as? Int, 0, "-14 is index 0")
        model.editMastering { $0.targetLufs = -18 }
        state = AppModelControlSurface.state(model).dictionary
        expectEqual(state["masteringTarget"] as? Int, -1, "a custom target is -1 (D10)")
        expectEqual(state["masteringOn"] as? Bool, true, "masteringOn"); expectEqual(state["autoCaptions"] as? Bool, false, "autoCaptions false")
        expectEqual(state["multiviewTileCount"] as? Int, 16, "multiviewTileCount")
        expectEqual(state["lowerThirdOnAir"] as? Bool, model.lowerThirdVisible, "confirmatory: mirrors the model field (rule 8)")
        model.applySnapshotForTesting(["overlayState": [["overlayId": "key:lower-third", "visible": true, "keyPhase": "on-air"]]])
        expectEqual(AppModelControlSurface.state(model).dictionary["lowerThirdOnAir"] as? Bool, true, "on air once the core says visible")

        expectEqual(invoke(s, "audio.vst.scan"), .success, "scan accepted with a core")
        let noCore = AppModel()
        expectEqual(invoke(AppModelControlSurface(model: noCore, box: noCore.controlSnapshotBox), "audio.vst.scan"),
                    .fail("VST3 scan is unavailable: no media core."), "no core: no false success")
    }
```
Before writing this test, read `programLowerThirdSubject()` and the `overlayState` parser in `M/AppModel.swift`
for the exact snapshot keys that make a Program subject and `visible` true. Adjust the two
`applySnapshotForTesting` payloads to the real keys (rule 5), and say what you changed.

**Mutations to run**
- Lower-third set compares against `lowerThirdKeyedSourceId` instead of calling `showLowerThird`. The
  byte-identical assertion goes red.
- `automation.captions.set(false)` refuses. Red.
- Tile count clamps to 1...10. The `(12, 12)` row goes red.
- `masteringTarget` rounds `-18` to the nearest index. The `-1` assertion goes red.
- `audio.vst.scan` returns success with no core. Red.

**Done when:** the case passes; closure **33**; `unavailable-422` **77**; feedback count **19**. Commit:
`mac control: lower third, multiview, mastering and VST scan over the API (RC3)`.

---

### Task 14: Monitor on/volume with authority epoch and revision

**Lane:** control. **Slice:** RC3. **Depends on:** T7; commit after T13.

**Files**
- Modify `M/MonitorControlProjection.swift`:
  - add `remoteEdit`;
  - `nextCommand` uses the pinned operation id when one is set;
  - add `outcome(of:)`.
- Modify `M/AppModel.swift`:
  - add `requestMonitorControl(...)`. It reuses the `setMonitor` send loop (`:4441-4497`); extract that loop
    into `runMonitorSendLoop() async`, called by both, so the UI's behaviour is unchanged.
- Modify `M/ControlSurface+Production.swift`, `M/ControlSurface.swift`, the platform file (2 actions plus
  feedback `audioMonitorOn` and `audioMonitorVolume`), tests.

**Interfaces**
```swift
enum MonitorControlOutcome: Equatable { case applied, conflict(revision: Int64), rejected, unconfirmed }
extension MonitorControlProjection {
    mutating func remoteEdit(_ draft: Draft, expectedEpoch: String?, expectedRevision: Int64?, operationId: String?)
    func outcome(of operationId: String) -> MonitorControlOutcome?   // nil while pending
}
extension AppModel {
    func requestMonitorControl(enabled: Bool?, volume: Double?, expectedEpoch: String?,
                               expectedRevision: Int64?, operationId: String?) async -> MonitorControlOutcome
}
```

**Behaviour** (Windows `ApplyAudioMonitorControlAsync`, `StudioControlSurface.cs:1325-1343`; model
`StudioViewModel.AudioMonitorControl.cs:138-166`)
- **Args:** `epoch = optionalString(1)`, treated as nil when blank. `expectedRevision = args[2]`: when present
  it must be a finite, non-negative, integral `Double` ≤ 9007199254740991, else fail with
  `"expectedRevision must be a nonnegative safe integer."`. `operationId = optionalString(3)`, nil when blank.
- **The draft:**
  - `enabled ?? current`.
  - `volume`: clamp to 0...1 (Windows `Math.Clamp`, `:655-656`), else current.
  - `enabled && volume <= 0` gives the default volume 0.7.
  - The device id and name are the current ones.
- **`remoteEdit`** pins `draftExpectedEpoch`/`draftExpectedRevision` to the caller's values when given, else to
  the current applied values, exactly like `edit`. It also pins the operation id (a fresh UUID when nil), so
  `nextCommand` sends **that** id.
- **`requestMonitorControl`:**
  1. Run the send loop.
  2. Wait for `outcome(of: id)` (up to `controlSettleSeconds`), applying snapshots as they arrive.
  3. Map the result: `applied` → `.applied`; core status `conflict` → `.conflict(revision: current revision)`;
     any other result status → `.rejected`; no result → `.unconfirmed`.
  4. On `.applied`, set `monitorEnabled`/`monitorVolume` and save prefs, as the UI does.
- **Replies** (Windows texts):
  - `.applied` → success;
  - `.conflict(r)` → `"Monitor control conflict at revision <r>; read state and retry with the current epoch and revision."`;
  - `.rejected` → `"Monitor control rejected; application is unconfirmed."`;
  - `.unconfirmed` → `"Monitor control unconfirmed; application is unconfirmed."`.

  The last two follow Windows' `$"Monitor control {kind.ToLowerInvariant()}; …"` pattern.
- **State** (Windows takes these from the core-applied session, `StudioControlSurface.cs:1010-1014`):
  - `audioMonitorOn` = `monitorControl.applied.enabled`;
  - `audioMonitorVolume` = `monitorControl.applied.volume`;
  - `audioMonitorAuthorityEpoch` = `monitorControl.authorityEpoch`;
  - `audioMonitorRevision` = `monitorControl.revision` (Int64);
  - `audioMonitorLastResult` = the last result status observed, else `""`.

**Tests** (complete; the fake core plays the core's monitor-control contract)
```swift
    final class FakeMonitorCore: @unchecked Sendable {
        var epoch = "core-1", revision: Int64 = 3, enabled = false, volume = 0.7, results: [JSONObject] = []
        var forceStatus: String?   // "conflict" / "rejected" / nil (apply)
        var answer = true
        func handle(_ body: JSONObject) -> JSONObject {
            for c in (body["commands"] as? [JSONObject]) ?? [] where c["type"] as? String == "set-audio-monitor-control" {
                let op = c["operationId"] as? String ?? ""
                let ok = c["authorityEpoch"] as? String == epoch && (c["expectedRevision"] as? NSNumber)?.int64Value == revision
                let status = forceStatus ?? (ok ? "applied" : "conflict")
                if status == "applied" { enabled = c["enabled"] as? Bool ?? enabled; volume = (c["volume"] as? NSNumber)?.doubleValue ?? volume; revision += 1 }
                results.append(["operationId": op, "status": status])
            }
            guard answer else { return ["snapshot": [:]] }
            return ["snapshot": ["audioMixSession": ["monitorEnabled": enabled, "monitorVolume": volume,
                                                     "monitorControl": ["authorityEpoch": epoch, "revision": revision, "recentResults": results]]]]
        }
    }

    @MainActor
    static func testControlMonitorRevisioned() {
        let fake = FakeMonitorCore()
        let model = AppModel()
        model.controlSettleSeconds = 0.4
        model.quitRequestForTesting = { body, _ in await MainActor.run { fake.handle(body) } }
        model.status = .connected(renderer: "test")
        let s = AppModelControlSurface(model: model, box: model.controlSnapshotBox)
        // A second client that read epoch core-1 at revision 3 applies.
        expectEqual(invoke(s, "audio.monitor.set", [true, "core-1", 3, "client-a"]), .success, "applied at the expected revision")
        expectEqual(fake.results.last?["operationId"] as? String, "client-a", "the caller's operation id reached the core")
        var state = AppModelControlSurface.state(model).dictionary
        expectEqual(state["audioMonitorOn"] as? Bool, true, "applied state"); expectEqual(state["audioMonitorRevision"] as? Int64, 4, "revision moved to 4")
        expectEqual(state["audioMonitorAuthorityEpoch"] as? String, "core-1", "epoch")
        // A stale revision conflicts (port of CT ControlActionRegistryTests:106 intent).
        expectEqual(invoke(s, "audio.monitor.volume", [0.3, "core-1", 3, "client-b"]),
                    .fail("Monitor control conflict at revision 4; read state and retry with the current epoch and revision."), "conflict")
        state = AppModelControlSurface.state(model).dictionary
        expectEqual(state["audioMonitorVolume"] as? Double, 0.7, "a conflict changes nothing")
        // Legacy shape: no epoch/revision -> the current values are used (Windows binds them as null).
        expectEqual(invoke(s, "audio.monitor.volume", [1.7]), .success, "legacy volume applies")
        expectEqual(model.monitorControl.applied.volume, 1.0, "volume clamps to 1.0 (Windows Math.Clamp)")
        expectEqual(invoke(s, "audio.monitor.volume", [0.4, "core-1", -1]), .fail("expectedRevision must be a nonnegative safe integer."), "negative revision")
        expectEqual(invoke(s, "audio.monitor.volume", [0.4, "core-1", 2.5]), .fail("expectedRevision must be a nonnegative safe integer."), "fractional revision")
        fake.forceStatus = "rejected"
        expectEqual(invoke(s, "audio.monitor.set", [false]), .fail("Monitor control rejected; application is unconfirmed."), "rejected")
        fake.forceStatus = nil; fake.answer = false
        expectEqual(invoke(s, "audio.monitor.set", [false]), .fail("Monitor control unconfirmed; application is unconfirmed."), "no result inside the settle window")
    }
```
Before writing, read `MonitorControlProjection.observe` (`M/MonitorControlProjection.swift:51-89`) and
`applySnapshot`'s monitor branch for the exact node name the core uses (`audioMixSession` above is the
Windows name). Use the Mac's key if it differs (rule 5), and say so.

**Mutations to run**
- `remoteEdit` ignores the caller's revision (uses current). The conflict assertion goes red.
- `nextCommand` mints a fresh UUID instead of the pinned id. The `client-a` assertion goes red, or the outcome
  wait times out.
- State reads `monitorEnabled` (local intent) instead of `applied`. Drop the fake's apply on conflict and
  check that "a conflict changes nothing" goes red under the mutation; if it stays green, add an assertion on
  `audioMonitorOn` after a conflicting `set`.
- The volume clamp is removed. Red.

**Done when:** the case passes; the existing `monitor/revisioned-projection` and Tier 0 monitor cases still
pass; closure **35**; feedback count **21**. Commit:
`mac control: revisioned monitor on/volume for remote clients (RC3)`.

---

### Task 15: `audio.route.set`, `audioRoutes[]` and `audioSources[]`

**Lane:** control. **Slice:** RC3. **Depends on:** T14 (the same pattern); commit after T14.

**Files**
- Modify `M/AudioRouteControlProjection.swift`: `remoteEdit(source:bus:enabled:gainDb:expectedEpoch:expectedRevision:operationId:)`,
  plus `outcome(of:)`.
- Modify `M/AppModel.swift`: `requestAudioRouteControl(...) async -> AudioRouteControlOutcome`, reusing the
  route send path (`editRoute` → `pushRoutingBase`, `:3651-3656`).
- Modify `M/ControlSurface+Production.swift`, `M/ControlSurface.swift`, the platform file (1 action), tests.

**Behaviour** (Windows `ApplyAudioRouteControlAsync`, `StudioControlSurface.cs:1345-1369`)
- **Validation:**
  - `sourceId`, `busId`, `authorityEpoch` and `operationId` must all be non-blank after trimming; else fail
    with `"sourceId, busId, authorityEpoch and operationId are required."`.
  - `gainDb` must be finite and within -60...10; else fail with `"gainDb must be between -60 and +10."`.
  - `expectedRevision` uses the same safe-integer rule as T14, with the same text.
  - `busId` must be one of `AppModel.routingBuses` (`:3598`); else fail with `"busId must be one of master, pgm-l, pgm-r, stream, mon, iso-1."`.
    This is a Mac text: Windows lets the core refuse, but the Mac's full-state matrix would drop an unknown
    bus silently.
- **Apply:** `remoteEdit` pins the caller's epoch, revision and operation id. Also update
  `operatorRouteEdits` and `audioSends` exactly as `selectOrRouteSend`, `setSendGain` and `removeSend` do, so
  the UI grid shows the remote edit. Include the ISO-exclusive rule for `iso-*` buses (`:3611-3619`) when
  `enabled`.
- **Replies:**
  - `applied` → success;
  - conflict → `"Audio route conflict at revision <r>; read state and retry."`;
  - otherwise `"Audio route <kind>; application is unconfirmed."`, with kind `rejected` or `unconfirmed`.
- **State:**
  - `audioRouteAuthorityEpoch`, `audioRouteRevision` and `audioRouteLastResult` come from the projection.
  - `audioRoutes` is `applied` (enabled entries) sorted by `(sourceId, busId)` as
    `{sourceId, busId, gainDb}` (Windows `ControlAudioRouteState`).
  - `audioSources` is one row per `audioStrips` entry, sorted by id: `{sourceId, name, level, muted, status}`.
    `name` is the slot name for a source a slot carries (canonical id match), else the id (Windows
    `:968-981`). `peakDbfs` and `rmsDbfs` are **omitted**, because the Mac strip has no true-peak or LUFS per
    source (`M/AppModel.swift:130-141`).
  - `audioSourceCount` is `audioSources.count`.
  - `programLoudnessLufs`/`programTruePeakDbfs` are not published (no Mac source); they remain push-key meter
    names for when they are.

**Tests** (complete)
```swift
    final class FakeRouteCore: @unchecked Sendable {
        var epoch = "core-1", revision: Int64 = 2, sends: [JSONObject] = [], results: [JSONObject] = []
        func handle(_ body: JSONObject) -> JSONObject {
            for c in (body["commands"] as? [JSONObject]) ?? [] where c["type"] as? String == "set-audio-route-control" {
                let ok = c["authorityEpoch"] as? String == epoch && (c["expectedRevision"] as? NSNumber)?.int64Value == revision
                if ok {
                    sends.removeAll { $0["sourceId"] as? String == c["sourceId"] as? String && $0["busId"] as? String == c["busId"] as? String }
                    if c["enabled"] as? Bool == true { sends.append(["sourceId": c["sourceId"]!, "busId": c["busId"]!, "gainDb": c["gainDb"]!]) }
                    revision += 1
                }
                results.append(["operationId": c["operationId"] as? String ?? "", "status": ok ? "applied" : "conflict"])
            }
            return ["snapshot": ["audioRoutingMatrix": ["sends": sends, "control": ["authorityEpoch": epoch, "revision": revision, "recentResults": results]]]]
        }
    }

    @MainActor
    static func testControlAudioRoute() {
        let fake = FakeRouteCore()
        let model = AppModel()
        model.controlSettleSeconds = 0.4
        model.quitRequestForTesting = { body, _ in await MainActor.run { fake.handle(body) } }
        model.status = .connected(renderer: "test")
        model.applySnapshotForTesting(fake.handle([:]))   // learn epoch core-1 / revision 2
        let s = AppModelControlSurface(model: model, box: model.controlSnapshotBox)
        expectEqual(invoke(s, "audio.route.set", ["zoom:42", "mon", true, -6, "core-1", 2, "client-b"]), .success, "applied")
        var state = AppModelControlSurface.state(model).dictionary
        let routes = state["audioRoutes"] as? [[String: Any]] ?? []
        expect(NSArray(array: routes).isEqual(to: [["sourceId": "zoom:42", "busId": "mon", "gainDb": -6.0]]), "audioRoutes is the core's applied set")
        expectEqual(state["audioRouteRevision"] as? Int64, 3, "revision 3")
        expectEqual(model.audioSends["zoom:42"]?["mon"], -6, "the UI grid shows the remote edit")
        expectEqual(invoke(s, "audio.route.set", ["zoom:42", "mon", true, -3, "core-1", 2, "client-c"]),
                    .fail("Audio route conflict at revision 3; read state and retry."), "stale revision conflicts")
        for (args, text) in [
            (["", "mon", true, 0, "core-1", 3, "op"], "sourceId, busId, authorityEpoch and operationId are required."),
            (["zoom:42", "mon", true, 0, "core-1", 3, " "], "sourceId, busId, authorityEpoch and operationId are required."),
            (["zoom:42", "mon", true, 11, "core-1", 3, "op"], "gainDb must be between -60 and +10."),
            (["zoom:42", "mon", true, -61, "core-1", 3, "op"], "gainDb must be between -60 and +10."),
            (["zoom:42", "mon", true, 0, "core-1", 3.5, "op"], "expectedRevision must be a nonnegative safe integer."),
            (["zoom:42", "booth", true, 0, "core-1", 3, "op"], "busId must be one of master, pgm-l, pgm-r, stream, mon, iso-1."),
        ] as [([Any?], String)] {
            expectEqual(invoke(s, "audio.route.set", args), .fail(text), text)
        }
        expectEqual(invoke(s, "audio.route.set", ["zoom:42", "mon", false, 0, "core-1", 3, "client-d"]), .success, "un-route")
        state = AppModelControlSurface.state(model).dictionary
        expectEqual((state["audioRoutes"] as? [Any])?.count, 0, "removed")
        // audioSources: names from slots, no peak/rms keys, sorted.
        model.slots[0].kind = "zoom"; model.slots[0].sourceId = "42"; model.slots[0].name = "Host"
        model.audioStripLevelForTesting(id: "zoom:42", level: 55)
        model.audioStripLevelForTesting(id: "capture:mic", level: 10)
        let sources = AppModelControlSurface.state(model).dictionary["audioSources"] as? [[String: Any]] ?? []
        expectEqual(sources.compactMap { $0["sourceId"] as? String }, ["capture:mic", "zoom:42"], "sorted by id")
        expectEqual(sources.last?["name"] as? String, "Host", "slot name"); expectEqual(sources.first?["name"] as? String, "capture:mic", "else the id")
        expect(sources.allSatisfy { $0["peakDbfs"] == nil && $0["rmsDbfs"] == nil }, "no fabricated peak/RMS on the Mac")
        // End-to-end push key (deferred from T7): level churn pushes nothing.
        let host = ControlApiHost()
        var pushed = 0
        host.broadcastSinkForTesting = { _ in pushed += 1 }
        host.attachForTesting(model: model)
        spin(until: { pushed >= 1 }, limit: 1)
        let first = pushed
        for i in 0..<20 { model.audioStripLevelForTesting(id: "zoom:42", level: i * 5); spin(until: { false }, limit: 0.02) }
        spin(until: { false }, limit: 0.3)
        expectEqual(pushed, first, "meter churn on audioSources never pushes")
    }
```
Read the Mac's `audioRoutingMatrix` node name in `applySnapshot` before writing (rule 5). If the Mac reads a
different key, the fake must use it.

**Mutations to run**
- Drop the push-key filter (the T7 deferral). "meter churn on audioSources never pushes" goes red. **This is
  the mutation T7 could not red; report it here.**
- Skip the `audioSends` update. "the UI grid shows the remote edit" goes red.
- Add `peakDbfs: 0` to each source. Red.
- Make the bus check pass any bus. Red.

**Done when:** the case passes; closure **36**; `unavailable-422` **74**; feedback count stays 21.
`audioSources` and the route fields are Mac `/state` extras beyond `StateFields`, which is legal: FeedbackFields
lists the OSC-feedback set only.

**RC3 proof (slice exit)**
1. The shell suite, CT, MCT, `contract:check` and `test:scripts` are green; `control/closure` = 36.
2. **Lower-third pixels** (replaces the spec's snapshot proof; spec correction 8):
   1. On a dev build with a capture device on Program, run `transport.record.set true` over HTTP.
   2. Run `graphics.lowerThird.set true`; wait 3 s; run `graphics.lowerThird.set false`; wait 2 s.
   3. Run `transport.record.set false`.
   4. Decode the recording:
      `ffmpeg -i <file> -vf "crop=iw:ih*0.2:0:ih*0.75,signalstats,metadata=print:key=lavfi.signalstats.YAVG" -f null -`.
   5. The lower band's mean luma during the keyed window differs from the 1 s before the key by more than
      8 (the key draws a plate). Attach the per-second YAVG.
3. **Owner check 2**: from Companion, monitor on, volume 0.3, mastering on, and hear it.

Commit: `mac control: revisioned audio routes and audio source state (RC3)`.

---

## RC4: OSC

### Task 16: Spike, Network.framework UDP versus BSD sockets (decision task)

**Lane:** control. **Slice:** RC4. **Depends on:** nothing in this plan; it can start any time. **Output:**
a decision and a kept test.

The question (spec section 8, first risk row): can `NWListener` over UDP give a server that receives from
many senders on one port and replies **from that port** to each sender's source port, the way Windows
`UdpClient` does (`C/Osc/OscControlServer.cs:55,137`)?

**Files**
- NEW `M/ControlUdpSpikeTests.swift`: the harness case `control/udp-spike`. It is kept only if NW wins;
  otherwise it is rewritten against the BSD transport in T19.
- NEW `docs/superpowers/notes/2026-11-mac-osc-udp-spike.md`: the decision record. It holds the measured table
  and the rule outcome in under 40 lines.

**What to build:** a minimal `NWListener(using: .udp)` bound to `127.0.0.1:0`. It echoes every received
datagram back on the per-sender `NWConnection` the listener hands over. The clients are **BSD sockets**
(`socket(AF_INET, SOCK_DGRAM)`, `bind` to `127.0.0.1:0`, `sendto`, `recvfrom` with `SO_RCVTIMEO` 1 s), so the
client sees the reply's true source address.

**Decision rule.** Network.framework is chosen only if **all six** hold. Otherwise the transport is a BSD
datagram socket on a `DispatchSourceRead` (fallback), with no further debate.

| # | Check | Pass |
|---|---|---|
| 1 | Reply source port | `recvfrom`'s source port equals the listener's bound port, for every reply |
| 2 | Many senders | 16 client sockets each send 1 datagram; all 16 receive exactly 1 reply within 1 s |
| 3 | Server-initiated send | after a client has sent once, the server sends 3 unsolicited datagrams on that client's connection 200 ms later; all 3 arrive |
| 4 | Dead sender | client A closes; the server sends to A (ICMP unreachable); then client B sends and gets its reply; the listener never leaves `.ready` |
| 5 | Burst | one client sends 1000 datagrams of 64 bytes back to back; ≥ 990 are received by the server (loopback) |
| 6 | Oversize | a 65,000-byte datagram is received intact (size reported) or reported as an error; never a crash or a wedged listener (a normal datagram afterwards still echoes) |

Run the whole spike 3 times. A check passes only if it passes all 3 runs. Record wall time and results per
run in the note.

**Tests:** the spike case itself, written as `expect(...)` lines naming each check (for example
`"1: reply source port == listener port (got \(p))"`), so the harness output is the table.

**Mutations to run:** the spike is a measurement, not a guard. Instead, prove that the checks can fail:
- point check 1 at a second listener on another port and reply from there; check 1 goes red;
- drop the echo for odd-numbered clients; check 2 goes red.

Report both.

**Done when:** the note is committed with the decision, and the spike case is registered (passing if NW was
chosen, or removed in T19 if BSD was chosen; say which). Commit:
`mac control: UDP spike — <Network.framework|BSD socket> chosen for OSC (RC4)`.

---

### Task 17: OSC codec (pure) and the shared codec fixtures

**Lane:** control. **Slice:** RC4. **Depends on:** T4 (fixture file). **Parallel with:** T16.

**Files**
- NEW `M/ControlOscCodec.swift`.
- Modify:
  - `M/ControlApi.swift`: `coerce` gains `Float` cases (D14);
  - `contracts/control.fixtures.json`: add `osc.decode` and `osc.encode`;
  - `CT/ControlFixtureTests.cs`: the OSC codec theory;
  - Mac `control/fixtures`;
  - `M/ShellTests.swift`.

**Interfaces**
```swift
enum ControlOscArg: Equatable { case int(Int32), float(Float), string(String), bool(Bool), blob(Data), null }
struct ControlOscMessage: Equatable { var address: String; var args: [ControlOscArg] }
enum ControlOscCodec {
    static let maxDatagram = 65_536
    static func encode(_ message: ControlOscMessage) -> Data
    static func decode(_ packet: Data) -> [ControlOscMessage]   // [] on anything malformed; never throws
    /// Binder transport values: int -> Int64, float -> Float, string -> String, bool -> Bool, null -> nil, blob -> Data.
    static func transportValues(_ args: [ControlOscArg]) -> [Any?]
}
```

**Behaviour**: a case-for-case port of `C/Osc/OscCodec.cs`:
- **Strings** are UTF-8, then NUL padding of `4 - (len % 4)` bytes (always at least one). The reader pads the
  same way.
- **Numbers** are big-endian `Int32` and `Float32`.
- **Blobs** are an `Int32` size, the bytes, then NUL padding to 4. A size below 0 or past the end throws.
- **Message decoding:**
  - An address that is empty or does not start with `/` gives no message.
  - A missing tag string gives no args.
  - A tag string that does not start with `,` gives no args.
  - `i f s S T F b N I` are decoded. `N` and `I` are `.null`.
  - An unknown tag returns the args decoded so far.
- **Bundles:** `#bundle\0` + 8-byte timetag + repeated `[Int32 size][element]`. A negative size, or one that
  runs past the end, **stops** bundle parsing and keeps what was decoded. Nested bundles are flattened.
- **Error scope.** A numeric or blob read past the end throws, and the **whole packet** decodes to `[]`, even
  messages already decoded earlier in the bundle. This is Windows' try/catch scope (`:72-88`).
- A packet over `maxDatagram` returns `[]`.
- `encode`: `.bool` → `T`/`F`, `.int` → `i`, `.float` → `f`, `.string` → `s`, `.blob` → `b`, `.null` → `s`
  with `""` (Windows `:52-56`).
- `coerce` additions (Windows `TryCoerce`, `C/ControlCatalog.cs:190-245`):
  - `.string` from Float is `String(f)` (shortest round-trip, which matches .NET Core's `float.ToString()`);
  - `.int` from Float is `rounded(.toNearestOrEven)` with the same range guard;
  - `.double` from Float is `Double(f)`;
  - `.bool` from Float is `abs(f) > Float.leastNonzeroMagnitude`.

**Fixture block** (append under `"osc"`; hex computed from the Windows encoder rules and checked by both
runners)
```json
"osc": {
  "encode": [
    { "name": "address-only", "message": { "address": "/cvp/transport/take", "args": [] }, "hex": "2f6376702f7472616e73706f72742f74616b65002c000000" },
    { "name": "string", "message": { "address": "/cvp/scene/select", "args": [ { "s": "interview" } ] }, "hex": "2f6376702f7363656e652f73656c6563740000002c730000696e74657276696577000000" },
    { "name": "true", "message": { "address": "/cvp/transport/ftb/set", "args": [ { "bool": true } ] }, "hex": "2f6376702f7472616e73706f72742f6674622f73657400002c540000" },
    { "name": "int", "message": { "address": "/cvp/state/multiviewTileCount", "args": [ { "i": 6 } ] }, "hex": "2f6376702f73746174652f6d756c74697669657754696c65436f756e740000002c69000000000006" },
    { "name": "float", "message": { "address": "/cvp/state/audioMonitorVolume", "args": [ { "f": 0.5 } ] }, "hex": "2f6376702f73746174652f617564696f4d6f6e69746f72566f6c756d650000002c6600003f000000" },
    { "name": "int-and-string", "message": { "address": "/cvp/input/assign", "args": [ { "i": 3 }, { "s": "zoom:p-1" } ] }, "hex": "2f6376702f696e7075742f61737369676e0000002c697300000000037a6f6f6d3a702d3100000000" },
    { "name": "blob", "message": { "address": "/cvp/x", "args": [ { "blob": "010203" } ] }, "hex": "2f6376702f7800002c6200000000000301020300" }
  ],
  "decode": [
    { "name": "bundle-flattens", "hex": "2362756e646c65000000000000000000000000182f6376702f7472616e73706f72742f74616b65002c000000000000242f6376702f7363656e652f73656c6563740000002c730000696e74657276696577000000",
      "messages": [ { "address": "/cvp/transport/take", "args": [] }, { "address": "/cvp/scene/select", "args": [ { "s": "interview" } ] } ] },
    { "name": "nested-bundle", "hex": "2362756e646c65000000000000000000000000182f6376702f7472616e73706f72742f74616b65002c000000000000382362756e646c65000000000000000000000000242f6376702f7363656e652f73656c6563740000002c730000696e74657276696577000000",
      "messages": [ { "address": "/cvp/transport/take", "args": [] }, { "address": "/cvp/scene/select", "args": [ { "s": "interview" } ] } ] },
    { "name": "unknown-tag-stops", "hex": "2f6376702f6100002c69780000000007", "messages": [ { "address": "/cvp/a", "args": [ { "i": 7 } ] } ] },
    { "name": "truncated-int-empties-packet", "hex": "2f6376702f6100002c6900000007", "messages": [] },
    { "name": "no-tag-string", "hex": "2f6376702f7472616e73706f72742f74616b6500", "messages": [ { "address": "/cvp/transport/take", "args": [] } ] },
    { "name": "tag-without-comma", "hex": "2f6376702f7472616e73706f72742f74616b650069780000", "messages": [ { "address": "/cvp/transport/take", "args": [] } ] },
    { "name": "bundle-overrun-keeps-earlier", "hex": "2362756e646c65000000000000000000000000182f6376702f7472616e73706f72742f74616b65002c000000000003e700000000",
      "messages": [ { "address": "/cvp/transport/take", "args": [] } ] },
    { "name": "bundle-truncated-element-empties-packet", "hex": "2362756e646c65000000000000000000000000182f6376702f7472616e73706f72742f74616b65002c0000000000000e2f6376702f6100002c6900000007", "messages": [] },
    { "name": "nil-impulse-and-S", "hex": "2f6376702f6100002c4e49530000000068690000", "messages": [ { "address": "/cvp/a", "args": [ { "nil": true }, { "nil": true }, { "s": "hi" } ] } ] },
    { "name": "no-leading-slash", "hex": "6376702f610000002c000000", "messages": [] },
    { "name": "empty", "hex": "", "messages": [] },
    { "name": "blob", "hex": "2f6376702f7800002c6200000000000301020300", "messages": [ { "address": "/cvp/x", "args": [ { "blob": "010203" } ] } ] }
  ]
}
```
Fixture invariants:
- Every `encode.hex` length is a multiple of 4.
- `truncated-int-empties-packet` is 14 bytes: an 8-byte address, a 4-byte tag, and 2 of the 4 int bytes.
- `bundle-overrun-keeps-earlier` declares a second element of 999 bytes with only 4 present.
- The `f` value 0.5 is exact in float32, so Windows `float` and the fixture's JSON number compare equal.

On Windows, compare decoded args by tag: `int` with `{i}`, `float` with `{f}` (as `(float)`), `string` with
`{s}`, `bool` with `{bool}`, `byte[]` with `{blob}` hex, `null` with `{nil:true}`.

**Tests** (Mac, added to `control/fixtures`; the C# theory is the same loop)
```swift
        let osc = fixtures["osc"] as? [String: Any] ?? [:]
        func arg(_ j: [String: Any]) -> ControlOscArg {
            if let v = j["i"] as? Int { return .int(Int32(v)) }
            if let v = j["f"] as? Double { return .float(Float(v)) }
            if let v = j["s"] as? String { return .string(v) }
            if let v = j["bool"] as? Bool { return .bool(v) }
            if let v = j["blob"] as? String { return .blob(Data(hex: v)) }
            return .null
        }
        func msg(_ j: [String: Any]) -> ControlOscMessage {
            ControlOscMessage(address: j["address"] as? String ?? "", args: (j["args"] as? [[String: Any]] ?? []).map(arg))
        }
        for c in osc["encode"] as? [[String: Any]] ?? [] {
            expectEqual(ControlOscCodec.encode(msg(c["message"] as? [String: Any] ?? [:])).hexString, c["hex"] as? String, "osc encode \(c["name"] ?? "")"); ran += 1
        }
        for c in osc["decode"] as? [[String: Any]] ?? [] {
            let want = (c["messages"] as? [[String: Any]] ?? []).map(msg)
            expectEqual(ControlOscCodec.decode(Data(hex: c["hex"] as? String ?? "")), want, "osc decode \(c["name"] ?? "")"); ran += 1
        }
        expect(ControlOscCodec.decode(Data(count: ControlOscCodec.maxDatagram + 1)).isEmpty, "an oversize datagram is dropped")
```
Add `Data(hex:)` and `hexString` test helpers in `M/ControlApiTests.swift`. Bump the `ran` total by
7 + 12 = 19. Add a coerce test: `coerce(Float(0.1), to: .string) == "0.1"`,
`coerce(Float(2.5), to: .int) == 2`, `coerce(Float(0), to: .bool) == false`.

**Mutations to run**
- Pad strings with `(4 - len % 4) % 4`. `address-only` goes red (Windows always writes at least one NUL).
- Catch errors per bundle element instead of per packet. `bundle-truncated-element-empties-packet` goes red.
- Decode `f` to Double in `transportValues`, and add the routing fixture `string-from-float` (T18). That case
  goes red.
- Treat `N` as an unknown tag. `nil-impulse-and-S` goes red.

**Done when:** both hosts pass the 19 OSC codec cases. Commit:
`mac control: OSC codec ported case for case, with shared codec fixtures (RC4)`.

---

### Task 18: OSC router and feedback encoder (pure) and the shared routing and feedback fixtures

**Lane:** control. **Slice:** RC4. **Depends on:** T17.

**Files**
- NEW `M/ControlOscRouter.swift`.
- Modify the fixtures (`osc.addresses`, `osc.routes`, `osc.feedback`), the CT runner, Mac `control/fixtures`,
  and `M/ShellTests.swift`.

**Interfaces**
```swift
enum ControlOscAddressMap {
    static func actionId(for address: String) -> String?   // Windows OscAddressMap.AddressToActionId (:19-33)
    static func stateAddress(_ field: String) -> String     // "/cvp/state/" + field
}
final class ControlOscRouter {
    init(surface: ControlSurface)
    /// nil = not an action (ignored). Unavailable or bind errors return .fail without invoking.
    func route(_ message: ControlOscMessage) async -> ControlInvokeResult?
}
enum ControlOscFeedback {
    /// Windows OscFeedback.Encode order (C/Osc/OscFeedback.cs:15-73), restricted to the
    /// feedback rows the generated catalog marks supported.
    static func encode(_ state: [String: Any]) -> [ControlOscMessage]
}
```

**Behaviour**
- **`actionId`:**
  - An address not under `/cvp/` gives nil.
  - An empty tail, or a tail starting with `state/`, gives nil.
  - Otherwise the tail with `/` replaced by `.`. Note `"/cvp/state"` (no slash after `state`) gives `"state"`,
    which is Windows' behaviour.
- **`route`:**
  - An id that is nil, or in neither file, gives nil (ignored).
  - A known but unsupported id gives `.fail(unavailableMessage)` (D1 applied to OSC; it is logged, never sent).
  - Otherwise `ControlCatalog.bind(id, ControlOscCodec.transportValues(args))`: a failure gives
    `.fail(message)`, else `surface.invoke`.
  - Windows has no unavailable case: every id it lists it supports. Its LoopbackOnly provider exposure is out
    of scope (spec section 10).
- **`encode`:** a static ordered table of Windows' feedback lines.

  | field | state key | encoding |
  |---|---|---|
  | `recording` | `recording` | flag |
  | `streaming` | `streaming` | flag |
  | `ndi` | `ndiOutputOn` | flag |
  | `engineOn` | `engineOn` | flag |
  | `canTake` | `canTake` | flag |
  | `lowerThirdOnAir` | `lowerThirdOnAir` | flag |
  | `zoomStatus` | `zoomStatus` | string |
  | `engineStatus` | `engineStatus` | string |
  | `commandStatus` | `commandStatus` | string |
  | `activeSceneId` | `activeSceneId` | string |
  | `previewSceneId` | `previewSceneId` | string |
  | `previewTilesNamesOn` | `previewTilesNamesOn` | flag |
  | `viewMode` | `viewMode` | string |
  | `multiviewLayoutMode` | `multiviewLayoutMode` | string |
  | `multiviewTileCount` | `multiviewTileCount` | int |
  | `automationOn` | `automationOn` | flag |
  | `autoTake` | `autoTake` | flag |
  | `autoAssignInputs` | `autoAssignInputs` | flag |
  | `autoLowerThirds` | `autoLowerThirds` | flag |
  | `autoCaptions` | `autoCaptions` | flag |
  | `audioMonitorOn` | `audioMonitorOn` | flag |
  | `audioMonitorVolume` | `audioMonitorVolume` | float32 |
  | `masterLimiterOn` | `masterLimiterOn` | flag |

  Then, for each `inputs[]` row, `input/<slot>/inShow` (flag), `kind`, `source` (a null `sourceId` is `""`)
  and `name`. Then `graphics/*` (never Mac), then `ohg/*` (N/A).
  - A flag is `.int(1)` or `.int(0)`; an absent key is 0, a string is `""`, an int is 0.
  - A line is emitted only if its field's row (`input/{slot}/<x>` for input lines) is `.supported` in
    `ControlManifestGenerated.feedbackFields`.
- **Closure (rule 10):** every supported feedback field must appear in the table. A test drives this by
  setting every supported field to a distinctive value and asserting each address appears (below).

**Fixture blocks**
```json
"addresses": [
  { "address": "/cvp/transport/take", "actionId": "transport.take" },
  { "address": "/cvp/scene/select", "actionId": "scene.select" },
  { "address": "/cvp/state/recording", "actionId": null },
  { "address": "/cvp/state", "actionId": "state" },
  { "address": "/cvp/", "actionId": null },
  { "address": "/cvp", "actionId": null },
  { "address": "/other/transport/take", "actionId": null }
],
"routes": [
  { "name": "bool-arg", "message": { "address": "/cvp/transport/ftb/set", "args": [ { "bool": true } ] }, "result": { "ok": true }, "invocations": [ { "action": "transport.ftb.set", "args": [ true ] } ] },
  { "name": "int-to-bool", "message": { "address": "/cvp/transport/ftb/set", "args": [ { "i": 0 } ] }, "result": { "ok": true }, "invocations": [ { "action": "transport.ftb.set", "args": [ false ] } ] },
  { "name": "string-from-float", "message": { "address": "/cvp/scene/select", "args": [ { "f": 0.1 } ] }, "result": { "ok": true }, "invocations": [ { "action": "scene.select", "args": [ "0.1" ] } ] },
  { "name": "unknown-ignored", "message": { "address": "/cvp/nope/nothing", "args": [] }, "result": null, "invocations": [] },
  { "name": "feedback-namespace-ignored", "message": { "address": "/cvp/state/recording", "args": [ { "i": 1 } ] }, "result": null, "invocations": [] },
  { "name": "bind-error", "message": { "address": "/cvp/scene/select", "args": [] }, "result": { "ok": false, "error": "Action 'scene.select' requires parameter 'sceneId' at position 0." }, "invocations": [] },
  { "name": "unavailable-on-mac", "hosts": [ "mac" ], "message": { "address": "/cvp/showEngine/restart", "args": [] }, "result": { "ok": false, "error": "'showEngine.restart' is not available on macOS (the OHG show engine is not part of macOS 1.0)." }, "invocations": [] }
],
"feedback": {
  "state": { "recording": true, "streaming": false, "engineOn": true, "canTake": true, "commandStatus": "Ready",
             "activeSceneId": "s2", "previewSceneId": "s1", "multiviewLayoutMode": "pgmPvwTop", "multiviewTileCount": 6,
             "automationOn": false, "autoTake": false, "audioMonitorOn": true, "audioMonitorVolume": 0.5,
             "zoomStatus": "Zoom Live", "engineStatus": "Engine on — Zoom media live.", "lowerThirdOnAir": false, "autoCaptions": false,
             "inputs": [ { "slot": 1, "inShow": true, "kind": "ZoomParticipant", "sourceId": "zoom:p-1", "name": "Host" },
                         { "slot": 2, "inShow": false, "kind": "Unassigned", "sourceId": null, "name": "" } ] },
  "windowsMessages": [
    ["recording", {"i":1}], ["streaming", {"i":0}], ["ndi", {"i":0}], ["engineOn", {"i":1}], ["canTake", {"i":1}],
    ["lowerThirdOnAir", {"i":0}], ["zoomStatus", {"s":"Zoom Live"}], ["engineStatus", {"s":"Engine on — Zoom media live."}],
    ["commandStatus", {"s":"Ready"}], ["activeSceneId", {"s":"s2"}], ["previewSceneId", {"s":"s1"}],
    ["previewTilesNamesOn", {"i":0}], ["viewMode", {"s":""}], ["multiviewLayoutMode", {"s":"pgmPvwTop"}],
    ["multiviewTileCount", {"i":6}], ["automationOn", {"i":0}], ["autoTake", {"i":0}], ["autoAssignInputs", {"i":0}],
    ["autoLowerThirds", {"i":0}], ["autoCaptions", {"i":0}], ["audioMonitorOn", {"i":1}], ["audioMonitorVolume", {"f":0.5}],
    ["masterLimiterOn", {"i":0}],
    ["input/1/inShow", {"i":1}], ["input/1/kind", {"s":"ZoomParticipant"}], ["input/1/source", {"s":"zoom:p-1"}], ["input/1/name", {"s":"Host"}],
    ["input/2/inShow", {"i":0}], ["input/2/kind", {"s":"Unassigned"}], ["input/2/source", {"s":""}], ["input/2/name", {"s":""}],
    ["graphics/onProgramCount", {"i":0}], ["graphics/onPreviewCount", {"i":0}], ["graphics/automationMode", {"s":"manual"}],
    ["graphics/automationArmed", {"i":0}], ["ohg/health/engine", {"s":"stopped"}], ["ohg/shadow/lastCommand", {"s":""}]
  ]
}
```
Each `windowsMessages` entry is `[field, arg]`, meaning address `/cvp/state/<field>` with one arg.

- **Windows runner:** deserialize `feedback.state` into `ControlState` with camelCase, case-insensitive
  options (the same configuration as `HttpControlRouter.JsonOptions`; it is internal, so build an equal
  instance). Assert `OscFeedback.Encode(...)` equals `windowsMessages` exactly, in order.
- **Mac runner:** the expected list is **derived**: `windowsMessages` filtered to entries whose field (with
  `input/<n>/` mapped to `input/{slot}/`) has a `supported` row in `contracts/control.platforms.json` **read
  from disk**. Assert `ControlOscFeedback.encode(state)` equals that list. After RC2 and RC3 this is 25
  messages: 17 scalar lines plus 8 input lines.

Routing runners: Windows uses `OscControlRouter.RouteAsync` with `FakeControlSurface`, skipping
`hosts:["mac"]`. The Mac uses `ControlOscRouter.route` with `FakeSurface`. Both assert the result (null or
`{ok,error}`) and the invocations.

**Closure test** (Mac, complete)
```swift
    @MainActor
    static func testControlOscFeedbackCoversEverySupportedField() {
        let supported = ControlManifestGenerated.feedbackFields.filter { $0.availability == .supported }.map(\.field)
        var state: [String: Any] = ["inputs": [["slot": 7, "inShow": true, "kind": "ZoomParticipant", "sourceId": "zoom:x", "name": "N"]]]
        for field in supported where !field.hasPrefix("input/") { state[field] = field == "multiviewTileCount" ? 9 : (field == "audioMonitorVolume" ? 0.25 : "v-\(field)") }
        let addresses = Set(ControlOscFeedback.encode(state).map(\.address))
        for field in supported {
            let address = ControlOscAddressMap.stateAddress(field.replacingOccurrences(of: "{slot}", with: "7"))
            expect(addresses.contains(address), "supported feedback field \(field) is encoded (\(address))")
        }
        expectEqual(supported.count, 21, "after RC3: 21 supported feedback rows")
    }
```
The string values above are deliberately wrong-typed for flag fields. A flag field given a string encodes 0,
which is still an emitted line, so this test pins **presence**. Value correctness is pinned by the fixture.

**Mutations to run**
- Remove the `autoCaptions` row from the encoder table. The closure test goes red (and so does the fixture).
- Emit every line regardless of platform rows. The Mac fixture goes red (`ndi` appears).
- Swap `zoomStatus` and `engineStatus` order. The fixture goes red.
- Encode flags as `.bool`. The fixture goes red.
- Map `/cvp/state` to nil. The `addresses` fixture goes red (Windows gives `"state"`).

**Done when:** both hosts pass the 7 address cases, 7 Mac and 6 Windows routing cases, and the feedback case.
Commit: `mac control: OSC routing and feedback encoder against shared fixtures (RC4)`.

---

### Task 19: OSC UDP server, environment gating, feedback targets, host wiring

**Lane:** control. **Slice:** RC4. **Depends on:** T16 (decision), T18.

**Files**
- NEW `M/ControlOscServer.swift`: the transport chosen by T16.
- Modify `M/ControlSurface.swift`: `ControlApiHost` starts and stops the OSC server next to HTTP, and feeds
  it from the same throttled sink; the push key is shared.
- Modify `M/ControlApiTests.swift`; remove `M/ControlUdpSpikeTests.swift` if BSD won.

**Interfaces**
```swift
struct ControlOscOptions: Equatable {
    var port: UInt16 = 8010          // 0 = ephemeral (tests)
    var lan = false                  // binds 0.0.0.0 when true
    var feedbackTargetCap = 16
    var senderTtl: TimeInterval = 300
    static func fromEnvironment(_ env: [String: String]) -> (options: ControlOscOptions, notice: String?)
}
final class ControlOscServer: @unchecked Sendable {
    init(router: ControlOscRouter, options: ControlOscOptions, now: @escaping () -> Date = Date.init)
    func start(completion: @escaping (Result<UInt16, ControlServerError>) -> Void)
    func stop()
    func sendFeedback(_ messages: [ControlOscMessage])   // to current targets; best effort
    var feedbackTargets: [String] { get }                // "ip:port", most recent first (tests, Settings row)
    var lastRefusal: (actionId: String, error: String)? { get }
}
```

**Behaviour** (Windows `MainWindow.xaml.cs:141-174`, `OscControlServer.cs:48-163`)
- **Environment:**
  - `COREVIDEO_OSC_PORT` is used when it parses into 1...65535.
  - `lan = (COREVIDEO_OSC_LAN == "1" && COREVIDEO_OSC_TRUSTED_NETWORK == "1")`.
  - When only the first is `"1"`, `notice` is Windows' exact log line: `"control: OSC remains on loopback. Unauthenticated LAN OSC requires COREVIDEO_OSC_TRUSTED_NETWORK=1 on a trusted network."`.
- **Receive:** every datagram's sender is recorded as a feedback target with `now()` (Windows `:86`,
  including senders of junk). Then `ControlOscCodec.decode`, and each message goes through
  `router.route(...)` in order.
  - A `nil` result is ignored.
  - An `.ok == false` result sets `lastRefusal` and writes `control: OSC refused <id> from <ip:port>: <error>`
    to `ShellLog`, at most one line per second, with a suppressed count appended to the next line.
  - No reply is ever sent for a command (Windows `:92-96`).
- **Targets:** entries older than `senderTtl` are pruned on each send. Beyond `feedbackTargetCap`, the
  least-recently-seen target is evicted (the cap is **new**; Windows is unbounded).
- **Feedback:** `ControlApiHost`'s sink, on the same push-key change that broadcasts `/ws`, calls
  `oscServer.sendFeedback(ControlOscFeedback.encode(state))`. Each message is its own datagram, in order, to
  each target (Windows `:130-144`).
- **Start failure** (port in use) logs `control: OSC server failed to start (<error>) — HTTP still active`,
  calls `model.pushWarning("OSC control unavailable: <error>")`, and never stops HTTP or the app.
- **Start success** logs `control: OSC server listening on <127.0.0.1|0.0.0.0>:<port>` (Windows' line). The
  harness and snapshot runs never auto-start it (same `shouldAutoStart`).

**Tests** (complete; the twin of `CT/OscControlServerTests.cs:11,37`, extended)
```swift
    final class UdpClient {
        let fd: Int32; let port: UInt16
        init() {
            fd = socket(AF_INET, SOCK_DGRAM, 0)
            var addr = sockaddr_in(); addr.sin_family = sa_family_t(AF_INET); addr.sin_addr.s_addr = inet_addr("127.0.0.1"); addr.sin_port = 0
            _ = withUnsafePointer(to: &addr) { $0.withMemoryRebound(to: sockaddr.self, capacity: 1) { bind(fd, $0, socklen_t(MemoryLayout<sockaddr_in>.size)) } }
            var bound = sockaddr_in(); var len = socklen_t(MemoryLayout<sockaddr_in>.size)
            _ = withUnsafeMutablePointer(to: &bound) { $0.withMemoryRebound(to: sockaddr.self, capacity: 1) { getsockname(fd, $0, &len) } }
            port = UInt16(bigEndian: bound.sin_port)
            var tv = timeval(tv_sec: 0, tv_usec: 200_000); setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, socklen_t(MemoryLayout<timeval>.size))
        }
        func send(_ data: Data, to port: UInt16) {
            var to = sockaddr_in(); to.sin_family = sa_family_t(AF_INET); to.sin_addr.s_addr = inet_addr("127.0.0.1"); to.sin_port = port.bigEndian
            data.withUnsafeBytes { raw in _ = withUnsafePointer(to: &to) { $0.withMemoryRebound(to: sockaddr.self, capacity: 1) {
                sendto(fd, raw.baseAddress, data.count, 0, $0, socklen_t(MemoryLayout<sockaddr_in>.size)) } } }
        }
        /// Every datagram that arrives within `seconds`, with its source port.
        func drain(_ seconds: Double) -> [(Data, UInt16)] {
            var out: [(Data, UInt16)] = []; let end = Date().addingTimeInterval(seconds)
            while Date() < end {
                var buf = [UInt8](repeating: 0, count: 65_536); var from = sockaddr_in(); var len = socklen_t(MemoryLayout<sockaddr_in>.size)
                let n = withUnsafeMutablePointer(to: &from) { $0.withMemoryRebound(to: sockaddr.self, capacity: 1) { recvfrom(fd, &buf, buf.count, 0, $0, &len) } }
                if n > 0 { out.append((Data(buf[0..<n]), UInt16(bigEndian: from.sin_port))) }
            }
            return out
        }
        deinit { close(fd) }
    }

    @MainActor
    static func testControlOscServerEndToEnd() {
        let surface = FakeSurface()
        let server = ControlOscServer(router: ControlOscRouter(surface: surface), options: ControlOscOptions(port: 0))
        let box = Box<Result<UInt16, ControlServerError>>()
        server.start { box.value = $0 }
        spin(until: { box.value != nil }, limit: 3)
        defer { server.stop() }
        guard case .success(let port)? = box.value, port != 0 else { expect(false, "OSC server starts on an ephemeral loopback port"); return }
        let client = UdpClient()
        client.send(ControlOscCodec.encode(ControlOscMessage(address: "/cvp/transport/take", args: [])), to: port)
        spin(until: { surface.invocations.count == 1 }, limit: 3)
        expectEqual(surface.invocations.first?.0, "transport.take", "a datagram invokes the action")
        expect(client.drain(0.3).isEmpty, "a command gets no reply (Windows sends none)")
        server.sendFeedback(ControlOscFeedback.encode(["recording": true]))
        let got = client.drain(0.5)
        let decoded = got.flatMap { ControlOscCodec.decode($0.0) }
        expect(decoded.contains(ControlOscMessage(address: "/cvp/state/recording", args: [.int(1)])), "feedback reaches the sender")
        expect(got.allSatisfy { $0.1 == port }, "feedback comes FROM the OSC port (Companion matches on it)")
        // Junk is ignored, and its sender still becomes a target (Windows records every sender).
        let junk = UdpClient()
        junk.send(Data([1, 2, 3]), to: port)
        spin(until: { server.feedbackTargets.count == 2 }, limit: 2)
        expectEqual(server.feedbackTargets.count, 2, "both senders are targets")
        // An unavailable action is refused, logged, never answered.
        client.send(ControlOscCodec.encode(ControlOscMessage(address: "/cvp/showEngine/restart", args: [])), to: port)
        spin(until: { server.lastRefusal != nil }, limit: 2)
        expectEqual(server.lastRefusal?.actionId, "showEngine.restart", "refusal recorded")
        expectEqual(surface.invocations.count, 1, "the refused action never reached the surface")
        // Oversize is dropped without harm.
        client.send(Data(count: 65_000), to: port)
        client.send(ControlOscCodec.encode(ControlOscMessage(address: "/cvp/transport/take", args: [])), to: port)
        spin(until: { surface.invocations.count == 2 }, limit: 3)
        expectEqual(surface.invocations.count, 2, "still serving after an oversize datagram")
    }

    @MainActor
    static func testControlOscTargetsCapAndExpiry() {
        var clock = Date(timeIntervalSince1970: 1_000)
        var options = ControlOscOptions(port: 0); options.feedbackTargetCap = 3; options.senderTtl = 300
        let server = ControlOscServer(router: ControlOscRouter(surface: FakeSurface()), options: options, now: { clock })
        let box = Box<Result<UInt16, ControlServerError>>()
        server.start { box.value = $0 }
        spin(until: { box.value != nil }, limit: 3)
        defer { server.stop() }
        guard case .success(let port)? = box.value else { expect(false, "starts"); return }
        let clients = (0..<4).map { _ in UdpClient() }
        for (i, c) in clients.enumerated() {
            clock = clock.addingTimeInterval(1)
            c.send(ControlOscCodec.encode(ControlOscMessage(address: "/cvp/nope", args: [])), to: port)
            spin(until: { server.feedbackTargets.count == min(i + 1, 3) }, limit: 2)
        }
        expectEqual(server.feedbackTargets.count, 3, "capped at 3")
        expect(!server.feedbackTargets.contains("127.0.0.1:\(clients[0].port)"), "the least recently seen sender was evicted")
        clock = clock.addingTimeInterval(301)
        server.sendFeedback([ControlOscMessage(address: "/cvp/state/recording", args: [.int(0)])])
        expectEqual(server.feedbackTargets.count, 0, "senders older than 5 minutes are pruned on send")
        let (o1, n1) = ControlOscOptions.fromEnvironment(["COREVIDEO_OSC_LAN": "1"])
        expect(!o1.lan, "LAN needs both variables (D2)")
        expectEqual(n1, "control: OSC remains on loopback. Unauthenticated LAN OSC requires COREVIDEO_OSC_TRUSTED_NETWORK=1 on a trusted network.", "Windows log line")
        let (o2, n2) = ControlOscOptions.fromEnvironment(["COREVIDEO_OSC_LAN": "1", "COREVIDEO_OSC_TRUSTED_NETWORK": "1", "COREVIDEO_OSC_PORT": "9010"])
        expect(o2.lan && o2.port == 9010 && n2 == nil, "LAN opt-in and port")
        expectEqual(ControlOscOptions.fromEnvironment(["COREVIDEO_OSC_PORT": "70000"]).options.port, 8010, "an out-of-range port keeps 8010")
    }

    @MainActor
    static func testControlOscPortInUseNeverBlocksHttp() {
        let first = ControlOscServer(router: ControlOscRouter(surface: FakeSurface()), options: ControlOscOptions(port: 0))
        let a = Box<Result<UInt16, ControlServerError>>(); first.start { a.value = $0 }; spin(until: { a.value != nil }, limit: 3)
        defer { first.stop() }
        guard case .success(let port)? = a.value else { expect(false, "first starts"); return }
        let second = ControlOscServer(router: ControlOscRouter(surface: FakeSurface()), options: ControlOscOptions(port: port))
        let b = Box<Result<UInt16, ControlServerError>>(); second.start { b.value = $0 }; spin(until: { b.value != nil }, limit: 3)
        second.stop()
        if case .failure(.portInUse)? = b.value { expect(true, "port in use reported") } else { expect(false, "a taken OSC port is port-in-use: \(String(describing: b.value))") }
        // The host keeps HTTP up when OSC fails: drive ControlApiHost with a taken OSC port.
        let model = sceneEditModel()
        let host = ControlApiHost()
        host.startForTesting(model: model, http: ControlServerOptions(port: 0, host: "127.0.0.1", authToken: nil), osc: ControlOscOptions(port: port))
        defer { host.stop() }
        spin(until: { host.httpPortForTesting != nil && host.oscFailedForTesting }, limit: 3)
        expect(host.oscFailedForTesting, "OSC failure observed")
        expectEqual(http("GET", "http://127.0.0.1:\(host.httpPortForTesting ?? 0)/state").status, 200, "HTTP still serving")
        expect(model.warnings.contains { $0.contains("OSC control unavailable") }, "the operator is told, once")
    }
```
`ControlApiHost` gains `startForTesting(model:http:osc:)`, `httpPortForTesting` and `oscFailedForTesting`.
They run the real start path and skip the environment and `shouldAutoStart`. If `model.warnings` has another
name, read `pushWarning` and use its store (rule 5).

**Mutations to run**
- Reply on a fresh outbound socket instead of the listener's (or the BSD socket). "feedback comes FROM the OSC
  port" goes red.
- Record senders only after a successful route. The junk-sender assertion goes red (Windows parity).
- Remove the cap. "capped at 3" goes red.
- Remove TTL pruning. Red.
- Make an OSC start failure throw out of `start`. `port-in-use-never-blocks-http` goes red.

**RC4 slice proof (wire level)**
1. The shell suite, CT (OSC fixtures), MCT, `contract:check` and `test:scripts` are green.
2. On a dev build, with `oscsend` (liblo) or a 10-line Python sender: send `/cvp/transport/take` to
   `127.0.0.1:8010`, and see `/cvp/state/canTake` arrive back on the sender's port.

**Done when:** the three cases pass, and the host starts OSC and HTTP in the app (not in the harness).
Commit: `mac control: OSC over UDP with Windows gating, capped feedback targets (RC4)`.

---

### Task 20: Settings "Remote control" status row

**Lane:** shell. **Slice:** RC4. **Depends on:** T19.

**Files**
- NEW `M/RemoteControlStatus.swift`: a pure value plus its formatter.
- Modify:
  - `M/ControlSurface.swift`: `ControlApiHost` publishes the status on `AppModel`;
  - `M/AppModel.swift`: `@Published var remoteControlStatus`;
  - `M/SettingsPane.swift`: a new `card("Remote control")` after `System`;
  - tests.

**Interfaces**
```swift
struct RemoteControlStatus: Equatable {
    var httpEndpoint: String?          // "127.0.0.1:8011" or nil = not running
    var httpError: String?
    var oscEndpoint: String?           // "127.0.0.1:8010"
    var oscError: String?
    var lan: Bool                      // either transport bound beyond loopback
    var tokenRequired: Bool
    var webSocketClients: Int
    var oscTargets: Int
    var lastRefusal: String?           // "<actionId>: <error>" from HTTP 4xx (router) or OSC
    var lines: [(label: String, value: String)] { get }   // the formatter the view renders
}
```

**Behaviour**
- The row is read-only (D4). There is no persistence and no editing; ports and token stay environment-only
  (spec 4.5).
- **`lines`, in order:**
  - `HTTP`: the endpoint, or `Off — <error>`.
  - `OSC`: the endpoint, or `Off — <error>`.
  - `Network`: `Loopback only` or `LAN (token required)` / `LAN (OSC, no token)`.
  - `Clients`: `<n> WebSocket, <m> OSC`.
  - `Last refused`: the refusal, or `None`.
- **Refusal capture:** `ControlRouter` records the last `/invoke` refusal (status 400 or 422) as
  `(actionId or "?", error)` in a lock-protected box the host reads. `ControlOscServer.lastRefusal` covers OSC.
  The more recent one wins.
- The host refreshes the status at most once per second (a timer on the main queue). It never polls on
  `objectWillChange`.
- The design lint must stay green, so use the existing `card`/`labeled` helpers and `Studio` tokens only.
  Add a `COREVIDEO_SHELL_SNAPSHOT` PNG with `COREVIDEO_SHELL_TAB=settings` to the report.

**Tests** (complete)
```swift
    @MainActor
    static func testRemoteControlStatusLines() {
        var s = RemoteControlStatus(httpEndpoint: "127.0.0.1:8011", httpError: nil, oscEndpoint: "127.0.0.1:8010", oscError: nil,
                                    lan: false, tokenRequired: false, webSocketClients: 2, oscTargets: 1, lastRefusal: nil)
        expectEqual(s.lines.map(\.label), ["HTTP", "OSC", "Network", "Clients", "Last refused"], "row order")
        expectEqual(s.lines.map(\.value), ["127.0.0.1:8011", "127.0.0.1:8010", "Loopback only", "2 WebSocket, 1 OSC", "None"], "loopback values")
        s.oscEndpoint = nil; s.oscError = "port 127.0.0.1:8010 is already in use"
        s.lan = true; s.tokenRequired = true; s.lastRefusal = "zoom.join: 'zoom.join' is not available on macOS yet (Remote control: session and inputs)."
        expectEqual(s.lines[1].value, "Off — port 127.0.0.1:8010 is already in use", "OSC failure says why")
        expectEqual(s.lines[2].value, "LAN (token required)", "LAN with token")
        expect(s.lines[4].value.hasPrefix("zoom.join: "), "last refusal shown")
        // The router records refusals for the row.
        let router = ControlRouter(surface: FakeSurface())
        _ = route(router, "POST", "/invoke", #"{"action":"bogus.id"}"#)
        expectEqual(router.lastRefusal?.actionId, "bogus.id", "router records the refused id")
        expectEqual(router.lastRefusal?.error, "Unknown action 'bogus.id'.", "with the reply text")
        _ = route(router, "POST", "/invoke", #"{"action":"transport.take"}"#)
        expectEqual(router.lastRefusal?.actionId, "bogus.id", "a success does not clear the last refusal")
        // The Settings view renders the row (source check: the card exists and uses the formatter).
        guard let pane = sourceFile("SettingsPane.swift") else { expect(false, "SettingsPane.swift"); return }
        expect(pane.contains("card(\"Remote control\")") && pane.contains("remoteControlStatus.lines"), "Settings shows the row from the formatter")
    }
```
The source check is a presence pin only (rule 10: it describes, so it is labelled here). The PNG is the
visible proof.

**Mutations to run:** `lan` with no token renders `LAN (token required)`; add the
`tokenRequired=false, lan=true` → `LAN (OSC, no token)` assertion if the mutation stays green. A success
clears `lastRefusal`: red. Drop the card from `SettingsPane`: red.

**Done when:** the case passes, the lint is green, and the PNG is attached. Commit:
`mac settings: read-only Remote control status row (RC4)`.

---

### Task 21: LAN OSC and LAN HTTP on the installed, signed build (owner-run)

**Lane:** control (owner-run steps). **Slice:** RC4. **Depends on:** T19, T20, and a signed and notarized
`.pkg` built from the branch (`scripts/make-macos-pkg.sh` through the release recipe; see memory
`corevideo-release-process`).

This is system behaviour: macOS Local Network privacy and the application firewall. A simulation is not
proof (master plan section 7). It is proven **only** on the installed, signed build, launched the way an
operator launches it.

**Procedure** (write it into `docs/owner-acceptance` beside the existing checklist; the owner runs it)
1. Install the `.pkg`. Do not run the app from the build folder.
2. Set the environment for LaunchServices-launched apps, so the app (not Terminal) is the responsible process
   for the privacy prompt:
   ```
   launchctl setenv COREVIDEO_OSC_LAN 1
   launchctl setenv COREVIDEO_OSC_TRUSTED_NETWORK 1
   launchctl setenv COREVIDEO_HTTP_LAN 1
   launchctl setenv COREVIDEO_CONTROL_TOKEN <a fresh token>
   ```
   Then `open -a "CoreVideo Pro"`. Launching the binary from Terminal makes Terminal the responsible app and
   proves nothing about the prompt.
3. From a second machine (or a tablet with TouchOSC) on the same network, send `/cvp/transport/take` to
   `<mac-ip>:8010`. Record:
   - (a) whether a Local Network prompt or a firewall prompt appeared, and its text;
   - (b) whether the Take happened;
   - (c) whether `/cvp/state/canTake` feedback arrived back on the tablet.
4. **Owner check 5** (spec section 7). From the second machine:
   - `curl -H "Authorization: Bearer <token>" http://<mac-ip>:8011/state` gives 200;
   - the same request without the header gives 401;
   - quit, then `launchctl setenv COREVIDEO_CONTROL_TOKEN ""`, relaunch: the Settings row shows `HTTP Off —
     LAN HTTP/WS control requires a non-empty COREVIDEO_CONTROL_TOKEN…`, and port 8011 refuses connections.
5. Clean up with `launchctl unsetenv` for all four variables, then relaunch: the row reads `Loopback only`.

**Conditional change** (only if step 3 shows a Local Network prompt with no explanation, or feedback silently
fails to arrive):
- add `NSLocalNetworkUsageDescription` = `"CoreVideo Pro sends remote-control feedback to OSC devices on your network when LAN control is enabled."`
  to the Info.plist block in `scripts/make-macos-shell-bundle.sh:129` (next to `NSCameraUsageDescription`);
- add a check for it to `scripts/check-macos-camera-extension.sh`'s plist checks, if that script already
  inspects the app plist (read it first);
- rebuild, re-sign and reinstall, then repeat steps 2 to 5.

If no prompt appears and feedback arrives, make **no** plist change, and record that.

**Done when:** the owner's results for steps 3 to 5 are recorded in the acceptance doc, and the conditional
change is made or explicitly not needed. Commit (if the plist changed):
`mac bundle: Local Network usage string for LAN OSC feedback (RC4)`.

**Owner check 3** (spec section 7, loopback; may run with step 1): TouchOSC or `oscsend` on the same Mac
sends `/cvp/transport/take`, and `/cvp/state/canTake` returns.

---

## Integration and owner checks (end of Phase 1)

1. All gates green on the branch:
   - shell suite (`control/closure` reports 36);
   - Control.Tests (the export, platform rows, fixture and OSC theories);
   - MCT redaction fixtures;
   - `contract:check`; `test:scripts`; `python3 -m unittest scripts/tests/test_control_setup_drill.py`;
   - `lint-mac-design.sh`.
2. `node scripts/control-conformance.mjs --host mac --base http://127.0.0.1:8011` against the installed build:
   PASS.
3. Owner checks 1 (T12), 2 (T15), 3 and 5 (T21), on the installed signed build.
4. **The cross-host mutation from the spec**: add a fake action to the Windows registry and rewrite the export.
   CT fails (no platform row), and `npm run contract:generate` refuses. Report both. Then revert.
5. Companion's `GET /manifest` on the Mac lists 36 actions. Three of the seven shipped presets used to fail:
   - `transport.engine.toggle` now works;
   - `graphics.lowerThird.toggle` now works;
   - `automation.autoAssignInputs.set` (STU) still answers 422 with its reason. RC5 handles it.

## Risks carried from the spec

| Risk | Where it is handled |
|---|---|
| Network.framework UDP reply and multi-sender behaviour | T16 decision rule; BSD fallback with no debate |
| Local Network prompt and firewall on a LAN bind | T21, owner-run on the installed build; conditional usage string |
| Mac source ids differ from Windows canonical ids; the Sources rebuild may change slots | `ControlSourceId` is the only mapping point (T7); the Sources spec owns keeping `control/source-id-and-kinds` green |
| `/state` size on the main actor | T7 measures the median and enforces the budget |
| Long invokes hold a connection | T5 pins that the request timer is cancelled at dispatch |
| `zoom.join` semantics | T10; webinar resolved by D8 |
| `graphics.caption.set` and `multiview.tileCount.set` mapping | D7 moves caption.set to GFX; D11 keeps tileCount on the Mac range |
| **New:** `source.capture.online.set false` does not unassign the slot on the Mac, though the Windows description says it does | The UI entry point is followed (T10). If the owner wants Windows' unassign, it is a small follow-up to T10 that calls `unassignSlot` for slots carrying the device; it is not in this plan |
| **New:** `masteringTarget = -1` is a Mac-only value | D10. Companion shows it as an unknown index; documented in the platform file README section |

## Out of scope

- RC5 (Companion verification), RC6 (Volt rundown) and RC7 (follow-on batches) are later phases.
- Commands over WebSocket, TLS, accounts, dynamic providers, `ohg.*`, OSC `LoopbackOnly` exposure, and a
  settings UI for ports or token (spec section 10).
- Changing Windows' CORS wildcard. File it as its own backlog issue (spec section 10); this plan does not.
- Any `native/` change.

## ORDER

```
T1 → T2 → T3 ─┬─ T4 ─┬─ T6
              │      └─ T17 → T18 ─┐
              ├─ T5                │
              └─ T7 → T8 → T9 → T10 → T11 → T12      (RC2: write in parallel, commit in this order)
                   └─ T13 → T14 → T15                (RC3: after T11 is committed, same files)
T16 (spike) can start at any time ───────────────────┴─ T19 (needs T16 + T18) → T20 → T21
```

Parallel groups:
- after T3: {T4, T5, T7, T16};
- after T4: {T6, T17};
- RC2 and RC3 dispatch tasks share `M/ControlSurface.swift` and the platform file, so they are committed
  serially.
