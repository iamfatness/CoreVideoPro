# The crash class you WILL hit: CoreMessagingXP 0xc000027b

_Moved verbatim from `CLAUDE.md` (#737). Paths are relative to the repo root._

`CoreMessagingXP.dll +0x93b66`, exception `0xc000027b`, **no managed stack** — a native
WinUI fail-fast that bypasses managed handlers (so no first-chance logger catches it).
It is a churn/reentrancy fail-fast, NOT (usually) off-thread access (instrumented: the
off-thread guards never fired). Confirmed and suspected triggers:

- **Frame-rate rebuilds of x:Bound collections.** `OnSurfacesChanged` fires ~100/s; its
  coalesce did not cap the *rate*, so `RefreshSurfaceBindings` ran ~98/s rebuilding
  `MultiviewTiles` wholesale. FIXED by throttling to ~12.5/s (`RsbMinIntervalMs`).
- **Per-tile GPU swap chains.** One DXGI swap chain per multiview participant tile,
  created/reloaded on roster/active-speaker churn, fail-fasts. FIXED by the
  **core-composited single-texture multiview** (`docs/gpu-multiview-plan.md`): the core
  renders the whole grid into ONE keyed-mutex shared texture
  (`D3D11CompositorAdapter::renderMultiview`), presented by a single
  `ShowMultiviewHost` surface with XAML overlays (labels/tally/meters/clock) that
  rebuild only on structural change. The old per-tile CPU grid has been deleted.
- **ComboBox ItemsSource churn.** The Sources editors were keyed on participant `Health`
  (toggles constantly), rebuilding 10 source dropdowns per tick → blank flicker + churn.
  FIXED by keying the signature on the participant id-set only.
- **Window resize (mitigated by design, not soak-verified).** A crash reproduced right
  after a programmatic window resize. `Direct3D11InteropService` now creates the swap
  chain at the **source** size and never calls `ResizeBuffers` on panel resize — it only
  re-applies a matrix scale (`ApplyPanelTransform`), so the resize-vs-present race cannot
  occur by construction. No dedicated regression soak has confirmed it closed; treat any
  resize-adjacent fail-fast as this until the alpha soak passes.
- **The post-Exit dispatcher drain on a normal close (T1.7, #457, 2026-09-10).** Here the
  stack is `DispatcherQueue::DeferInvokeCallback` under
  `DispatcherQueueController::ShutdownQueue` under `FrameworkApplication::StartDesktop`, on the
  UI thread, AFTER `shutdown: resources released`. `Application.Current.Exit()` handed the
  process back to XAML. Its shutdown drain then ran a leftover work item against torn-down XAML,
  the item returned a failure HRESULT, and CoreMessaging fail-fasted. The failing item was a
  `DispatcherQueueTimer::TimerCallback` (stowed E_UNEXPECTED) in one dump and a non-managed
  callback (stowed E_ABORT) in the other. It hit 2 of 10 graceful closes, both after long
  in-meeting sessions. Nothing aired, but each one costs a 1.1 GB dump, a WER APPCRASH, and a
  false crash prompt on the next launch. FIXED: after a CLEAN shutdown, `MainWindow.ShutdownAsync`
  calls `ShutdownCompletion.Complete`. It stops the view model's leftover timers, writes the last
  log line, and calls `TerminateProcess` on its own process. It never calls
  `Application.Current.Exit()`. It uses TerminateProcess, not `Environment.Exit`, because
  `ExitProcess` would still run `DLL_PROCESS_DETACH` in Microsoft.UI.Xaml and CoreMessagingXP.
  The logs are synchronous, and no ProcessExit handlers exist. A failed or timed-out cleanup
  keeps the `ApplicationLifecycle.ForceExit` fallback. `PrepareForShutdown` also stops the view
  model's DispatcherQueueTimers (defence in depth). **Rule: never hand a torn-down shell back to
  WinUI's shutdown drain.** Unit tests (`ShutdownCompletionTests`) pin the order and the gate.
  (T1.8 put a close guard in FRONT of this path — see "Engine teardown order": closing while
  recording/streaming asks first and finishes the files before `ShutdownAsync` starts.)
  The proof is a scripted close-cycle loop on the real app: zero new
  `CoreVideoPro.WinUI.exe.*.dmp` and zero Application Error 1000 events.

- **The GC FINALIZER THREAD releasing a XAML object (#513, 2026-09-13) — the first
  member of this family that is ASYNCHRONOUS and TIME-DELAYED, and it is NOT
  reproduced yet.** The app died IDLE, 58 min into a live meeting, 43 min after the
  last operator action, with `launch.log` silent the whole time. Dump
  (`CoreVideoPro.WinUI.exe.19580.dmp`, full memory): crashing thread is the CLR
  **Finalizer** (MTA); stack `GC.RunFinalizers -> WinRT.IObjectReference.Finalize
  -> Microsoft_UI_Xaml!ctl::ComObject<DirectUI::Border>::Release ->
  FailFastWithStowedExceptions`, stowed `0x8000000E` = **E_ILLEGAL_METHOD_CALL**.
  The wrapper was a PLAIN `WinRT.ObjectReference<IUnknownVftbl>` (not
  `ObjectReferenceWithContext`) with `_referenceTrackerPtr` set, so the release
  had no UI context to marshal to; the UI thread was idle in `GetMessage`, so a
  marshaled release would have landed. **What this is NOT:** a finalizer-thread
  release is the ORDINARY path — a forced full GC (`dotnet-gcdump collect -p`)
  on a healthy run finalized ~2,700 wrappers and a couple of Borders with no
  incident, three times (fresh app; after 12 takes; after a record/stop cycle).
  The dead population at the crash (66 Borders, 1,845 wrappers) was the SAME size
  as a healthy run's. So the trigger is a specific object STATE, not volume, and
  it did not reproduce on demand. Our code has no manual CsWinRT marshaling and
  no element-building control touches XAML off-thread (checked). Framework:
  WinAppSDK Runtime 2.4.0 / WinUI 2.3.6, CsWinRT 2.2.0. The four code-behind
  element factories that `Children.Clear()` (`ShowMultiviewHost` overlays,
  `ScenePreviewControl`, `AudioLevelMeter`, `SceneCanvasEditorControl` — which
  hooks 4 handlers and unhooks 0) are the likely POPULATION, not a proven cause;
  pooling them reduces exposure and cannot be claimed to eliminate the crash.
  **Two rules it teaches.** (1) A stability claim is bounded by the window you
  watched: 25 clean minutes of takes/drill/soak said nothing about hour 1, and a
  crash with NO application code on the stack is invisible to every log we write
  — only the dump sees it, so `setup-crash-dumps.ps1` full dumps are not optional
  on a test box. (2) Analyze a WinUI dump BEFORE rebuilding the shell (same PDB
  rule as the core); `!dumpobj` on the finalizer frame's `this` is what
  distinguishes a marshaled release from an unmarshaled one.

Rules of thumb: never replace a bound collection at frame rate (sync in place / diff);
keep one stable swap chain per surface (program, preview, one multiview);
present with **skip-present** (only on a new keyed-mutex frame) — smooth-present crashes
~31s in.
