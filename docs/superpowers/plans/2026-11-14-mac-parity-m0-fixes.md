# Mac parity M0 — correctness fixes in what the Mac ships today

**Roadmap:** `docs/superpowers/plans/2026-11-14-mac-parity-1.0-roadmap.md` §3 M0 (this plan
implements M0.1–M0.11). **Spec/authority:** the Windows behaviour named in each task, plus the
evidence audits in `docs/superpowers/audits/2026-11-14-mac-parity-*.md`. Where an audit and the
code disagree, the code wins — verify before changing anything.

Written to `docs/superpowers/plan-authoring-rules.md`: each task gives the defect, the binding
behaviour, required tests (which MUST fail on the unfixed code first), and mutations to run. No
implementation bodies are supplied.

## Global Constraints

- **Branch:** `mac-parity-1.0` in `.claude/worktrees/mac-parity-1.0`. Commit per task; never
  push; never touch `main`.
- **Windows behaviour is unchanged.** Any edit to shared `native/` code must keep the D3D11 /
  MF / WASAPI paths bit-identical (gate Mac-only behaviour on the Mac adapter or `__APPLE__`).
- **Builds/tests on this Mac (arm64):**
  - Native: `export PATH="$HOME/Library/Python/3.9/bin:$PATH"`; configure once with
    `cmake -S native -B native/build-metal -G Ninja -DCMAKE_BUILD_TYPE=Release -DCOREVIDEO_STUB=OFF -DCOREVIDEO_ENABLE_DEV_ADAPTERS=ON -DCOREVIDEO_WITH_METAL=ON -DCOREVIDEO_WITH_AVF_ENCODER=ON -DCOREVIDEO_WITH_COREAUDIO=ON`,
    build `cmake --build native/build-metal -j 8`, run focused tests with
    `native/build-metal/corevideo-native-tests --gtest_filter=<Suite>.*`, and the full suite
    with `ctest --test-dir native/build-metal -j 6` before reporting DONE.
  - Stub config must still build/test if you touch shared code:
    `cmake -S native -B native/build-stub -G Ninja -DCMAKE_BUILD_TYPE=Release` then build + ctest.
  - Shell: `cd mac-shell && swift build -c release`, then
    `COREVIDEO_SHELL_TESTS=1 mac-shell/.build/release/CoreVideoProShell` (currently 2341
    checks, 0 failed) and `COREVIDEO_SHELL_SELFCHECK=1 mac-shell/.build/release/CoreVideoProShell`.
    Shell tests live in `mac-shell/Sources/CoreVideoProShell/ShellTests.swift` (in-binary
    harness, not XCTest). `scripts/lint-mac-design.sh` must stay green.
  - The vendored gtest has NO `GTEST_SKIP`/`SUCCEED`; skip pattern is `fprintf(stderr, ...)`
    + early return.
- **Red first.** Every behaviour test must be shown failing against the unfixed code (record
  the failing output in the report), then passing.
- **Pixel claims are proven with pixels** (Metal readback via IOSurface, as the existing
  `MetalCompositorTest.cpp` does), never by reading code.
- **Port the Windows rule, not a lookalike.** Quote the Windows ruling/commit and point at the
  Windows test that proves it in your report.

---

### Task 1: Metal draws sourceless solid-fill layers (FTB is black, Tiles background is its colour)

**Defect.** `native/src/compositor/MetalCompositorAdapter.mm` `resolveLayers` has no
`hasFillColor` branch (named in the comment at ~line 820). A sourceless layer falls through to
`ResolvedLayer::color = 0xff444444`. `ProgramBlackout::append`
(`native/src/core/ProgramBlackout.h:28-40`) emits FTB as exactly such a layer
(`hasFillColor=true`, `fillColor="#000000"`, opacity = fade), so Mac FTB fades to dark grey; the
Tiles wall `tiles-bg:` layer renders as a grey slab.

**Behaviour.** Mirror the D3D11 branch (`D3D11CompositorAdapter.cpp` `resolveLayers`,
`else if (layer.plan.hasFillColor)`) and `ProgramFramePreview.cpp`'s equivalent: parse
`fillColor` the same way D3D11 does, draw it as a solid quad at the layer's rect and opacity
(reuse `drawSolidQuad`). Update the KNOWN GAP comment to say it is closed. Also update the
CLAUDE.md sentence "And **Metal has no `hasFillColor` branch**…" (Other gotchas → empty render
plan bullet) to say it was fixed, with this task's test name.

**Tests (MetalCompositorTest.cpp).**
1. Port `D3D11Compositor.FadeToBlackProducesBlackPixelsAndRestoresHeldVideo`
   (`native/tests/D3D11CompositorTest.cpp:715`) to Metal: full-opacity FTB over a bright video
   layer reads back black (every channel ≤ a small epsilon, not ~0x44); at opacity 0.5 it is a
   50% blend toward black; removing it restores the video.
2. A sourceless `hasFillColor` layer with a non-black colour (e.g. `#3366CC`) reads back that
   colour, proving the colour is parsed, not hard-coded black.

**Mutations to run.** (a) Drop the branch → both tests fail with ~0x44 grey. (b) Ignore
`opacity` → the 0.5 case fails. (c) Hard-code black → test 2 fails.

### Task 2: Mac Zoom engine dispatches commands by the parsed `cmd`, never by substring

**Defect.** `native/zoom-engine/engine/main-macos.mm` command loop (~2295-2345): the
`init`/`join`/`leave`/`start_media`/`stop_media` branches use `msg.find(IPC_CMD_X) != npos`,
so any command whose payload contains those words matches the wrong branch. Later branches
already use `ipc_command_is` (`native/zoom-engine/shared/engine-ipc.h:35`). There is no
unknown-command branch, so unhandled commands vanish silently.

**Behaviour.** Every branch matches via `ipc_command_is`. Add a final branch that logs an
unknown command name once per name (bounded; never per line forever). Check whether the
Windows engine (`native/zoom-engine/engine/main.cpp`) has the same substring pattern — if so,
note it in the report but do NOT change Windows in this task.

**Tests.** Extract the dispatch decision into a pure, platform-neutral helper in
`native/zoom-engine/shared/` (e.g. a function mapping a line to a command enum) that
`main-macos.mm` calls, and test it in the native test suite (add the file to the test target
in `native/CMakeLists.txt`). Cases: each real command maps correctly; a `subscribe` line whose
JSON payload contains `"join"`, `"init"`, `"leave"`, `"quit"` inside string values does NOT map
to those commands; an unknown `cmd` maps to Unknown; a line with no `cmd` maps to Unknown.
Confirm `main-macos.mm` still compiles in the Metal config.

**Mutations.** Restore `find()` for `join` → the payload-contains-join case fails.

### Task 3: An output start/stop mid-take must not shrink the armed ISO set (#1188)

**Defect.** `mac-shell/Sources/CoreVideoProShell/AppModel.swift` `resolvedIsoSourceIds()`
(~2004) filters the selection by the CURRENT roster/connected captures, and
`startProgramOutput` (~2036) resends that list on every Stream/Record start or stop. The core
accepts the shorter list mid-take. A transient empty roster (ordinary, per #1183) during a
Stream toggle silently stops every Zoom ISO for the rest of the take.

**Behaviour.** Port Windows' rule from #1188 ("preserve recording take across passive source
loss" `1d6649c8`, "ignore unrelated assignments when retaining ISO intent" `0203e310`, and
#1183 "preserve ISO departure across empty roster" `fc4d254d`): while a recording take is
active, the ISO set sent to the core is the take's armed set — passive roster loss never
removes a source from it; only an explicit operator ISO change does. Between takes, the
present-filter at arm time stays. Read the Windows implementation and tests (search
`native-shell` for the commits above) and port the decision, including what happens to an
explicit operator deselect mid-take.

**Tests (ShellTests.swift).** Arm 2 Zoom ISOs, start recording, empty the roster, toggle Stream
on → the `start-program-output` command still carries both ISO ids. Same with a stop. An
operator deselect mid-take removes exactly that id. After recording stops, a new arm filters
by presence again.

**Mutations.** Revert to the present-filter → first case fails.

### Task 4: Mac ISO writers open at the selected resolution and scale, never crop (#1203)

**Defect.** `native/src/modules/AVFoundationEncoderAdapter.mm`: ISO writers open lazily at the
FIRST frame's dimensions (~466-470) and later frames of a different size are cropped/padded
(~230-256). A guest whose first frame is 640x360 or 720p records every later 1080p frame as
its top-left corner.

**Behaviour.** Port #1203 ("Record Zoom camera ISOs at the selected resolution", `219ca1db`):
read how Windows (MF encoder + core) decides the ISO writer size and how a mismatched frame is
conformed (look for `IsoFrameConform.h` / `D3DIsoFrameConformer.h` and the request field
carrying the selected resolution). The Mac writer opens at that size and SCALES (aspect-fit,
matching Windows' conform policy) frames of any other size. If the shared `IsoFrameConform.h`
already decides geometry portably, reuse it rather than re-deriving.

**Tests.** Native test against the AVF adapter (the existing AVF tests write real MP4s to a
temp dir and read them back): feed a 640x360 first frame then 1920x1080 frames with a known
pattern (e.g. a distinct colour in the bottom-right quadrant) → the MP4's dimensions are the
selected size and the bottom-right quadrant colour is present in decoded output (it is absent
under crop). Use AVAssetReader to decode.

**Mutations.** Restore first-frame sizing → dimension and quadrant assertions fail.

### Task 5: A departed ISO guest's Mac writer finalizes instead of growing silence (#1030/#1183)

**Defect.** The core flags a departed ISO source, but `AVFoundationEncoderAdapter.mm`
(~506-535) never reads the flag; the ISO keeps growing as silence-filled audio with no video
until Stop.

**Behaviour.** Port Windows' departure handling (see `native/src/core/IsoDeparture.h` and how
`MediaFoundationEncoderAdapter.cpp` consumes it): on departure, finalize that ISO writer
independently (the existing independent-finalize path from increment 2), and on a return
within the same take follow Windows' rule exactly (new segment vs resume — read it, don't
guess). Program recording and other ISOs are unaffected.

**Tests.** Native test: two ISOs, mark one departed mid-take, continue feeding the other →
the departed file is finalized (playable, duration ≈ time-to-departure, not take length) while
the other keeps growing. Plus the return case per the Windows rule.

**Mutations.** Ignore the flag → the duration assertion fails.

### Task 6: Mac meters stop looking live when the audio observation is stale (#870)

**Defect.** The core publishes the age of the audio observation; the Mac shell never reads it,
so meters freeze at their last value and look live when the audio worker stalls.

**Behaviour.** Port #870's shell rule (find the Windows consumer of the published age field and
its threshold). Past the threshold, Mac meters show the same stale state Windows shows
(dimmed/zeroed per Windows — match it) and recover when fresh.

**Tests (ShellTests.swift).** Fresh observation → levels shown; age past threshold → stale
state; fresh again → recovers. Use the exact field name and threshold from Windows.

**Mutations.** Ignore the age → stale case fails.

### Task 7: Scene editing never changes what is on air (#1196, #1070)

**Defect** (studio audit §scenes): rename/duplicate silently drops the scene background
(`AppModel.swift` ~1672, on every keystroke); the on-air scene can be deleted and Program
switches without a Take (~1717); with Preview empty, the Scenes tab's edits and Delete act on
the live Program scene though the pane says Program is untouched until Take.

**Behaviour.** Port Windows #1196 ("Fix scene naming and renaming without changing live
content", `cb83cf5a`) and #1070 (scene removal rules): renaming changes only the name and
carries every other field (background included); duplicate copies every field; deleting the
Program scene is refused (match Windows' exact rule and message, including Preview); editing
never targets the Program scene implicitly — follow Windows for what the editor targets when
Preview is empty.

**Tests (ShellTests.swift).** Rename keeps background and layers byte-equal except name;
duplicate equals source except id/name; delete of Program scene refused and no scene command
sent; with empty Preview an edit does not emit a Program scene change.

**Mutations.** Rebuild-from-defaults on rename → background test fails; allow Program delete
→ refusal test fails.

### Task 8: Remove brand-kit controls the core no longer applies (#764)

**Defect.** The Mac still shows logo text, lower-third style and caption style brand-kit
controls that the core stopped applying (#764); they do nothing.

**Behaviour.** Match Windows after #764 exactly: remove (or hide) the same controls Windows
removed, and stop sending the dead fields if Windows stopped sending them. Do not remove
controls Windows kept.

**Tests.** ShellTests: the brand-kit command payload matches Windows' field set. Design lint
green; a `COREVIDEO_SHELL_SNAPSHOT` render of the affected tab (attach the PNG path in the
report).

**Mutations.** n/a (removal); confirm no remaining references with grep.

### Task 9: Quit during a recording finishes the files first (T1.8 parity)

**Defect (unverified).** The Mac has no guard when quitting while recording/streaming; whether
the MP4 survives is unknown.

**Behaviour.** First MEASURE: start a recording (stub or real sources), quit the app, and check
the file (playable? moov present?). Then port Windows T1.8 (CLAUDE.md "Engine teardown order":
closing while recording/streaming asks first and finishes the files before shutdown): a quit
while recording asks for confirmation (custom in-app dialog, design-system styled) and, if
confirmed, stops outputs and waits for the truthful `completed`/`failed` lifecycle (bounded
timeout) before terminating the core.

**Tests.** ShellTests for the decision (recording → asks; idle → quits). Report the measured
before/after file integrity (ffprobe output) — the fix is not done until the after-file plays.

### Task 10: Resolve the three unverified recording rows (#976, #984, #1186)

Read `docs/superpowers/audits/2026-11-14-mac-parity-behaviour.md` UNVERIFIED rows. For each:
measure on this Mac (stub sources or AVF tests), and either fix (red-first test) or close with
evidence in the report. #976: can a stale `failed` from the previous take cancel a new Start
in `RecordingCommandPolicy.swift`? #984: do Mac camera ISOs duplicate/drop frames (measure
frame count vs expected over 10 s)? #1186: does the Mac recorder lose the final picture / A/V
tail on stop (compare last video PTS vs last audio PTS)? Effort cap: if a fix is larger than
M, stop and report it as a new roadmap item instead.

### Task 11: Sign Mac dev bundles with the Developer ID

**Defect.** Ad-hoc-signed dev bundles get a new identity every rebuild, so TCC (camera, mic,
screen) and Keychain prompts recur after every build.

**Behaviour.** `scripts/make-macos-shell-bundle.sh` (and the engine bundle script it calls)
gain an opt-in signing identity: env `COREVIDEO_SIGN_IDENTITY` (default unset = current ad-hoc
behaviour, so CI is unchanged). When set: sign nested code deepest-first (`find -depth`),
hardened runtime + secure timestamp, with entitlements — port the recipe and the
`disable-library-validation` entitlement rationale from
`~/Developer/CoreVideo/scripts/make-macos-bundle.sh` and its entitlements file (read them;
note XML forbids `--` in comments; macOS bash is 3.2 — no empty arrays under `set -u`). Add an
optional `--notarize <keychain-profile>` mirroring the plugin script (CoreVideo Pro's profile:
`CoreVideo Pro`; `CoreVideo Plugin` is the OBS plugin's — owner correction 2026-11-14). Do not notarize in this task.

**Tests.** Build the bundle with `COREVIDEO_SIGN_IDENTITY="Developer ID Application: John
Wallace (42394L3353)"`; `codesign --verify --deep --strict` passes; `codesign -dv` shows the
team ID and `flags=0x10000(runtime)`; the app launches and `COREVIDEO_SHELL_SELFCHECK` passes
from inside the bundle. Without the env var, output is unchanged ad-hoc.
