# Present-Stutter Fix Spec (operator display hitches at 4K)

Status: **in delivery** (2026-07-09). Owner-reported: "frame rate across the app is
struggling to keep up, very laggy" — a stuttery/choppy operator display, present even
with the virtual camera off, on an RTX 4090 at a 4K canvas.

## Symptom, measured (PresentMon 2.5.1 on `CoreVideoPro.WinUI.exe`)

`PresentMon --process_name CoreVideoPro.WinUI.exe --timed 20` captured 1177 frames:

- On-screen frame interval: **median 16.7ms (60fps)** but **~33% of frames hitch to
  33-734ms**, and **14% of presents never display** (`MsBetweenDisplayChange = NA`).
- Interval sequence shows the pattern: `17 17 33 17 17 ... 17 67 200 17 50 ... 183 17 50`
  — smooth for ~10-15 frames, then a 50-200ms hitch, **repeating**. 80 separate hitch
  runs, interspersed (NOT one clustered freeze -> not window occlusion).
- **GPU busy = 1.0ms/frame** (RTX 4090 idle). **`MsInPresentAPI` = 0.03ms** (Present is
  NOT blocking). **`MsCPUBusy` = the whole hitch** (77ms busy for a 77ms gap, 570ms for a
  570ms gap; and 16.6ms/16.7ms even on "normal" frames).

Conclusion: the render (core) and GPU are fine. The **WinUI UI thread is CPU-saturated**
and periodically does heavy synchronous work that blocks `CompositionTarget.Rendering`
(the present driver, `VideoSurfaceHost.OnCompositionRendering` -> `Direct3D11InteropService`
`Present(1)`), producing the hitch. This is a shell-side bug, independent of the core
render, the GPU, the vsync/swap-chain, and the virtual camera.

## Root cause

`MediaCoreBridgeService` polls the core every **250ms** (`_pollTimer`), raising
`SnapshotChanged`. `StudioViewModel.OnSnapshotChanged` marshals it to the UI thread and
runs `ApplySnapshotChanged` (StudioViewModel.cs ~9238) which, **every 250ms, on the UI
thread, unconditionally** does: `_surfaces.OnMediaCoreSnapshot`, `ApplyMeetingFieldsFromSnapshot`,
`Transport.ApplySnapshot`, `ApplyConfiguredOutputReadouts`, **`RefreshAudioParticipantRows`**,
**`HydrateAudioRoutingMatrixFromSnapshot`**, `RefreshVstPluginHostFromSnapshot`,
`RefreshAudioReadoutBindings`, `MaybeLogAudioTelemetry`, **`Settings.RefreshDiagnosticsReadout`**,
and (when joined) **`BuildLiveProductionContext` + `MapSnapshotToStudioPatch` +
`ApplyLiveProductionPatch` + `ApplyGraphicsAndCaptionStateFromSnapshot`**. Several of these
rebuild collections / invalidate many bindings. The hitch cadence in PresentMon matches the
250ms poll exactly: **each snapshot apply stalls the present.**

Most snapshots do NOT change the structural data (roster, routing, scenes, devices, VST
list, output config) — only meters/levels/status tick. But the heavy rebuilds run every
time regardless, so the UI thread pays the worst-case cost 4x/second.

## Fix plan (phased, each independently shippable)

**P0 — Instrument (temporary).** Add per-sub-op stopwatch logging inside
`ApplySnapshotChanged` (behind a debug flag or throttled log) to rank the actual costs, so
the gating targets the real hogs, not guesses. Remove after P1 lands.

**P1 — Signature-gate the heavy, rarely-changing sub-ops.** For each expensive rebuild,
compute a cheap signature of ONLY its inputs from the snapshot and skip the rebuild when
the signature is unchanged since the last apply. Candidates (confirm with P0): audio
participant rows (roster/mix ids), routing-matrix hydration (sends set), VST host list
(already partly gated), diagnostics readout, live-production patch (scene/roster/graphics
ids). Cheap scalar fields (meters, GR, status labels, resolution) stay unconditional — they
are `OnPropertyChanged` scalars, not collection churn. This turns the common-case apply
from tens-of-ms into microseconds and is the primary fix.

**P2 — Move pure computation off the UI thread.** `MapSnapshotToStudioPatch`,
`MaybeLogAudioTelemetry` string building, and `RefreshDiagnosticsReadout` formatting are
pure transforms — compute on the threadpool from the bridge, hand the UI thread only the
minimal already-diffed result to apply. Keeps UI-thread time bounded.

**P3 — Decouple snapshot apply from the present frame (if P1+P2 insufficient).** Cap the
heavy apply to a lower rate (e.g. structural refresh at most every 500ms-1s) OR time-slice
it so a single apply can never exceed a few ms on the UI thread. Meters/status keep the
250ms cadence.

Out of scope: the ~16ms/frame present-path baseline (PumpIngest + CopyResource x N hosts) —
revisit only if P1-P3 don't restore a steady 16.7ms.

## UPDATE 2026-07-10 — the apply cost was only ONE layer; the dominant cause is deeper

Instrumentation (P0) under real load ranked the `ApplySnapshotChanged` hogs precisely:
`applyParticipants` (`ApplyLiveParticipants`) ~5ms and `applyPatch`
(`ApplyLiveProductionPatch`, which calls `ApplyLiveParticipants` AGAIN — it runs 2x/apply)
~4ms; everything else <1ms. **P1 landed:** the participant-set structure signature is now
order-independent (sorted) and reduces `Health` to just the structural `VideoOff` bit, so
active-speaker reordering and `LowResolution`/`Recovering` health flicker no longer force a
full roster/tile rebuild 4x/second. That cut the common-case apply from ~13ms to ~10ms and
made `applyParticipants`/`applyPatch` early-return when the set is unchanged.

**BUT re-measuring with PresentMon showed the stutter was NOT fixed** (avg 39ms->60ms,
max 734ms->1877ms — load also heavier). Root cause is bigger than the apply's own compute:
1. **XAML layout on rebuild.** PresentMon: the big freezes are 400-875ms of UI-thread
   `MsCPUBusy` with `MsCPUWait=0` (compute, NOT GC-suspend, NOT I/O), and they EXCEED the
   `ApplySnapshotChanged` self-time (which maxes ~112ms). The extra 300-700ms is the
   DEFERRED XAML measure/arrange that runs after the apply's binding invalidations when a
   structural rebuild reassigns whole collections (`MultiviewTiles`, RoomVideoParticipants,
   the Sources pickers via `RefreshShowInputEditors`, `RefreshParticipantListItems`,
   `RefreshMultiviewGridTiles`). A single rebuild taking 400-875ms is pathological for ~8
   tiles + a roster -> likely re-laying-out a LARGE/growing visual tree.
2. **Huge managed heap + churn.** gcdump: **1.5 GB live managed heap**; working set swings
   5.7-8.1 GB; the shell grew 1.8->2.8 GB over ~6 min and CRASHED (OOM/churn fail-fast).
   This grows over a session -> layout over a growing tree gets slower -> the freezes
   worsen over time (matches "it's gotten laggier") and end in a crash.

**Next phases (the real fix, needs focused work):**
- **P4 — diff-update instead of collection-reassign.** The structural rebuilds must update
  existing bound items IN PLACE (add/remove only what changed) so the ItemsControls don't
  re-realize + re-layout the whole tree. This is the direct fix for the 400-875ms freezes.
- **P5 — find the 1.5 GB heap / visual-element retention.** Profile with PerfView or VS
  memory tools (retained-size graph). Prime suspects: leaked multiview tile visuals /
  participant items not detached on rebuild; base64 preview / per-frame byte[]; Text.Json
  snapshot deserialization churn (180+ large byte[] in the dump). Fixing the growth stops
  the worsening + the crash.
- **P6 — cut per-snapshot/per-frame allocation** (reuse buffers; throttle the ~2KB audio
  telemetry string built+logged every 250ms -> 19 MB launch.log).

TOOLS THAT WORK HERE (verified): PresentMon 2.5.1 (`--process_name CoreVideoPro.WinUI.exe
--timed N --output_file x.csv`, needs elevation; MsBetweenDisplayChange NA = undisplayed,
MsCPUBusy distinguishes compute vs GC/IO). dotnet-gcdump (`dotnet tool install -g
dotnet-gcdump`; `collect -p <pid> -o x.gcdump`; `report x.gcdump`) — but open the gcdump in
PerfView/VS for the retained-size graph the CLI report doesn't give.

## Verification

Re-run the same PresentMon capture after each phase. Target: **>99% of displayed frames at
16.7ms +/- one refresh, zero >33ms hitches, <1% NA (undisplayed)**, GPU still idle. The
interval sequence should read a flat `17 17 17 ...` with no periodic spikes.
