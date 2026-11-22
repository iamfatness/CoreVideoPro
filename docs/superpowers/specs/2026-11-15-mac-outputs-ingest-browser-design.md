# macOS outputs, network ingest and browser sources: design

Status: DRAFT design for owner review, 2026-11-15.

Covers consolidated gap rows **2.8** (separate stream and record render sizes and codecs, recording
bitrate and audio kbps), **2.10** (browser overlays / DSK), **2.11** (SRT / RTMP / NDI ingest and
SRT / HLS / NDI output) and **2.13** (Zoom quality caps and the capture audio-sync nudge) of
`docs/superpowers/audits/2026-11-15-mac-ui-parity-consolidated.md`. It also answers whether the
stream backpressure work (`docs/superpowers/specs/2026-09-23-stream-backpressure-design.md`) reaches
the Mac path. The roadmap (`docs/superpowers/plans/2026-11-14-mac-parity-1.0-roadmap.md`) has no
milestone for these rows; they sit under "everything else" after the owner's four priorities.

Path shorthand: `N/` = `native/src/`, `M/` = `mac-shell/Sources/CoreVideoProShell/`,
`W/` = `native-shell/CoreVideoPro.WinUI/`. Every `file:line` below was read in this worktree on
2026-11-15 unless it is marked UNVERIFIED or "audit ref" (taken from an audit and not re-read).

---

## 1. Why

A Mac operator today can record one H.264 file at the canvas size and push one RTMP stream at a
fixed recipe. Everything else an operator expects from the Windows 1.0 output and source set is
absent:

- **One stream, one recipe.** The stream is pinned to H.264, 30 fps, 160 kbps audio, a 2 s
  keyframe interval and two B-frames (`M/AppModel.swift:3517-3527`). Recording quality is the
  literal `"high"` (`M/AppModel.swift:4423`). An operator cannot record 1080p while streaming a
  smaller picture, cannot pick HEVC, and cannot set the recording bitrate or audio bitrate.
- **Streaming needs a tool the product does not ship.** The release core launches an external
  `ffmpeg` that is not in the package (`scripts/release-macos.sh:56-63`). A fresh Mac cannot
  stream until the operator installs Homebrew ffmpeg.
- **No SRT, HLS or NDI.** The footer says so (`M/StudioFooter.swift:552`). Contribution over SRT
  and NDI into a switcher are the two standard ways a production Mac hands off its Program.
- **No network or NDI sources.** Remote cameras over SRT/RTMP and NDI sources cannot be brought in.
- **No browser overlays.** The card reads "Unavailable on macOS" (`M/Overlays.swift:326-330`).
  Web-based scoreboards, timers and graphics tools (the DSK use case) cannot key over Program.
- **No Zoom quality lever and no lip-sync trim.** The big-room bandwidth control and the capture
  card audio offset both exist in the shared core and have no Mac control.

Who notices: any operator moving a show from the Windows build, and any Mac-first operator whose
venue requires SRT or NDI hand-off or who uses a browser graphics tool.

## 2. Headline findings that change the size of this work

These were re-verified in code and several contradict the audits.

1. **A macOS browser host already exists.** `native/browser-host/main-macos.mm` (319 lines,
   commit `9a7d810b`, 2026-08-07) is a WKWebView host that publishes into the same seqlock
   segment as the Windows host, and `native/CMakeLists.txt:492-500` builds it under
   `COREVIDEO_WITH_BROWSER_HOST AND APPLE`. The audits call the host "needed". What is actually
   missing is narrower: it is not built by the release, not bundled
   (`scripts/make-macos-shell-bundle.sh:151-153`), the core cannot find it without an environment
   variable, the POSIX half of its supervisor is a stub (§5.3), and it captures with
   `takeSnapshotWithConfiguration:` whose frame rate is unmeasured.
2. **SRT and HLS output are already compiled into the Mac release core.** They are the RTMP
   FFmpeg sender with a different protocol profile, gated only on `COREVIDEO_WITH_RTMP_OUTPUT`
   (`N/modules/SrtOutputSenderAdapter.cpp:7-18`, `N/modules/RtmpOutputSenderAdapter.cpp:3613-3629`),
   and `ModuleComposition.cpp:495-512` registers them. The gap is the shell UI plus an ffmpeg
   built with libsrt. The `COREVIDEO_WITH_SRT_OUTPUT` option is unrelated (a future libsrt path).
3. **SRT and RTMP ingest already have a POSIX implementation** (`fork` + `execvp`, close-on-exec
   pipes, no shell: `N/modules/SrtIngestCaptureAdapter.cpp:356-386, 1199-1262`). Only the build
   gates are off.
4. **NDI output already has a `dlopen` path** (`N/modules/NdiOutputSenderAdapter.cpp:152-165`,
   candidates at `:192-194`), and the core's entitlements already carry
   `disable-library-validation` for exactly this reason (`scripts/macos-core.entitlements`). NDI
   **receive** is the one adapter that is compiled out on Mac by an explicit `defined(_WIN32)`
   (`N/modules/NdiReceiveCaptureAdapter.cpp:19, 29, 447`).
5. **Backpressure does not reach the Mac.** The policy observes the compressed-bitstream queue,
   which exists only on the GPU-direct path and only inside `#if defined(_WIN32)`
   (`N/modules/RtmpOutputSenderAdapter.cpp:2336-2348`, `#endif` at `:2280`). The Mac has no
   `GpuVideoEncoder` (`MediaFoundationGpuVideoEncoder.cpp:980-984` returns null off Windows), so
   it always takes the raw path: full BGRA frames piped to ffmpeg
   (`RtmpOutputSenderAdapter.cpp:1268`), about 500 MB/s at 1080p60. That is the path the GPU-direct
   work measured at 0.76x realtime on Windows (`N/modules/GpuVideoEncoder.h:17-22`), and it is the
   likely reason the Mac shell pins the stream to 30 fps.
6. **The stream profile is the canvas, in the core.** `MediaCore::startProgramOutput` writes
   `streamOutputProfile.width/height/fps` into `outputWidth_/outputHeight_/outputFps_`
   (`N/core/MediaCore.cpp:2507-2513`), the same fields `set-output-profile` writes (`:1708-1722`)
   and the render plan reads (`:4637-4638`). The sender has no scaler (`N/modules/RtmpFfmpegArgs.h:249`
   feeds ffmpeg the frame's own size). The Mac shell already documents this
   (`M/ProductionSettings.swift:106-114`). So "stream render size" independent of the canvas is
   not a shell-only port. How Windows behaves when its Stream render size differs from its Canvas
   size is UNVERIFIED (it sends both commands; last writer wins in the core).

## 3. Windows behaviour to match

### 3.1 Output profiles and codecs (2.8)

- Production Settings exposes Canvas, and separate **Stream** and **Recording** render profiles:
  size, fps, codec, target bitrate, audio kbps (`W/Views/ProductionSettingsWindow.xaml:172-251`,
  audit refs `:339-384`, `:692-735`). The shell sends them as `streamOutputProfile` and
  `recordingOutputProfile` on `start-program-output` and as `renderProfile` on
  `set-recording-targets` (`native-shell/CoreVideoPro.MediaCore/Services/MediaCoreCommandBuilder.cs:741-745, 799, 843`);
  each profile is `{profileId, resolution, width, height, fps, targetBitrateMbps, audioBitrateKbps, codec}`
  (`:850-860`). Tests: `MediaCoreCommandBuilderTests`.
- Advanced encoder settings per destination: `encoderMode`, `keyframeIntervalSeconds`,
  `rateControl`, `h264Profile`, `bFrames`, `allowEnhancedRtmp`, `audioBitrateKbps`, `videoCodec`
  (parsed and clamped at `N/core/MediaCore.cpp:213-225`).
- **A destination's own codec wins over the stream profile** (#606, `MediaCore.cpp:2533-2541`).
- **Codec policy** (`N/modules/EncoderPolicy.h`): hardware or OS encoders only, no software
  fallback, never a silent substitute. HEVC ships on Windows since 2026-09-20 (`:54-55`); a codec
  with no hardware encoder is refused with a sentence, not downgraded
  (`RtmpOutputSenderAdapter.cpp:1747-1767`, `core/StreamStartAdmission.h`). Tests:
  `GpuVideoEncoderPolicyTest.cpp`, `RtmpCompatibilityTest.cpp`, `RtmpFfmpegArgsTest.cpp`.
- **GPU-direct encode**: the hardware encoder is fed the compositor's GPU texture and ffmpeg is
  only a muxer (`N/modules/GpuVideoEncoder.h:14-28`, spec
  `2026-09-20-gpu-direct-hevc-av1-stream-design.md`). The 2026-09-13 design named in the brief is
  not in this worktree (UNVERIFIED; the interface header is the surviving statement of it).
- **Backpressure** (#597): hold quality, shed whole GOP tails from the destination's own queue,
  request an IDR, never rebuild the encoder in a storm. Pure policy in
  `N/core/StreamBackpressurePolicy.h`; published per sender as `backpressure`.
- **Recording**: `set-recording-targets` carries `quality`, `targetBitrateMbps`,
  `audioBitrateKbps`, `renderProfile` (`MediaCore.cpp:2619-2633`); the request the encoder gets has
  its own width/height/fps/codec/bitrates (`:2803-2809`).

### 3.2 Destinations, supervisor and truthful lifecycle (2.11)

- Destinations: RTMP/RTMPS, SRT (mode, host, port, latency, key length, stream id, passphrase),
  HLS push (HTTP PUT playlist URL, validated by `N/modules/HlsOutputPolicy.h`), NDI. Settings UI
  audit refs: `ProductionSettingsWindow.xaml:433-443, 456-488, 512-536, 547-550`. Wire fields at
  `MediaCoreCommandBuilder.cs:746-760`.
- **Output supervisor** (CLAUDE.md "One destination failing cannot take the show down"):
  `CompositeOutputSender -> SupervisedOutputSender -> AsyncOutputSender -> adapter`
  (`N/modules/ModuleComposition.cpp:61-69`). House ladder 5/10/20/40/60 s, give up after five,
  health = accepted units advancing, terminal result codes bypass the ladder, give-up rewrites the
  record to `failed` / `supervisor-gave-up`. Tests: `OutputDestinationSupervisorTest.cpp`.
- **Truthful lifecycle** (CLAUDE.md "Destination lifecycle is TRUTHFUL"): every sender publishes
  `outputSenders.senders[].lifecycle`; `status` and `destinationHealth` are projections of the
  lifecycle and the supervisor; `lastError` is sticky history and never a health input; a
  destination that never connected is its own failure mode. Tests: `OutputLifecyclePolicyTest.cpp`.
- **NDI is its own output, not a stream destination** (owner ask, design
  `2026-11-01-ndi-independent-output-design.md`).
- **FFmpeg runtime on Windows** is a pinned, SHA-256-verified **LGPL shared** build installed on
  first run (`scripts/alpha/Install-MediaRuntime.ps1:5-6, 28-36, 73`).
- **Network ingest**: `configure-srt-ingest-sources` / `configure-rtmp-ingest-sources`
  (`MediaCore.cpp:1366-1368`), config shapes at `N/modules/Interfaces.h:1207-1224`; NDI receive
  appears as a picker group. Sources page audit refs: `SourcesInputsPage.xaml:159-311`.

### 3.3 Browser overlays (2.10)

- Card: URL + size preset (1920x1080 full canvas, 1280x720, 960x540 widget) + **Add overlay**
  (`W/Views/OverlaysPage.xaml:399-413`, presets `W/ViewModels/StudioViewModel.cs:7223-7228`).
- Wire: `browser-add {url,width,height,fps}` (default 1080p30), `browser-remove`, `browser-reload`
  (`N/rpc/JsonRpcServer.cpp:340-366`, `N/core/MediaCore.cpp:606-630`). A browser source then
  appears as a capture device `browser:<id>` and in the `browserSources.sources[]` snapshot node
  (`MediaCore.cpp:632-645`). Tests: `BrowserSourceTest.cpp` (`MediaCoreBrowserCommands.*`).
- **DSK in / out is a route, not a new layer type.** `BrowserOverlayProgramService.ApplyKeyState`
  appends one full-canvas, top-most capture route for the on-air browser id to the Program routes
  at sync time, leaving authored routes alone (`W/ViewModels/StudioViewModel.cs:9117-9120`,
  toggle at `:14294-14321`). One overlay on air at a time (`:14305-14310`). Test:
  `BrowserOverlayProgramServiceTests` (four cases).
- Host: one process per source, restart ladder 5/10/20/40/60 s, give up after five, a delivered
  frame resets the streak, Reload clears give-up (`N/modules/BrowserHostRestartPolicy.h`). Exit
  codes are wire vocabulary (`BrowserHostExitMessage.h`). The host never outlives the core (stdin
  EOF). Transparency is preserved. The Windows host carries no page audio (no audio code in
  `native/browser-host/main.cpp`).

### 3.4 Zoom quality caps and audio-sync nudge (2.13)

- Zoom quality flyout: preferred camera max resolution (360p / 720p / 1080p tier 0-2) and max
  guest fps (15 / 24 / 25 / 30, else 60), each with a requested-versus-applied status line
  (`W/ViewModels/StudioViewModel.ZoomCameraResolution.cs:13-48`,
  `W/Services/ZoomCameraResolutionPreference.cs`, `ZoomCameraFrameRatePreference.cs`). Sent on
  `zoom-join` as `cameraMaxResolution` / `cameraMaxFps`
  (`native-shell/CoreVideoPro.MediaCore/Services/MediaCoreSupervisor.cs:568-569`).
  **Ruling: the preference is latched by a valid join; an edit during a meeting applies at the
  next join** (`N/modules/ZoomEngineRuntime.cpp:246-265`). The fps cap limits local camera
  playout only, never Zoom transport or screen share (`:1969-1974`). Test:
  `ZoomCameraResolutionPreferenceTests`.
- Per-capture-device audio sync nudge, -20 / +20 ms per press (audit ref
  `SourcesInputsPage.xaml:121-134`), command `set-capture-audio-sync-offset {deviceId, offsetMs}`
  (`N/rpc/JsonRpcServer.cpp:255-261`), clamped to +/-500 ms.

## 4. Mac today

| Area | Exists | Stubbed or missing |
|---|---|---|
| Recording encoder | `N/modules/AVFoundationEncoderAdapter.mm`: opens at `request_.width/height`, honours `targetBitrateMbps` (`:105`) and `audioBitrateKbps` (`:149`), mp4/mov | Codec is hard-coded `AVVideoCodecTypeH264` (`:101`) although `videoCodec` is passed in (`:846`). A Program frame whose size differs from the writer is **cropped/padded top-left**, not scaled (`:197-199`). `quality` is ignored. Program pixels arrive as a CPU BGRA readback (`:709`) |
| Stream encode | Raw BGRA pipe to external ffmpeg, `h264_videotoolbox` (`EncoderPolicy.h:74, 97`), `posix_spawn` with argv (`RtmpOutputSenderAdapter.cpp:1945-2066`) | No `GpuVideoEncoder` on Mac; HEVC absent (`EncoderPolicy.h:53-54`); the encoder-availability probe is skipped (`:604-609` returns true); no backpressure node |
| Destinations | `rtmp`, `srt`, `hls` senders constructed in the release core; `interrupt()` sends SIGTERM (`:1214-1216`); stop path SIGTERM then SIGKILL with `waitpid` (`:3225-3244`) | Shell sends only `rtmp` (`M/AppModel.swift:3533-3535`) and reads only the `rtmp` sender (`:1693`). No job-object equivalent: nothing kills an ffmpeg child if the core is SIGKILLed. `destinationIsolationTraits` names only `rtmp`/`srt`/`ndi` (`N/modules/OutputDestinationSupervisor.h:102-112`); `hls` is "unknown, assumed unprotected" |
| Supervisor and lifecycle | Shared and platform-neutral (`AsyncOutputSender`, `OutputDestinationSupervisor*`, `core/OutputLifecyclePolicy.h` contain no platform conditionals) | Shell shows none of `supervisor`, `lifecycle`, give-up or Recover |
| FFmpeg runtime | Looked up in `COREVIDEO_FFMPEG_BIN_DIR`, `FFMPEG_BIN_DIR`, the core's own directory, PATH, then Homebrew/MacPorts dirs (`RtmpOutputSenderAdapter.cpp:413-454`, `PosixExecutableLocator.h:34-36`) | Not bundled. Ingest reads a **different** variable, `COREVIDEO_FFMPEG_DIR` (`SrtIngestCaptureAdapter.cpp:107`) |
| SRT/RTMP ingest | POSIX code present | `COREVIDEO_WITH_SRT_INGEST` / `_RTMP_INGEST` off in `release-macos.sh:175-185`; no shell UI; POSIX path has no Mac run on record (UNVERIFIED that it works) |
| NDI out | `dlopen` path present | `COREVIDEO_WITH_NDI_OUTPUT` off; candidate list is bare leaf names (`libndi.dylib`), so it depends on the loader's default search path under hardened runtime (UNVERIFIED); in-process, no `interrupt()` |
| NDI in | — | Compiled out on Mac |
| Browser host | `native/browser-host/main-macos.mm`; adapter spawns it with `fork`/`execv` and an argv vector (`N/modules/BrowserSourceHostAdapter.cpp:624-673`); core maps the segment with `shm_open` (`:811-833`); segment names fit the 31-character macOS limit (`BrowserSourceShm.h:39-60`) | See §5.3 |
| Zoom caps | Entire mechanism is in shared `ZoomEngineRuntime` | Shell never sends `cameraMaxResolution`/`cameraMaxFps` (no match in `M/`) |
| Audio-sync nudge | `AvfCaptureDeviceAdapter.mm:184-189, 328, 346` and `CoreAudioCaptureSourceAdapter.cpp:371` apply the offset | No shell control |

## 5. Design

### 5.1 Output profiles and encoders (2.8)

**Model.** Three profiles, as on Windows: Canvas, Stream, Recording. On the Mac for 1.0 the
**Stream size and fps equal the Canvas** (this is what the core does today on every platform, §2
finding 6), and the operator gets Stream **codec, bitrate, audio kbps and the advanced settings**.
Recording gets its **own size, fps, codec, bitrate and audio kbps**. A smaller stream than canvas
is a follow-on that needs a scaled encoder export in the compositor (slice O5); the shell shows
"Stream size follows the canvas" rather than a picker that would silently re-size Program.

**Shell (`M/ProductionSettings.swift`, `M/SettingsPane.swift`).** A `ProductionOutputProfile`
value type mirroring the Windows payload shape exactly, a pure command builder
(`streamOutputProfile()`, new `recordingOutputProfile()`, `streamDestinationSettings()`), and
persistence in `ShellPrefs` (depends on Tier 0.7). Commands used, all existing:
`start-program-output` (`streamOutputProfile`, `recordingOutputProfile`, `destinationSettings`),
`set-recording-targets` (`quality`, `targetBitrateMbps`, `audioBitrateKbps`, `renderProfile`),
`set-output-profile`. The destination's explicit fields keep winning (#606), so the shell writes
the operator's codec, fps and bitrate into both the profile and the destination.

**Recording encoder (`AVFoundationEncoderAdapter.mm`).**
- Honour `videoCodec`: `h264` -> `AVVideoCodecTypeH264`, `h265` -> `AVVideoCodecTypeHEVC`
  (`hvc1` sample entry so QuickTime and editors open it). `av1` is refused at start with a
  `lastError` sentence; it is never written as H.264.
- Scale, never crop: when the Program frame size differs from the writer's, conform with an
  aspect-preserving scale (Accelerate/vImage on the writer thread, or letting the writer input
  scale source-sized pixel buffers). A crop of the top-left corner is the defect class CLAUDE.md
  records for the Windows thumbnail recording; the proof must be pixels.
- Map `quality` to the documented meaning Windows uses, or drop the field from the Mac payload if
  the core ignores it when a bitrate is present (UNVERIFIED which; resolve in the plan by reading
  the MF adapter).

**Stream encoder.**
- `EncoderPolicy.h`: on Apple, `h265` -> `hevc_videotoolbox`; add it to `isSupportedEncoder` and
  `codecHasSupportedHardwareEncoder`. AV1 stays unsupported on Apple and is refused with the
  existing sentence. HEVC over RTMP needs Enhanced RTMP (`allowEnhancedRtmp`); the shell enables
  the HEVC choice only for SRT/HLS or when Enhanced RTMP is on, matching `RtmpCompatibility.h`.
- Replace the unconditional `return true` probe on POSIX with the same one-frame encode probe
  Windows runs (`RtmpOutputSenderAdapter.cpp:570-603`), so a missing `hevc_videotoolbox` in the
  shipped ffmpeg is reported before a stream starts.

**GPU-direct on Mac (the 60 fps path).** New `VideoToolboxGpuVideoEncoder` behind the existing
`GpuVideoEncoder` interface (the header already reserves `iosurfaceId` and names this
implementation, `GpuVideoEncoder.h:26-28, 61`):
- Input: an IOSurface id from a new Metal encoder export. `MetalCompositorAdapter` blits Program
  into a small ring of BGRA IOSurfaces (three, so an asynchronous encode never reads a surface
  being rewritten) and publishes the current one in `ProgramFrame::encoderSharedTexture.iosurfaceId`.
  `chooseStreamEncodePath`'s `frameHasEncoderTexture` test (`RtmpOutputSenderAdapter.cpp:2100`)
  must accept an IOSurface id as well as a handle string.
- Encode: `VTCompressionSession`, real-time, hardware-required, H.264 or HEVC, B-frames off for
  HEVC as on Windows, `requestKeyframe()` implemented. Output converted to Annex B with parameter
  sets on keyframes, delivered through `GpuEncodedChunkSink` with encoder timestamps.
- Threading: the encoder owns its queue; it never touches `coreMutex` or the render thread. The
  blit is one GPU pass on the render thread and must be costed in the drill.
- **Bitstream queue and backpressure.** Lift the bitstream queue, its writer thread and
  `observeStreamBackpressure` out of `#if defined(_WIN32)` so the POSIX path enqueues instead of
  writing to the pipe from the encoder callback (`:2134-2141`, a blocking write on the encoder's
  thread today). The policy, its constants and the published `backpressure` node are unchanged.
  This is what makes #597 reach the Mac.
- The raw BGRA pipe stays as the CPU fallback and its reason is published exactly as on Windows
  (`platform-unsupported` disappears; `encoder-start-failed` and friends remain).

**What the operator sees.** Footer recipe line built from the live profile instead of the literal
string; a refused codec is a sentence in the stream status; `backpressure` shown as "Stream is
shedding frames to hold quality" with the divisor in Health.

### 5.2 Destinations, ingest and the FFmpeg runtime (2.11)

**FFmpeg runtime (owner decision 1).** Recommended: build our own **LGPL** ffmpeg for arm64 in CI
(`--disable-gpl --disable-nonfree`, VideoToolbox, the native AAC encoder, libsrt, TLS through
SecureTransport or OpenSSL 3), sign it with the Developer ID under hardened runtime, ship it in the
pkg next to `corevideo-native` (the core already searches its own directory first after the
environment overrides), and ship the licence, the exact configure line and a source offer under
`notices/ffmpeg/` as the Windows package does. It runs as a separate process, so LGPL linking
obligations do not attach to the core. One environment variable for both directions: ingest gains
`COREVIDEO_FFMPEG_BIN_DIR` and keeps `COREVIDEO_FFMPEG_DIR` as an alias. Health shows the resolved
path, version line and the encoders/protocols probed (`srt`, `hevc_videotoolbox`).

**POSIX child lifetime (new, native).** A shared `PosixChildProcess` helper used by the RTMP/SRT/HLS
sender, the ingest adapter and the browser adapter, giving the three things the Windows job object
gives:
1. No orphans. Each child is started in its own process group and inherits the read end of a
   "parent-alive" pipe whose write end only the core holds. Our own hosts watch that pipe
   themselves (the browser host already exits on stdin EOF). ffmpeg cannot, and a dead core
   cannot kill anything, so ffmpeg is launched through a small new `corevideo-child-guard`
   wrapper that starts ffmpeg, waits on the pipe, and kills the process group at EOF. Proof is a
   test that SIGKILLs the parent and asserts the child is gone.
2. Bounded stop: SIGTERM, grace, SIGKILL, `waitpid`, never under an adapter mutex.
3. Reaping with exit status so exit codes stay wire vocabulary.
`destinationIsolationTraits` gains `hls` (same child, same `interrupt()`), and its comment stops
saying "job object" for a platform that has none.

**SRT and HLS output (shell).** A destinations list replacing the single RTMP form: each row has a
protocol (RTMP/RTMPS, SRT, HLS), its fields (§3.2), and an enable toggle. Secrets (stream key, SRT
passphrase) go to the Keychain, extending `M/StreamKeychain.swift`; they never enter `ShellPrefs`.
The shell sends every enabled destination id in `destinations` with a matching `destinationSettings`
entry. Per-destination status rows read `outputSenders.senders[]`: projected `status`,
`destinationHealth`, `lifecycle.state`, `supervisor.{healthy, gaveUp, consecutiveFailures,
nextAttemptInMs, reason}`, `redactedEndpoint`, `backpressure`. A gave-up destination shows
"Stopped retrying" with a **Retry** button sending `recover-output-sender {destination}`
(`MediaCore.cpp:1314`). Stream start is proven per destination; one failing destination never
marks the others failed. Stop reads "Stopping" until the lifecycle is terminal.

**NDI output (owner decision 2).** Build with `COREVIDEO_WITH_NDI_OUTPUT=ON`; extend the candidate
list with absolute paths (`/usr/local/lib/libndi.dylib`, the NDI SDK and NDI Tools install
locations, and `NDI_RUNTIME_DIR`); a missing runtime is the terminal `runtime-missing` code with a
"Install NDI Tools" sentence. In the shell NDI is its **own toggle** per the 2026-11-01 design,
never under Stream. The published residual risk stands unchanged: NDI is in-process and has no
`interrupt()`. NDI graphics key stays out of scope.

**SRT and RTMP ingest.** Turn the two build options on, run the existing ingest tests on the Mac
(`SrtFfmpegArgsTest`, `SrtIngestHealthPolicy` tests, plus a new loopback integration test using the
bundled ffmpeg as the caller), then add the two Sources catalogs: SRT (mode, host, port, latency,
stream id, passphrase in Keychain) and RTMP listener (URL), each with the runtime health line the
adapter publishes. Sources appear as `capture:` devices, so slot assignment, multiview and ISO work
unchanged. The picker grouping belongs to the Sources rebuild spec (row 2.4); this spec only
guarantees the devices carry a kind the picker can group on.

**NDI receive.** Remove the `defined(_WIN32)` condition by routing the adapter's library loading
through the same loader shim as NDI output. Frame conversion (`NdiPixelConvert.h`,
`NdiReceiveFramePolicy.h`) is portable and already tested.

### 5.3 Browser sources (2.10)

**Process model: keep it.** One `corevideo-browser-host` process per source, spawned by the core's
`BrowserSourceHostAdapter` on its own supervisor thread, exactly as on Windows. Page content never
runs in the core.

**Finish the POSIX supervisor** (`BrowserSourceHostAdapter.cpp`), all of which is Windows-only today:
- Liveness reaping is inside `#ifdef _WIN32` (`:504-531`), so a dead Mac host is never noticed,
  never restarted and stays a zombie. Add `waitpid(WNOHANG)` with the same failure accounting.
- `closeProcessLocked` on POSIX only nulls the fields (`:756-759`): the control pipe is never
  closed and the process never signalled, so **Remove and Reload leak a running host** until the
  core exits. Close the pipe (the host exits on EOF), then SIGTERM/SIGKILL via `PosixChildProcess`.
- An orphan from a spawn race is not killed on POSIX (`:583-591`).
- `resolveDefaultHostPath` returns empty on POSIX (`:62-69`); resolve beside the core executable
  and inside the app bundle.
- The Mac host returns 2/3/4 for usage/segment/URL errors; align them with
  `BrowserHostExitMessage.h` so the operator sentence is right.

**Packaging, signing and privacy.** Ship the host as a helper bundle
(`Contents/Helpers/CoreVideo Browser Host.app`, `LSUIElement`), not a bare executable, because it
needs an `Info.plist`: App Transport Security exceptions for web content and local networking
(graphics tools are commonly plain `http://` on localhost or the LAN), and a local-network usage
string. Signed with the Developer ID, hardened runtime, **no** JIT or library-validation
entitlements (WebKit's JIT lives in Apple's own WebContent process, which is already sandboxed).
No file temporary-exception entitlements. Whether the local-network prompt is attributed to the
shell or the helper, and whether ATS blocks plain http without the plist keys, are UNVERIFIED and
are the first thing the packaging slice measures.

**Frame capture.** The existing `takeSnapshotWithConfiguration:` loop is correct but its rate is
unmeasured. Slice B1 ships it with the achieved rate published; slice B3 replaces it if it cannot
hold 30 fps at 1080p. The candidate is ScreenCaptureKit capture of the host's own off-screen
window (the documented twin of the Windows WGC self-capture), which needs the Screen Recording
grant the product already asks for. Two unknowns decide it and are a spike: whether WebKit keeps
painting a window that is fully off-screen, and whether SCK returns real alpha for it.

**Frame transport: shared memory now, IOSurface only if measured necessary.** Recommendation: keep
the seqlock segment. It is the same ingest seam on both platforms, it already works, the reader
runs off the render thread through `CpuSourcePreparation`, and a 1080p30 BGRA copy is small on
Apple Silicon. A cross-process IOSurface would save one copy and one upload but needs either the
deprecated global-surface flag or a Mach-port hand-off, a second IPC channel and a second failure
mode, for a source that is typically a low-motion graphic. The gate for revisiting is a number:
if browser ingest costs more than 1 ms of render-thread time per tick or the host cannot reach
30 fps because of the copy, move to IOSurface.

**Transparency.** The host draws a transparent web view and writes premultiplied BGRA (the segment
contract, `BrowserSourceShm.h:12`). The Windows host un-premultiplies on one of its recovery paths
(`native/browser-host/main.cpp:321-358`) and Metal's content pipeline blends straight alpha
(`N/compositor/MetalCompositorAdapter.mm:516-519`), so which convention the route path expects for
a capture BGRA frame is UNVERIFIED. It is settled by a pixel test, not by reading: a page with a
50%-alpha white box over a known Program colour must read the mathematically blended value, with
no dark fringe.

**Audio.** Parity is silence in Program: the Windows host carries no page audio. On Mac the host
must additionally guarantee the page never plays through the operator's speakers (require a user
gesture for all media playback). Page audio into the mixer is out of scope (owner decision 3).

**Shell (`M/Overlays.swift`).** Replace the unavailable card with the Windows card: URL, preset,
Add overlay; a list of browser sources with status (running / restarting / gave up with the host's
sentence, measured fps), Reload, Remove, **Preview** and **DSK in / DSK out**. Add a Swift twin of
`BrowserOverlayProgramService.ApplyKeyState` and prove it with the four Windows test cases as
shared fixtures. Because a browser source is created by a one-shot command, the shell re-sends
`browser-add` for every persisted overlay on **every core generation** (CLAUDE.md "a one-shot
command must be re-applied on every core generation") and re-keys by URL when the core assigns a
new id. Control actions `browser.add/reload/remove` join the action manifest under the control
surface spec. The operator's URL is redacted in support bundles the same way Windows redacts it.

### 5.4 Zoom quality caps and audio-sync nudge (2.13)

- **Zoom quality.** Two pickers in the Zoom tab (max camera resolution, max guest fps), persisted
  in `ShellPrefs`, sent on `zoom-join` as `cameraMaxResolution` (0-2) and `cameraMaxFps`. The
  status line is a pure function ported from `ZoomCameraResolutionPreference.Status` /
  `ZoomCameraFrameRatePreference.Status` (requested, applied, in-meeting) and proven against the
  Windows test table. Applied values are read back from the Zoom snapshot node that publishes
  `cameraMaxResolution` / `cameraMaxFps` (`ZoomEngineRuntime.cpp:1424-1425`). Whether the Mac
  engine honours a per-subscription resolution below 1080p is UNVERIFIED and is checked live.
- **Audio-sync nudge.** -20 / +20 ms buttons and the current offset on each capture device row,
  sending `set-capture-audio-sync-offset`; the value is read back from the device's
  `audioSyncOffsetMs` (`MediaCore.cpp:304`), persisted per device id, and re-sent on every core
  generation.

## 6. Slices

Sizes: S <= 1 day, M 2-4 days, L 1-2 weeks, XL 2-4 weeks.

| Id | What lands | Lane | Size | Depends on | Proven by |
|---|---|---|---|---|---|
| Z1 | Zoom quality pickers + status + persistence | shell | S | — | Swift tests on the Windows status table; live join showing applied tier in the snapshot |
| Z2 | Capture audio-sync nudge, persisted, re-sent per core generation | shell | S | — | Swift command test; kill-the-core repro shows the offset restored |
| O1 | Recording profile (size, fps, codec, bitrate, audio kbps) + stream codec/bitrate/audio/advanced settings in the shell, persisted | shell | M | Tier 0.7 | Builder tests mirroring `MediaCoreCommandBuilderTests`; ffprobe of a recorded file |
| O2 | AVF: scale not crop, HEVC recording, honest AV1 refusal | native | M | — | Record 720p from a 1080p canvas: ffprobe size + corner-marker pixel test; HEVC file opens in QuickTime |
| O3 | `hevc_videotoolbox` policy + real encoder probe on POSIX | native | S | F1 | `GpuVideoEncoderPolicyTest` Apple cases; local SRT receive decodes HEVC |
| F1 | LGPL ffmpeg built, signed, bundled, notices; one env var; Health readout | native + release | M | owner decision 1 | Clean-Mac install streams RTMP with no Homebrew; notarization passes; `otool`/configure audit shows no GPL parts |
| P1 | `PosixChildProcess` + child guard; `hls` isolation trait | native | M | — | New tests: parent SIGKILL leaves no child; wedged child is interrupted; `OutputDestinationSupervisorTest` green on Mac |
| D1 | Destinations list (RTMP, SRT, HLS), Keychain secrets, per-destination truthful status, Retry | shell | M | F1, P1 | Local SRT and HLS receivers get decodable media; unplug test shows `interrupted` then recovery; give-up shows Retry |
| O4 | VideoToolbox GPU-direct encoder + Metal encoder export + POSIX bitstream queue with backpressure | native | L | F1, P1 | Drill at 1080p60 with a stream: delivered-frame gate; throttled receiver shows shedding and no encoder rebuild storm |
| N1 | NDI output build + loader paths + independent NDI toggle | native + shell | M | owner decision 2 | NDI Studio Monitor shows Program with audio; missing runtime reads as the terminal sentence |
| I1 | SRT + RTMP ingest: gates on, loopback test, Sources catalogs | native + shell | M | F1, P1 | Loopback integration test; a phone SRT camera reaches a slot, multiview and an ISO |
| N2 | NDI receive on Mac | native + shell | M | N1 | An NDI source reaches a slot; `NdiReceiveCaptureIntegrationTest` on Mac |
| B1 | Browser host: POSIX supervisor finished, helper bundle, signed, found by default; shell card with Add/Reload/Remove and DSK in/out; re-add per core generation | native + shell | L | P1 | Pixel test of a transparent test page over Program; kill-host repro restarts; Remove leaves no process |
| B2 | Preview staging + control actions `browser.*` | shell + control | S | B1, control surface spec | Shared fixtures; contract fixture for the three actions |
| B3 | Capture upgrade (SCK self-capture) if B1 measures under 30 fps | native | M | B1 spike result | Published measured fps >= 30 at 1080p; drill shows no Program delivery regression |
| O5 | Stream size independent of canvas (scaled encoder export) | native + shell | M | O4, owner decision 4 | 720p stream while Program and recording stay 1080p, proven by ffprobe on both |

Order: Z1, Z2, O1, O2 can start immediately and are independent. F1 and P1 unblock everything
networked. B1 is independent of F1.

## 7. Acceptance and test strategy

- **Unit (native, run on the Mac build):** encoder policy on Apple; VideoToolbox encoder start,
  keyframe request and Annex B framing; bitstream queue discard and backpressure on POSIX using the
  existing `setBackpressureObservationForTest` seam; `PosixChildProcess` lifetime tests; browser
  adapter reap/kill tests with a stub host executable (the `corevideo-stubborn-ffmpeg` pattern,
  `native/CMakeLists.txt:871`).
- **Unit (Swift):** command builders for profiles and destinations; status projections for
  senders (give-up, never-connected, stopping); `ApplyKeyState` twin; Zoom status table.
- **Contract fixtures:** the output-profile and destination-settings payloads are compared against
  fixtures generated from the Windows builder so both shells emit the same JSON; `browser.*`
  actions join the control manifest fixtures.
- **Pixel and media proofs:** recording scale proof (corner markers, mean luma); browser alpha
  proof by Metal readback or `COREVIDEO_SHELL_SNAPSHOT`; every stream/ingest claim is proven by
  decoding what a real receiver got (ffprobe + a frame luma check), never by sender counters alone.
  Audio claims are proven by decoding the received audio (tone present, A/V offset within 50 ms).
- **Drills:** `python scripts/mac-show-drill.py --seconds 40 --load 8` before and after O4 and B1,
  with a stream active and with a 1080p browser source on Program. Per-frame delivery is the gate,
  not mean fps. A new drill option is needed to start a stream to a local sink (new).
- **Fault drills:** unreachable host (never connects), receiver killed mid-stream (interrupted then
  recovered), DNS failure (terminal versus retry), core SIGKILL (no surviving ffmpeg or browser
  host), browser host killed five times (gives up loudly, Reload clears).
- **Owner live checks:** YouTube RTMPS at 1080p60 for 30 minutes; SRT to a venue-style receiver;
  NDI into a second machine; a real web graphics tool keyed over a meeting; a capture card
  lip-sync clap test using the nudge; a large meeting with the camera cap at 720p.

## 8. Risks and unknowns

| Risk | Resolved by |
|---|---|
| `takeSnapshot` cannot reach 30 fps; SCK may not paint an off-screen window or return alpha | Half-day spike at the start of B1, measured fps published by the host |
| Alpha convention mismatch (premultiplied segment versus straight-alpha Metal blend) | The B1 pixel test; fix in whichever side is wrong and port the test to D3D11 |
| ATS / local-network privacy behaviour for a helper process | Packaging spike on a clean macOS 15 and 26 machine |
| The raw BGRA stream path cannot hold 1080p60 on the Mac | Measure before O4 with the drill; if it can, O4 drops in priority but backpressure still needs the queue |
| Metal encoder export adds render-thread cost | Drill before/after; the blit is skipped when no destination wants it |
| VideoToolbox HEVC over Enhanced RTMP rejected by an ingest | Local and YouTube test in O3; refuse with a sentence rather than fall back |
| LGPL ffmpeg build misses a capability (libsrt, TLS, HLS PUT) | F1 probes each at build time and fails the release script, not the show |
| NDI runtime not found under hardened runtime | N1 test on a clean machine with NDI Tools installed |
| The POSIX ingest path has never run on a Mac | I1 loopback test is written first |
| Windows behaviour for Stream size != Canvas is unknown | Characterise on Windows before O5; do not port a behaviour nobody has stated |
| Mac Zoom engine may ignore a sub-1080p subscription request | Z1 live check; if it does, file an engine task |

## 9. Owner decisions needed

1. **How ffmpeg reaches a Mac.** Options: (a) bundle our own LGPL build, signed and notarized;
   (b) keep requiring Homebrew; (c) drop ffmpeg for VideoToolbox plus native RTMP/SRT libraries.
   **Recommendation: (a).** It matches the Windows LGPL posture, needs no install step, and keeps
   one hardened sender. (b) leaves streaming broken on a fresh Mac and gives us an unknown, often
   GPL-configured binary. (c) is the cleanest licence position but rewrites the sender, the
   supervisor's assumptions and every protocol test; revisit after 1.0.
2. **NDI on Mac in 1.0.** The roadmap says the NDI output path is Windows-only and scopes out only
   the graphics key. **Recommendation:** ship NDI **output** (small, already coded) and defer NDI
   **receive** behind the SRT/RTMP ingest work unless a customer needs it.
3. **Browser page audio.** **Recommendation:** silent, matching Windows; revisit as a cross-platform
   feature.
4. **Stream size smaller than the canvas.** **Recommendation:** not in the Mac 1.0 parity set;
   ship "Stream size follows the canvas" with an independent recording size, and treat O5 as a
   cross-platform feature once the Windows behaviour is characterised.
5. **Order against the four owner priorities.** **Recommendation:** Z1, Z2, O1, O2 ride along as
   small shell/native work now; F1 + P1 + D1 next (fixes "streaming needs Homebrew"); B1 after the
   graphics milestone, since browser overlays are the interim graphics path for operators.

## 10. Out of scope

- NDI graphics key; Auto director; OHG; EngineHost (settled).
- Moving NDI out of process (PR 20/28 on Windows) and any change to the supervisor ladder or the
  lifecycle vocabulary.
- AV1 encode on Apple Silicon (no hardware encoder exists).
- Streaming platform presets (B6) and the GPU-tier override (B4): shell polish for the settings
  workstream.
- The Sources picker grouping and inspector (row 2.4), production-settings persistence (Tier 0.7),
  the recording folder picker (Tier 0.11) and the control-surface action registry (row 2.7), which
  are owned by their own specs and only consumed here.
- Zoom dedicated ISO feed (M8.4) and virtual camera (M9.2).
