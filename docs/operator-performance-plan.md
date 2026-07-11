# Operator Performance Plan — professional-grade, no dropped/black frames

Status: **in delivery** (2026-07-10). Mandate (owner): CoreVideo Pro competes with vMix /
Ecamm / mimoLive; a live show cannot have lagging or black frames — that costs the producer
money. The operator display must hold a rock-steady frame rate at a 4K canvas, use all cores
and the GPU correctly, and never grow-until-crash. This plan supersedes the tactical fixes in
`present-stutter-fix-spec.md` (which stays as the diagnosis record) and drives execution.

## What is proven (evidence, not guesses)

Hardware: RTX 4090. Core: renders 4K program + 8-tile multiview in ~6.6ms = 60fps; GPU
~1ms/frame (idle). Owner is LOCAL (Parsec idle, DisplayLink ruled out). The 4K canvas is
INTENTIONAL (4K record/stream). PresentMon 2.5.1 on `CoreVideoPro.WinUI.exe` under load:
on-screen interval median 16.7ms but **33-44% of frames hitch 33-1877ms, ~14% never display**;
the big freezes are **UI-thread COMPUTE** (`MsCPUBusy` = full freeze, `MsCPUWait` = 0 — not
GC-suspend, not I/O), and they EXCEED `ApplySnapshotChanged` self-time -> the extra 300-700ms
is **deferred XAML layout** from bound-collection REASSIGNMENT. gcdump: **1.5 GB live managed
heap**, working set 5.7-8.1 GB, grows 1.8->3.7 GB and CRASHES (CoreMessagingXP 0xc000027b
churn fail-fast / OOM). Conclusion: the bottleneck is the **WinUI shell** — the core, GPU,
vsync and virtual camera are healthy.

Root mechanism: several bound collections are `IReadOnlyList { get; private set; }` that get
**reassigned to a brand-new list** on refresh, so their `ItemsControl`/`ItemsRepeater`
re-realize + re-lay-out EVERY item. Worst offender: `RefreshMultiviewGridTiles` reassigns
`MultiviewGridTiles` (bound to the multiview `ItemsRepeater`) and is called from ~20 sites
incl. the **active-speaker churn path ~1.3x/second** — so the whole overlay re-lays-out
~1.3x/s under a live meeting. `RoomVideoParticipants`, `ParticipantListItems`, `MultiviewTiles`
are the same anti-pattern. (`ShowInputEditors`, `AudioParticipantRows` are already
`ObservableCollection` — verify they diff, not Clear+re-Add.)

## P4 — Eliminate layout-on-rebuild (THE freeze fix) [FIRST]

Principle: never replace a bound collection wholesale; mutate it in place with a KEYED DIFF so
the ItemsControl touches only changed items, and update existing item view-models in place so
their own `INotifyPropertyChanged` refreshes visuals without a re-realize.

- Convert the reassigned lists to STABLE `ObservableCollection<T>` (created once): 
  `MultiviewGridTiles`, `MultiviewTiles`, `ParticipantListItems`, `RoomVideoParticipants`.
- Add a reusable keyed-diff helper `SyncObservableCollection(target, desired, keySelector,
  updateInPlace)` (add/remove/move by key; call `updateInPlace(existing, incoming)` for
  survivors). One well-tested helper, used by every refresh.
- Item VMs (`ParticipantSurfaceTile`, `ParticipantListItem`, `Participant`) must expose their
  mutable display fields (health/tally/label/active-speaker/surface) as observable properties
  updated in place, so an active-speaker/health change is a property poke, not a tile rebuild.
- Verify each is currently reassigned from an off-UI thread guard already present
  (`RefreshMultiviewGridTiles` marshals) — keep that; the diff still runs on the UI thread but
  is O(changed), not O(all).
- ACCEPTANCE: with a live/synthetic 8-participant meeting churning active speaker, PresentMon
  shows NO >33ms hitch attributable to a tile refresh; a genuine join/leave costs one small
  incremental layout, not a 400-875ms full re-realize.

## P5 — Get heavy work off the UI thread (multicore)

The UI thread must do ONLY: apply already-diffed minimal updates + present. Everything else
moves to the thread pool.

- Snapshot pipeline: `MediaCoreBridgeService` runs `MapSnapshotToStudioPatch`, participant
  mapping, telemetry/diagnostics string building, and signature computation on a POOL thread
  BEFORE raising `SnapshotChanged`; the UI thread receives a small immutable "view patch" and
  applies it (the P4 diff). Removes the ~10ms/250ms UI-thread apply + its allocations.
- Keep the strict rule (CLAUDE.md): bound-property sets and `ItemsControl` mutation stay on the
  UI thread (off-thread = 0xc000027b). So: compute off-thread, mutate on-thread, minimally.
- The core already parallelizes (render thread, 50Hz audio/output worker, ingest thread,
  per-source). Audit that no shell path serializes independent work; the vcam readback (see
  [[virtual-camera]]) already uses a dedicated device+thread — same discipline everywhere.

## P6 — GPU where it counts / never a black or CPU frame

- Video: the program/preview/multiview present via GPU keyed-mutex shared textures
  (`Direct3D11InteropService`, PresentMode "Composed: Flip"). ASSERT the CPU/base64 fallback
  (`PresentationPath.CpuFallback`) is NEVER silently active in a show (it allocates + CPU-
  converts per frame = black/laggy risk); add a visible telemetry/warning if it engages, and
  a fast recovery. Skip-present already avoids re-presenting stale frames — keep it.
- Meters / LED ladders / DSP curves: audit whether the audio console's per-frame visuals are
  XAML `Shape`/`Polyline` (CPU-rasterized by DWM each change) at high rate. If so, throttle to
  their ballistics timer (already ~33ms for meters) and/or move to `CompositionAPI`/Win2D so
  the GPU draws them. They must not force UI-thread layout at 60fps.
- Multiview is already core-GPU-composited (one shared texture) — good; keep XAML overlays
  rebuild-only-on-structural-change (P4 makes that incremental).

## P7 — Stop the memory growth + crash

- Profile the 1.5 GB heap with PerfView (retained-size graph) — CLI gcdump under-accounts.
  Prime suspect: tile/participant item VMs + their visuals RETAINED because the old reassigned
  lists (and their realized containers) aren't released; P4's in-place diff should drop this
  sharply (fewer objects churned + realized). Re-measure heap over a 30-min soak after P4.
- Cut per-snapshot allocation: reuse buffers; throttle the ~2KB audio-telemetry string
  built+logged every 250ms (19 MB `launch.log`) to ~1-2s or a ring.
- Target: flat working set over a 30-min live soak; zero fail-fast crashes.

## Verification (every phase) & professional bar

Re-run PresentMon under a live/synthetic 8-participant meeting with active-speaker churn:
- **>99% displayed frames at 16.7ms +/- one refresh; ZERO frames >33ms; ZERO undisplayed (NA).**
- GPU stays the limiter (idle here), UI thread well under one frame of CPU per present.
- 30-min soak: flat memory, no crash, no black frame, no CPU-fallback engagement.
Use the synthetic engine (`corevideo-zoom-engine-fake`, tone/churn modes per CLAUDE.md) to
reproduce load deterministically without a real meeting, so we can iterate PresentMon fast.

## RESULT 2026-07-10 — ROOT CAUSE FOUND + FIXED (native UVC capture)

`dotnet-trace` (in-process EventPipe, NO admin — use this, not PresentMon-only) with the
`dotnet-sampled-thread-time` profile, under synthetic 4-participant load
(`corevideo-zoom-engine-fake` swapped over the 3 `corevideo-zoom-engine.exe` paths), ranked
the shell's wall-clock: **`CaptureDeviceFrameReaderService.OnFrameArrived` 53% inclusive ->
`CaptureDeviceSharedMemoryWriter.Write` 35.9% -> `SafeBuffer.WriteSpan` 35.7% EXCLUSIVE.**
The WinUI MediaCapture bridge was reading every webcam frame in MANAGED code and copying it
byte-by-byte into shared memory (~180MB/s for 1080p cams) — ~50% of wall-clock, ~50MB/s heap
churn, grew the shell to 2-3GB and CRASHED, and starved the UI thread = the operator stutter.
This was the DOMINANT cost, NOT the XAML layout / snapshot handler I first chased.

**FIX (shipped, uncommitted):** made `COREVIDEO_NATIVE_UVC` **default ON** (opt-OUT) in
`NativeUvcCapturePolicy.IsEnabled` — the core's Media Foundation UVC adapter captures cameras
natively; the shell self-falls-back per device (keeps the bridge only if the core doesn't
confirm the camera connected), so defaulting on is a pure win. VERIFIED on a fresh launch with
NO env var: trace top methods are all idle waits (OnFrameArrived/SafeBuffer.WriteSpan GONE);
**working set 267MB, FLAT** (was 1.8-3.7GB churning); render 59fps; app "FRAME DROPS 0 (0.0%)";
screen + Zoom + multiview all render. Tests: NativeUvcCapturePolicyTests 22/22 (updated for the
new default). This likely ALSO fixes the earlier `OutputFormatNotSupported` frozen-webcam
(native MF capture negotiates formats the WinRT bridge couldn't).

Remaining (lower priority now that the CPU/memory root is fixed; verify with owner's REAL
cameras + PresentMon under load): P4 XAML diff-updates (the periodic 400-875ms freezes may now
be gone since the UI thread has CPU headroom — RE-MEASURE before doing P4), P6 audio-telemetry
allocation throttle (still ~2KB string/250ms -> 19MB launch.log), P6 CPU-fallback guard.

## Execution order

P4 (biggest freeze, highest confidence) -> P5 (removes the recurring UI-thread apply) ->
P7 (memory, largely a P4 side-effect) -> P6 (harden GPU path + meters). Each ships behind its
own verification; nothing merges without a clean PresentMon pass.
