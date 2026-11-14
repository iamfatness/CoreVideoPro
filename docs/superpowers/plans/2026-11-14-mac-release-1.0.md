# CoreVideo Pro for macOS 1.0 — release plan

**Owner request (2026-11-14):** "get this ready to be a solid release for mac."
**Scope ruling (controller, recorded):** the Mac 1.0 release = M0 correctness fixes
(`2026-11-14-mac-parity-m0-fixes.md`, done) + the 1.0 manual-control promise (M1) + the
control-API subset that promise names (M2.1/M2.2) + release engineering (M9.1). Graphics,
Comms, social, Tiles and the multiview inspector are later Mac releases per the roadmap
(`2026-11-14-mac-parity-1.0-roadmap.md`). The 1.0 manual-control promise is in
`docs/reference/first-release-scene-control.md` — read it.

**Evidence:** `docs/superpowers/audits/2026-11-14-mac-parity-studio.md` §1–§6, §11, §18
(Windows file/line references for every task below). Verify every audit claim in code.

## Global Constraints

Same as `2026-11-14-mac-parity-m0-fixes.md` Global Constraints (branch `mac-parity-1.0`,
never push, Windows bit-identical, red first, pixels proven with pixels, port the Windows
rule not a lookalike, harness never touches real user paths — the harness predicate is
`ShellHarness.isActive`). Additionally:

- **Keyboard on macOS uses ⌘ where Windows uses Ctrl** (audit §5 owner note). Space stays
  Space.
- **Design system:** every new control follows `docs/design-handoff-macos.md` and
  `DesignSystem.swift`; `bash scripts/lint-mac-design.sh` green; attach a
  `COREVIDEO_SHELL_SNAPSHOT` PNG for any visible change.
- **No outward actions** beyond Apple notarization of the release candidate: no push, tag,
  GitHub release or upload.

---

### Task 1: Take is a real transition with rollback (audit §3, §4)

Port Windows' Take: `begin-take-transition` first in the same `media-core-sync` batch as
`load-scene-graph` (fields `operationId, revision, mode, durationMs, direction, dipColor`;
`MediaCoreCommandBuilder.BuildSyncCommands` `:40-45`, `StudioViewModel.cs:9062-9083`),
default mode **fade**, 300 ms, Cut/Fade picker persisted like Windows (`_takeTransitionMode`,
normalizer `:4073`). Take bypasses the 120 ms scene debounce (`AppModel.swift:1793-1799`),
has an in-flight guard, and rolls back Program/Preview ids when the core rejects or the
request fails (`TransportCoordinator.TakeAsync`, #286 rollback; Mac has no media-selection
rollback to port unless the Mac bin tracks selection — check). Tests: batch order and
fields; default fade; cut when picked; immediate send (no debounce); rollback restores both
ids and emits the Windows rollback status; a second Take while in flight is refused.
Mutation: send `load-scene-graph` before `begin-take-transition` → order test fails.

### Task 2: Fade to Black + keyboard Take/FTB (audit §2, §5)

FTB shell admission exactly as Windows `StudioViewModel.ProgramBlackout.cs`: one in-flight
guard; refuse offline; refuse if the snapshot's `programBlackout` is not a bool ("This media
core does not support Fade to Black"); send
`{"type":"program-blackout","enabled":<target>,"durationMs":300}`; verify the echo else
"Media core did not acknowledge FTB"; labels from opacity ("Fade to Black" / "Fading to
black" / "BLACK · Restore" / "Restoring picture"); button = black fill, no visible text, red
outline when black. Honour every quoted ruling in §2. Keyboard: Space = Take with Windows
precedence over a focused control (NSEvent local monitor checking first responder; ignore
text entry and any modifier; consumed even when Take unavailable; initial press only),
⌘Return = alternate Take, ⌘⇧B = FTB; Studio tab only. Tests for admission, labels, and the
key-routing decision function. The Metal half (black pixels) is already proven by
`MetalCompositor` FTB tests from M0 Task 1.

### Task 3: RC Studio footer, health line, multiview placeholder (audit §1, §18, §11)

Footer to the Windows RC layout (quoted rulings in §1): Graphics/outputs group left with a
fixed gap, only the space before Take grows, Take + transition choice + black FTB key
anchored far right, compact Record/broadcast/webcam icons, separate health/audio row below.
Health line ports `StalledFeedsReadout` + `StreamDegradationReadout` (plain-net9, pure) —
"stream N ms behind and K Zoom feeds stalled", the on-air 1.5 s rule, never claims a cause.
Multiview placeholder ports `MultiviewEmptyStatePolicy` (guidance only when no tiles AND no
picture AND a generic preparing status; never cover an available picture). Tests: health
readout cases from the Windows tests; placeholder policy cases. Snapshot PNGs of Studio at
two window widths.

### Task 4: Local control API — 1.0 subset (roadmap M2.1)

A Swift-native server (Network.framework) implementing the SAME wire contract as
`CoreVideoPro.Control` (read `native-shell/CoreVideoPro.Control/Http/*`, the action manifest
and `ControlActionRegistry.cs`, and CLAUDE.md "Observing a RUNNING core: GET /snapshot"):
HTTP on 127.0.0.1:8011, same routes and JSON shapes, loopback needs no auth, a LAN bind
refuses to start without `COREVIDEO_CONTROL_TOKEN` (same rule). Actions in this subset:
`transport.take`, `transport.ftb.toggle`, `transport.ftb.set`, `scene.list`, `scene.select`,
plus record/stream start/stop if the manifest has them; unknown/retired IDs rejected exactly
like Windows (`automation.*`). `GET /snapshot` with the Windows envelope (`available`,
`receivedUtc`, `servedUtc`, `ageMs`, `stale` > 2 s, `reasonCode`) and the same redaction
rules (walk JSON values; secret-named suffix list). Actions run through the same AppModel
paths as the UI (Task 1 Take, Task 2 FTB). Off the main thread for I/O, main actor for
model calls. Tests: route/shape tests against fixtures shared with Windows where they exist
(`contracts/`), auth rule, redaction, rejected IDs; a live smoke with `curl` against the
running shell binary in a test mode is acceptable. Do NOT open any non-loopback port by
default.

### Task 5: Companion against the Mac (roadmap M2.2)

Check `companion-module-corevideopro` against the Task 4 server: run its tests/fixtures
against the Mac endpoint (or a scripted client using the module's request shapes) for Take,
FTB and scene select; fix server mismatches (never the module, unless it is Windows-only by
accident — report that instead). Document how to point Companion at a Mac in
`docs/release-guide.md`.

### Task 6: Release packaging — versioned, complete, signed, notarized DMG (roadmap M9.1)

`scripts/make-macos-shell-bundle.sh`: app `CFBundleShortVersionString` from `package.json`
version (currently hard-coded `0.1.0`), `CFBundleVersion` a monotonically increasing build
number (git commit count is acceptable); bundle and sign every helper the app resolves from
Resources (`corevideo-plugin-host` is resolved at `AppModel.swift:3027` but never bundled;
check browser-host and any other `bundleResources + "/…"` lookups); fix `sign_nested` to
sign bare Mach-O executables inside SDK frameworks (required for notarization). New
`scripts/release-macos.sh`: clean build of core (Metal+AVF+CoreAudio+RTMP as CI's mac config
plus whatever the shipped app needs), engine (Zoom SDK path via env), shell; signed bundle;
`.dmg` (hdiutil, app + Applications symlink, volume name "CoreVideo Pro"), signed;
notarize app-zip and DMG with keychain profile `CoreVideo Plugin` (`xcrun notarytool submit
--wait`), staple both; verify `spctl --assess --type execute` → "accepted source=Notarized
Developer ID", `stapler validate`, `codesign --verify --deep --strict`; write
`dist/macos/<version>/` with the DMG, `.sha256`, and a manifest JSON (version, build, git
sha, signing identity, notarization IDs). Run it once for real; attach outputs. If
notarization is rejected, fetch the log (`notarytool log`) and fix the cause.

### Task 7: CI, docs and owner acceptance list

CI (`.github/workflows/ci.yml`): confirm the macOS jobs build and run the new native tests
(AVF, Metal FTB, engine command) — exclude the `realtime` ctest label on shared runners if
they are timing-sensitive, and say so; keep `mac-shell-tests` green. Docs: a macOS section
in `docs/release-guide.md` (install, first-run permissions, Companion setup), CHANGELOG entry
for the macOS 1.0 release, and `docs/qa/macos-1.0-owner-acceptance.md` — the manual checks
nothing here can automate: real Cmd-Q while recording (dialog, buttons in terminate-wait,
double Cmd-Q, main window closed), stream-quit, real camera ISO frame count, Zoom auth +
join under hardened runtime (signed build), TCC prompts appear once and persist across
updates, Companion Take/FTB from a Stream Deck, 1080p60 frame delivery on the owner's Mac
(`scripts/mac-show-drill.py --load 8`, per CLAUDE.md north star), a full show rehearsal.

### Task 8: Top navigation matches Windows 1.0 (owner report 2026-11-14)

Owner: "The menu at the top seems out of date vs the windows port." Windows 1.0
(`StudioWorkspace.xaml` ~141-245): ONE "Produce" group, in order Studio, Zoom (opens the
`settings` page), Sources (`inputs`), Scenes, Routing, Graphics (the `overlays` page,
relabelled), Audio, Comms, Media, Health (direct action — first-release-scene-control.md:
"Zoom and Health are direct top navigation actions"), OHG Show. Mac today: three groups
(Produce / Setup / Diagnose) with Overlays, Diagnose and a separate Settings tab.
Align the Mac nav to Windows: one group, Windows order and labels; "Overlays" → "Graphics"
(same tab content — Mac graphics designer/operate is a later release); "Diagnose" →
"Health"; Comms and OHG Show omitted (not on Mac in 1.0 — no disabled placeholders that
imply otherwise unless Windows shows disabled items); decide Settings placement by reading
how Windows reaches the content of the Mac Settings tab (Windows' Zoom = settings page) —
port the Windows structure if the Mac Zoom/Settings content maps cleanly, otherwise keep
Settings as the trailing item and record why. Keep tab ids/prefs/snapshot-harness tab names
working (`COREVIDEO_SHELL_TAB` aliases for old names). Tests: nav model order/labels;
harness tab aliases. Snapshot PNGs at 1360/1920 compared with Windows.
