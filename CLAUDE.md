# CLAUDE.md — working guide for CoreVideo Pro

Operational notes for working in this repo. Product/positioning lives in `README.md`
and `COREVIDEO_PRO_PRODUCT_SPEC.md`; this file is the "how to build, run, and not break
it" guide.

## North star (non-negotiable)

Low-latency **and** high-quality A/V is the entire product. It must beat vMix, Ecamm,
and mimoLive. All Zoom is 1080p and up to 60fps — never downgrade quality to dodge a
performance problem; fix the pipeline. CPU per-pixel work does not scale to 8 Zoom + 2
capture @1080p60 — the compositor path must stay on the GPU.

### Frame-delivery acceptance (user requirement, 2026-09-06)

60 fps is a per-frame output delivery requirement, not an average-FPS target.
The approved Program buffer is selectable at 2 or 3 frames, default 3, applied
after app restart. At 60 Hz this adds 33.333 or 50 ms of buffering, with matching
Program audio delay. A render overrun absorbed by the buffer is diagnostic, not
an output failure. Any underrun or missed scheduled output deadline fails
performance acceptance for the tested configuration. The PR #407
operator soak passed functional checks, but its reported late intervals and
42 ms worst interval do NOT pass this performance requirement. Never waive a
frame-rate failure because the average or median is 60 fps. Preserve resolution,
quality, audio continuity, and enabled outputs while fixing the pipeline.

Measure render cost, GPU readiness by the scheduled presentation deadline, and each enabled output's
delivery separately. CPU submissions or advancing frame counters alone do not
prove presentation; duplicated/padded output must not conceal missed rendering.
Report missing evidence as unverified, and record exact tested hardware, workload,
duration, and failures. A finite soak cannot establish an unlimited guarantee.

### Test interaction preference (user instruction, 2026-09-07)

Use the local control API and headless test processes to manipulate CoreVideo
while the user is using the PC. Do not take over the desktop with Computer Use
unless the user explicitly requests it again. Existing authorization for tests,
spikes, and soaks in the designated test meeting remains in effect.
The owner explicitly authorized closing/restarting CoreVideo and repeating tests
in test meetings without renewed approval (2026-09-22).

## What to work on: `docs/BACKLOG.md`

The single ranked list. Pick the top unblocked item there, not the latest incident; a new
defect found mid-task gets a GitHub issue (label `backlog`) and a row in its tier, and the
owner re-ranks. `docs/beta-plan.md` and `docs/FOCUS_PLAN.md` are superseded for ordering
and status. A #419 architecture foundation lands on `main` only together with its first
real consumer, never as an unwired island.

## What this app is

Three processes, not a web app (plus an optional fourth, the OHG show engine host — see
`docs/reference/ohg-show-engine-host.md`):

- **WinUI 3 (.NET 9) shell** — `native-shell/CoreVideoPro.WinUI/` — the operator console
  (the product). It owns no real-time media; it sends commands and renders shared textures.
- **C++ media core** — `native/` → `corevideo-native.exe` — real-time pixels/PCM:
  D3D11 compositor, audio mixer, recorder, output senders.
- **Zoom engine subprocess** — `native/zoom-engine/` → `corevideo-zoom-engine.exe` —
  speaks the Zoom Meeting SDK, writes raw **I420** frames to shared memory.

IPC: JSON-line commands/snapshots over child stdin/stdout pipes; video as keyed-mutex **DXGI shared
textures** (cross-process) for program/preview, and shared-memory I420 for Zoom frames.

Process boundaries + where spine features (ISO/NDI/SRT/browser) plug in: `docs/architecture-seams.md`.

## Build & run (Windows)

```powershell
npm run app                       # build best-available native core + build/launch WinUI
npm run app -- -StubOnly          # skip the native core rebuild, just (re)launch the shell
npm run app -- -Rebuild           # force a native rebuild
npm run build:native-dev          # build the dev native core (needs ZOOM_SDK_DIR set)
dotnet build native-shell/CoreVideoPro.WinUI/CoreVideoPro.WinUI.csproj -c Release -p:Platform=x64
```

`ZOOM_SDK_DIR` must point at the staged Zoom SDK x64 dir for the full core. `npm run app`
runs `scripts/app.ps1`; the dev launcher is `scripts/run-studio.ps1` (now respects a
pre-set `COREVIDEO_ZOOM_ENGINE_PATH`).

**Run the binary the build just wrote, and ALWAYS pass `--config Release`.**
`native/build-dev/` is a **MULTI-CONFIG** generator (`CMAKE_GENERATOR: Visual
Studio 18 2026`) whose `CMAKE_RUNTIME_OUTPUT_DIRECTORY` is pinned to the binary
dir for EVERY config (`native/CMakeLists.txt:46-48`). So the exes have no
per-config suffix: **Debug and Release write to the exact same path**,
`native/build-dev/corevideo-native.exe`, and `cmake --build native/build-dev
--target …` with no `--config` silently builds DEBUG over your Release core.
This cost a full false regression on 2026-09-12 — a drill reported `coreMutex`
over-budget 1% -> 81% and was reported to the owner as a real regression caused
by the branch. The tell was uniform inflation across trivial stages (emit 32x,
plan 19x) and the binary SIZE: 8,322,560 bytes Debug vs 2,168,832 Release. With
`--config Release` every metric matched baseline and the drill passed. **Check
the size, or `--config`, before believing any native perf number.** (Libraries
DO get a per-config dir — `build-dev/Release/corevideo_native.lib` — so a
`Release/` subdirectory existing proves nothing about the exes.) Separately, a
test run from a STALE `build-dev/Release/*.exe` left by an older layout once
reported a confident "380 tests passed" from a binary a MONTH old, silently
omitting every test file added since. If a newly added test does not appear in
the output, check which binary you ran before suspecting CMake.

Logs: `%LOCALAPPDATA%\CoreVideoPro\launch.log` (WinUI) and `media-core.log` (core).
Support bundle (Diagnostics → "Export support bundle"): writes redacted JSON **and a
zip** to `%LOCALAPPDATA%\CoreVideoPro\support-bundles\` — the zip packs ~2MB
secret-filtered tails of launch/media-core/perf/vcam-serve logs + a CrashDumps
listing + manifest (`SupportBundleArchiveBuilder`/`SupportBundleLogRedactor`,
beta spec S2); missing logs become manifest skip notes, never a failed export.

MSIX signing (beta D2, 2026-07-18): `scripts/sign-native-msix.ps1 -Mode dev|production`.
Dev = self-signed, tolerates missing signtool (exit 0 + LOUD unsigned warning).
Production = Azure Trusted Signing (`COREVIDEO_SIGN_DLIB`+`COREVIDEO_SIGN_METADATA`)
or PFX/thumbprint env, RFC3161 timestamp + `verify /pa` + manifest-Publisher match
all REQUIRED — any gap hard-fails (never a silent unsigned artifact). `-DryRun`
prints the resolved plan; tests: `scripts/tests/test-sign-native-msix.ps1`. Full
env contract in the script header and `docs/beta-engineering-spec.md` §D2.

## Testing multi-participant WITHOUT a real meeting (important)

### Current test-meeting authorization (2026-09-06)

The user explicitly authorized any automatic test, spike, or soak in the current
test meeting, including exercising the HTTP control API (`127.0.0.1:8011`), scene
changes, Take, graphics, automation, and restarting/rejoining for validation.
Do not ask again before running these tests. Validate observed running behavior,
not only unit tests, and verify the executable path actually contains the fix.
This permission applies to this test meeting, not unrelated future live shows.

### Synthetic meeting setup

There is a **synthetic Zoom engine**: `native/zoom-engine/fake/fake-engine.cpp` →
`corevideo-zoom-engine-fake.exe`. It emits N participants + animated I420 + roster/
active-speaker churn over the real IPC. Build with `npm run build:native-dev` (or
directly with `cl.exe` via vcvars64 — the CMake VS-generator build can time out).

To drive the full WinUI app headlessly:
1. Stop any instance; copy the fake exe over **all three** `corevideo-zoom-engine.exe`
   paths (`native/build-dev/`, `native/build-dev/Release/`, and the WinUI `…/publish/`).
   **Back each up as `*.realbak` and RESTORE afterward** — the real engine is ~201728
   bytes, the fake ~87040.
2. `npm run app -- -StubOnly`, then drive Join via UI Automation:
   nav Button Name `"Zoom"` → Edit Name `"Zoom meeting URL or ID"` (ValuePattern.SetValue)
   → Button Name `"Join Zoom"`.
Caveat: fake participants populate the roster but only become live GPU tiles when
assigned to Show Inputs — so per-tile crash repros need that assignment.

## Reference topics — read the one you need, not all of them

Everything that used to live in this file is in `docs/reference/`, one topic per file,
verbatim. Open the file whose trigger matches what you are about to touch. When you learn
something worth keeping, **add it to the topic file (or a new one plus a row here) — do not
grow this file.** `docs/archive/` is history; do not read it unless asked.
Code comments that say "see CLAUDE.md" predate the split: grep `docs/reference/` for the
phrase they quote.

| Read this | Before |
|---|---|
| [`docs/reference/beta-release-runbook.md`](docs/reference/beta-release-runbook.md) | cutting, tagging or publishing a beta |
| [`docs/reference/browser-sources.md`](docs/reference/browser-sources.md) | browser (URL) sources, WebView2 host |
| [`docs/reference/core-on-air-policy.md`](docs/reference/core-on-air-policy.md) | handshake, Engine on, Record or Join gating; a stub adapter left in a real core |
| [`docs/reference/core-restart-drill.md`](docs/reference/core-restart-drill.md) | core crash recovery, respawn, Zoom rejoin |
| [`docs/reference/d3d-device-loss.md`](docs/reference/d3d-device-loss.md) | D3D devices, shared textures, device-removed handling |
| [`docs/reference/destination-lifecycle.md`](docs/reference/destination-lifecycle.md) | destination start/stop state and what the UI may claim |
| [`docs/reference/encoder-capacity-probe.md`](docs/reference/encoder-capacity-probe.md) | encoder session limits, software spill |
| [`docs/reference/engine-teardown-order.md`](docs/reference/engine-teardown-order.md) | stop/teardown/shutdown paths (the ZoomISO deadlock class) |
| [`docs/reference/fault-injection-seams.md`](docs/reference/fault-injection-seams.md) | proving stability work; writing a drill or soak |
| [`docs/reference/frame-alloc-failure.md`](docs/reference/frame-alloc-failure.md) | frame allocation, out-of-memory handling |
| [`docs/reference/gotchas.md`](docs/reference/gotchas.md) | **before any non-trivial change** — the long list of traps, grep it for your area |
| [`docs/reference/gpu-direct-encode.md`](docs/reference/gpu-direct-encode.md) | streaming encode, MF encoders, HEVC/AV1, bitrate/rate control |
| [`docs/reference/installer.md`](docs/reference/installer.md) | touching the installer, MSIX, uninstall or first-run |
| [`docs/reference/iso-recording.md`](docs/reference/iso-recording.md) | ISO recording — video, audio stems, capture sources, dispatch/throttle, pre-flight |
| [`docs/reference/live-meeting-qa-2026-08-09.md`](docs/reference/live-meeting-qa-2026-08-09.md) | shell UI defects seen in real meetings (eight worked examples) |
| [`docs/reference/media-no-decoder.md`](docs/reference/media-no-decoder.md) | media decode failures |
| [`docs/reference/monitor-input-admission.md`](docs/reference/monitor-input-admission.md) | isolated monitor holds, input identity and bounded cache retirement |
| [`docs/reference/observing-a-running-core.md`](docs/reference/observing-a-running-core.md) | reading live state: `GET /snapshot`, the control API |
| [`docs/reference/ohg-show-engine-host.md`](docs/reference/ohg-show-engine-host.md) | the OHG show engine, `show-engine/`, its host process |
| [`docs/reference/operator-stutter-snapshot-apply.md`](docs/reference/operator-stutter-snapshot-apply.md) | shell lag/stutter, snapshot apply cost |
| [`docs/reference/output-supervisor.md`](docs/reference/output-supervisor.md) | stream/record destinations, restarts, the output supervisor |
| [`docs/reference/performance-profiling.md`](docs/reference/performance-profiling.md) | measuring lag/stutter/crash: PresentMon, perf.log, dumps |
| [`docs/reference/persistent-media-source.md`](docs/reference/persistent-media-source.md) | media/clip/still sources, cue/live/pause transport |
| [`docs/reference/recording-output-location.md`](docs/reference/recording-output-location.md) | recording output paths |
| [`docs/reference/secrets-and-oauth.md`](docs/reference/secrets-and-oauth.md) | secrets storage, OAuth return URI |
| [`docs/reference/shared-texture-export-cost.md`](docs/reference/shared-texture-export-cost.md) | creating D3D exports / anything new on the render thread |
| [`docs/reference/source-bus.md`](docs/reference/source-bus.md) | the source bus, ingests, bus health, slates, hold/black |
| [`docs/reference/srt-ingest.md`](docs/reference/srt-ingest.md) | SRT contribution feeds in |
| [`docs/reference/stream-failure-reasons.md`](docs/reference/stream-failure-reasons.md) | stream error surfacing |
| [`docs/reference/studioviewmodel-strangler.md`](docs/reference/studioviewmodel-strangler.md) | adding anything to `StudioViewModel.cs` or extracting from it |
| [`docs/reference/take-tracing.md`](docs/reference/take-tracing.md) | Take, Program/Preview routing, "what did Program render" |
| [`docs/reference/the-law-slots.md`](docs/reference/the-law-slots.md) | slot/participant assignment writes (THE LAW) |
| [`docs/reference/tiles-composed-source.md`](docs/reference/tiles-composed-source.md) | Tiles, composed sources, source erase vs tombstone |
| [`docs/reference/trustworthy-gates.md`](docs/reference/trustworthy-gates.md) | QA gates you rely on: drill latency budget, GPU-encode path evidence, the meter gate |
| [`docs/reference/virtual-camera.md`](docs/reference/virtual-camera.md) | the virtual camera DLL, vcam SHM, Frame Server |
| [`docs/reference/winui-crash-class-0xc000027b.md`](docs/reference/winui-crash-class-0xc000027b.md) | **any WinUI/XAML change** — bound collections, selectors, UI-thread callbacks |
| [`docs/reference/zoom-capture-on-off.md`](docs/reference/zoom-capture-on-off.md) | Zoom raw-media start/stop |
| [`docs/reference/zoom-join-prompts.md`](docs/reference/zoom-join-prompts.md) | Zoom join flow and SDK prompts |
| [`docs/reference/history-current-state-2026-07.md`](docs/reference/history-current-state-2026-07.md) | history only: the July 2026 state notes (audio war, zero-audio recording bug) |
