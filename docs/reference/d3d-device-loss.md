# D3D device loss is RECOVERED, by generation (beta slice, 2026-09-09)

_Moved verbatim from `CLAUDE.md` (#737). Paths are relative to the repo root._

The shared device (`Direct3D11InteropService.s_sharedDevice`) can die mid-show — a TDR, a
driver upgrade, a hardware fault. It used to die **permanently**: the present threw,
one host dropped to CPU fallback, and nothing ever cleared `s_sharedDevice`, so
`EnsureDevice` kept returning true for a dead device and EVERY surface stayed on CPU
until the app restarted. Nothing in the tree called `GetDeviceRemovedReason`, so the log
never named the cause. On a tester's machine that is a silently degraded show we cannot
diagnose. Now:

- **Classification is pure and tested** (`Services/DeviceLossPolicy.cs`,
  `DeviceLossPolicyTests`): only `DXGI_ERROR_DEVICE_REMOVED`/`DEVICE_RESET` — or a
  negative `GetDeviceRemovedReason` — retire the device. Everything else
  (`WAS_STILL_DRAWING`, occlusion, a resource-pressure create failure, a stale shared
  handle) keeps the existing per-handle invalidation path. `PresentationAttempt` is
  unchanged.
- **Retirement is a GENERATION bump, never ad-hoc field clearing.** `RetireDevice` is a
  no-op for an already-retired generation, so a late callback from the dead device cannot
  resurrect anything. It disposes the whole `HandleIngest` map and drops the device/context
  RCWs — it does NOT dispose them, because other hosts' swap chains still hold native refs.
  Each host rebuilds its swap chain when it adopts the new generation (a CREATE — **still
  never `ResizeBuffers`**), and clears `_invalidHandles`, since a handle blacklisted against
  the dead device is usually fine against the new one.
- **Recovery is automatic and BOUNDED.** No restart needed: the next
  `CompositionTarget.Rendering` tick past the backoff deadline recreates the device and GPU
  presentation resumes. `DeviceLossPolicy.DeviceRecoveryPolicy` is the same shape as
  `ShowEngineRestartPolicy`/`MediaCoreSupervisor`/`BrowserHostRestartPolicy`/
  `PluginHostRespawnPolicy` — 250ms→1s→2s→5s→10s→30s, **give up after 5 consecutive
  failures**, 60s of healthy presenting resets the budget. Recreation never runs inline on
  the failing frame (that frame just drops to CPU), so the UI thread never eats a
  device-create stall on the same tick it already lost.
- **What a tester's log shows** (launch.log, which the support bundle already collects):
  `d3d: DEVICE LOST context=… generation=N->N+1 removedReason=0x887A0006 (DEVICE_HUNG …)
  totalLosses=K`, then `d3d: device recovery attempt K scheduled in Nms`, then either
  `d3d: DEVICE RECOVERED generation=… recreates=… losses=…` or
  `d3d: DEVICE RECOVERY ABANDONED after 5 consecutive failures …`. The per-vsync "no device"
  line is throttled to 5s so it can't roll the diagnosis out of the bundle.
- **Unproven, honestly:** no real TDR was provoked (deliberately). The GPU-side ordering is
  reasoned + reviewed, not executed; only the classify-and-decide half is test-covered.
