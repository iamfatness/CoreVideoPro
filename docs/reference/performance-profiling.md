# Performance profiling (operator lag/stutter/crash)

_Moved verbatim from `CLAUDE.md` (#737). Paths are relative to the repo root._

The right tools, cheapest first — a full evidence trail lives in
`docs/operator-performance-plan.md` and `docs/present-stutter-fix-spec.md`.

- **`dotnet-trace` — no admin, in-process, USE THIS FIRST for the shell.**
  `dotnet tool install -g dotnet-trace`; `dotnet-trace collect -p <pid>
  --profile dotnet-sampled-thread-time -o x.nettrace`; `dotnet-trace report x.nettrace
  topN -n 20`. (`--profile cpu-sampling` is Linux-collect only — don't use it here.)
- **PresentMon 2.5.1 — needs elevation (UAC), measures on-screen frame delivery.**
  `PresentMon --process_name CoreVideoPro.WinUI.exe --timed 20 --output_file x.csv`.
  Read `MsBetweenDisplayChange` (`NA` = frame never displayed), and
  `MsCPUBusy` vs `MsCPUWait` (busy = compute/UI-thread; wait = GC-suspend/IO).
- **`dotnet-gcdump`** for the managed heap: `dotnet-gcdump collect -p <pid>`, open the
  `.gcdump` in PerfView/VS for the retained graph (the CLI `report` under-accounts).
- **The stutter only reproduces under LOAD** — use the fake engine (below) to synthesize
  participants/sources; a fresh idle StubOnly launch has a cheap apply and won't repro.

**Diagnosing NATIVE crashes (make the next one post-mortemable).** Two things must be in
place or a `corevideo-native.exe` dump is unreadable:
1. **PDBs.** Release now emits symbols (`native/CMakeLists.txt` MSVC `/Zi /DEBUG`), and the
   build/launch scripts stage each `.pdb` beside its binary. Analyze with
   `cdb -y "native\build-dev;srv*https://msdl.microsoft.com/download/symbols"
   -z "%LOCALAPPDATA%\CrashDumps\corevideo-native.exe.<pid>.dmp" -c "!analyze -v; kb; q"`.
   **Caveat: a matching PDB only exists for the CURRENT build** — analyze a dump BEFORE
   rebuilding the core, or the offsets stop resolving (this is exactly what lost the
   2026-07-10 08:13 startup crash).
2. **Full dumps.** Run `scripts/setup-crash-dumps.ps1` once (elevated — writes HKLM WER
   `LocalDumps`) to get `DumpType=2` full-memory dumps instead of the default registers+
   stack minidump. Dumps land in `%LOCALAPPDATA%\CrashDumps`.

**Beta crash pipeline (S1, 2026-07-18, `docs/beta-engineering-spec.md` §S1).** On launch
the shell scans `%LOCALAPPDATA%\CrashDumps` for our exes' dumps newer than the offer-once
watermark (`%LOCALAPPDATA%\CoreVideoPro\crash-watermark.json`) and shows a one-shot
consent InfoBar (never auto-sends). Send = zip (dump + ~2MB log tails + redacted support
bundle + manifest.txt, ~24MB cap) → POST `application/zip` to the telemetry-ingest
worker's `/v1/crashes` with `X-CoreVideo-*` metadata headers; the zip stays under
`support-bundles\` either way. Enabled only when `COREVIDEO_TELEMETRY_ENDPOINT` +
`COREVIDEO_TELEMETRY_API_KEY` are set (empty default = quietly disabled). Pieces:
`CrashDumpScanner` / `CrashReportWatermarkStore` / `CrashReportArchiveBuilder` /
`CrashReportUploader` (MediaCore, unit-tested) + `CrashReportCoordinator` +
`StudioWorkspace.BeginCrashReportScan` (WinUI).

**Beta opt-in telemetry (S3, 2026-07-23, `docs/beta-engineering-spec.md` §S3).**
A Settings-tab toggle (default OFF) sends "is beta healthy" events to the
telemetry-ingest worker's `/v1/events` on app close (session-end, fire-and-forget
at the top of `ShutdownAsync` — never blocks the close) and a daily heartbeat.
Reuses S1's config (`COREVIDEO_TELEMETRY_ENDPOINT`/`COREVIDEO_TELEMETRY_API_KEY`,
empty = disabled) and Bearer/202 contract. Consent lives in a STANDALONE flag file
`telemetry-consent.json` (NOT prefs — avoids the prefs-version race), default OFF.
Payload is COUNTS/KINDS ONLY (`TelemetryPayloadBuilder`, sourced from
`_bridge.LastSnapshot`, never StudioViewModel): version + sessionLengthSeconds +
outputConfigShape {recording/streaming/vcam bools, iso/capture/participant counts} +
crashCountSinceLastSend (from the S1 crash-watermark) + machineClass (the SAME
`win-x64-cpuN-ramNgb` string as S1 via shared `MachineClassProbe`) + banded machine
{cpuCores,ramBand,gpuTier}. **NEVER a secret/endpoint/path/name** — the no-leak unit
test (`Serialize_NeverLeaksAnySecretOrEndpoint`) seeds a snapshot full of stream
keys/URLs/paths and asserts none appear. Settings "Preview what's sent" shows the
exact JSON (works with consent OFF), and every send logs the JSON locally first
(§7 inspectable-before-egress). Pieces: `TelemetryConsentStore`/`MachineClassProbe`/
`TelemetryEvent`/`TelemetryEventClient` (MediaCore) + `TelemetryEventService`/
`GpuTierProbe` (WinUI). Any new telemetry field MUST be a count/kind and get a
no-leak assertion.

**Capture reader stability (the frozen-webcam / restart-storm class).** A stalled
MediaCapture reader (`CaptureDeviceFrameReaderService`) used to restart on a fixed ~5s
cadence forever when it couldn't recover (e.g. an Elgato Game Capture whose HDMI signal
dropped → `reader.StartAsync` returns `OutputFormatNotSupported`; 515 restarts logged in a
day). Now `CaptureReaderStallPolicy` applies exponential backoff (5→10→20→40→60s) and
**gives up after 5 consecutive failed restarts**, leaving the last frame frozen and asking
for a manual reconnect — no perpetual churn (that churn can trip the `CoreMessagingXP`
fail-fast on a long show). The counter resets the instant a real frame lands. Separately,
when native UVC (`COREVIDEO_NATIVE_UVC=1`) claims a device, the shell now stops any managed
bridge reader for that same device so the two never run concurrently.

**Native-UVC no-first-frame watchdog + confirm-before-commit fallback (2026-07-23).**
The native MF adapter's `connect()` flips a device to `connectionState:"connected"` the
instant the reader thread STARTS — before any frame. A single-consumer capture card
(Elgato Game Capture / HD60 S+) that another app (Zoom, Camera Hub, OBS) holds, or an HDMI
input with no signal, OPENS and NEGOTIATES fine, then delivers zero samples: open+negotiate
succeeded so no stall policy fired, and it sat forever on a placeholder tile (the compositor
logging `capture:<id> has NO matching frame` every 5s). Two-part fix, mirroring the
`CaptureReaderStallPolicy` shape: (1) **core watchdog** — `UvcCaptureSession::readLoop`
gives a negotiated device `kUvcNoFirstFrameTimeoutMs` (4s) to produce its first frame; past
that it fails LOUD (`uvcNoFirstFrameWarning` names the device + likely cause), ends the read
loop and RELEASES the MF device (frees the single-consumer card). The check is lock-free off
the `loggedFirstFrame` flag and fires whenever `ReadSample` returns (stream tick / gap — the
shape a no-signal card presents); `frameId>0` is never a stall. The adapter also gained a
real `disconnect()` override (was a no-op) that resets the session. (2) **shell confirm-
before-commit** — `TryConnectNativeUvcCaptureAsync` no longer commits (stops the bridge) on
the connect response alone; it polls `ListNativeCaptureDevicesAsync` for up to
`NativeUvcCapturePolicy.FirstFrameTimeoutMs` (6s > the 4s core watchdog so the core marks it
"error" first) until `signalPresent` (→ commit native) or the device errors/vanishes/times
out (→ `DisconnectNativeCaptureDeviceAsync` + return false → the existing WinUI MediaCapture
bridge fallback, the robust default per this file). Pure decisions: `uvcNoFirstFrameTimedOut`
(core, `UvcCaptureSupportTest`) and `NativeUvcCapturePolicy.EvaluateFirstFrame`/`FindDevice`
(shell, `NativeUvcCapturePolicyTests`). Net: a camera native UVC can't pull frames from now
FAILS LOUD and FALLS BACK, never a silent placeholder forever. Honest caveat: the watchdog
relies on `ReadSample` returning periodically (the common no-signal case); a driver that
blocks `ReadSample` forever with no return can't be interrupted from a synchronous reader
(async callback = the WgcSession free-threaded-callback crash class we forbid). NOT a
regression from the ISO merge (#316/#318/#320) — the reader path was untouched (last change
#275); the failure is device contention/starvation.

**Bridge capture allocation churn (the "video slow down").** The managed MediaCapture
bridge used to allocate a fresh ~8MB BGRA `byte[]` **per frame** in
`CaptureDeviceFrameReaderService.CopyBgraBytes`. Across two 60fps cameras that is ~0.7GB/s
of garbage → the WinUI heap grew to **5.2GB** and a core sat in GC, and the GC pauses stall
the UI thread → the operator preview visibly slows (and eventually OOMs). Fixed with a
per-`CaptureSession` ring of 4 reused buffers (`RentFrameBuffer`) — `OnFrameArrived` is
single-flighted so the ring advances on one thread, and depth 4 exceeds the buffers live at
once (SHM write is synchronous; the preview holds only the latest surface state, flushed to
the UI within ~16ms ≪ the ~66ms 4-frame reuse interval). Result: working set **5210MB →
~350MB flat**, 0 dropped frames. The residual ~1.5 cores of bridge CPU (the per-frame
convert + copy + SHM write) is inherent to the managed path; the full elimination is native
UVC once its display gap is closed.

**What the profiling proved (2026-07-10): the lag is the WinUI shell, not the core/GPU.**
On an RTX 4090 the core renders the 4K program + multiview in ~6.6ms (60fps) and the GPU is
near-idle. `dotnet-trace` under synthetic 4-participant load ranked
`CaptureDeviceFrameReaderService.OnFrameArrived` at **53%** →
`CaptureDeviceSharedMemoryWriter.Write` → `SafeBuffer.WriteSpan` **35.7% exclusive**: the
WinUI MediaCapture bridge copies **every** webcam frame through managed memory (~180MB/s
@1080p) = ~50% CPU + ~50MB/s heap churn → 2–3GB working set → crash → UI-thread starvation.
**Native UVC capture (`COREVIDEO_NATIVE_UVC=1`) eliminates it** — verified working set
~265MB (vs 5210MB on the bridge), CPU near-idle, 0 drops. The old "pink tiles" display gap
is **FIXED** (2026-07-10): it was a frame-key mismatch — the multiview layer looks up a
capture tile by `capture:<shell captureDeviceId>` but native frames were keyed
`capture:<core MF id>`, because the outer `WinUiCaptureDeviceAdapter` overrode only the
1-arg `connect(deviceId)` so the `ICaptureDevice` default dropped the shell's `outputSourceId`
before it reached the UVC adapter. Fixed by forwarding the 2-arg
`connect(deviceId, outputSourceId)`. Enabling native UVC also surfaced (and the full-dump+PDB
tooling pinned) a **separate WGC screen-capture crash** — `WgcSession::onFrame` deref'd a
torn-down D3D `context_` because `stop()` revoked the FrameArrived handler without draining an
in-flight callback on the free-threaded pool thread; fixed with a `frameMutex_` held across
`onFrame` + drained in `stop()` + a `~WgcSession(){ stop(); }`. Native UVC is still opt-in
(default OFF): the WinUI MediaCapture **bridge is the robust default** (the shell owns both
id sides, so it cannot go pink; memory-stable since the buffer-reuse fix). Native UVC is the
faster-but-more-delicate opt-in (two id spaces that must agree). A secondary
snapshot-apply churn fix also shipped in `StudioViewModel.ApplyLiveParticipants`
(order-independent, structural-only signature).

**System-audio citizenship (vcam glitching OTHER apps' audio, 2026-07-11/12).** With the
virtual camera consumed by Zoom, other apps' audio (browser) glitched; OBS's vcam on the
same rig was clean → our serve chain. Fixes shipped: (1) render pacer no longer spins the
last 1.5ms of every frame — high-res waitable timer + 500µs tail (200µs measured 58.7fps;
timer wakes ~300-400µs late); (2) the vcam tap thread runs BELOW_NORMAL (it does the most
bus-hostile work in the app); (3) WASAPI monitor thread uses MMCSS "Pro Audio" instead of
raw TIME_CRITICAL (`avrt.h` must be included AFTER `windows.h`); (4) **GPU BGRA→NV12** in
the tap — two pixel shaders on the tap's own device (R8 luma + R8G8 half-res chroma; BT.601
studio-swing matched to `convertBgraToNv12`), readback 8MB→3MB/frame, scalar convert gone;
(5) the Frame Server DLL reader caps torn retries 8→2 and skips the copy when no new frame
was published. Rules distilled: never spin in hot loops (waitable timer + tiny tail); raw
TIME_CRITICAL is forbidden — MMCSS class it; GPU→CPU readbacks are uncached/WC — minimize
bytes and convert on the GPU first (the OBS lesson: learn from OBS architecture, never copy
its GPL code).

**Loud-failure guardrail (no more silent pink — updated for #535 slice 4a,
2026-09-19).** The compositor (`D3D11CompositorAdapter::warnUnmatchedCaptureLayer`)
still logs — rate-limited 5s/key — whenever a `capture:`/`media:` render-plan
layer resolves to NO matching frame, dumping the layer key AND the available
same-prefix frame keys. What changed is what the layer RENDERS while that is
true: a solid `colorFromParticipantId` slab (the "pink tile") is retired from
every layer-resolution path — the layer now renders the bus-health slate
(`compositor::slateColorFor`: `kWarmingSlateRgba` neutral dark, or
`kFailedSlateRgba` + the source's name if health is `failed`; see "Slice 4a" in
the source-bus section above). The log itself is unchanged: a key mismatch on
either path is still a 10-second diagnosis instead of a multi-session hunt,
firing only during the startup gap before first frames, then silent. Companion
audit: `WgcSession` was the ONLY free-threaded OS callback in the capture
layer — `UvcCaptureSession` owns its pull thread and signal+joins in its
destructor — so the WGC teardown-drain fix closed that crash class everywhere.
