# macOS outputs, network ingest and browser sources, Phase 4: outcomes

Branch `mac-outputs-phase4` (worktree `.claude/worktrees/mac-outputs-phase4`), cut from `origin/main`
`3529d568`. Plan: `2026-11-22-mac-outputs-ingest-browser-phase4.md` (copied onto this branch).

Owner decisions in force for this lane (coordinator, 2026-11-22): no 60 fps picker until GPU encoding
lands (O4/Task 7); **NDI: no bundling** (OD-1 = No); the runtime is found at run time from the operator's
NDI install. **A licence read is still owed** (OD-1/OD-2) before a Mac release turns NDI on. Every other
owner decision takes the plan's recommendation.

## Task 0: boundary spikes

Machine: MacBook Pro, Apple M3 Max, macOS 27.2.1 (26B131). Toolchain: Command Line Tools, SDK
`MacOSX26.5.sdk`. All spike code lives in the session scratchpad (`p4spike/`) and is not committed.

### Encode

| Row | Question | Answer | Command / evidence |
|---|---|---|---|
| E-1 | Does `IOSurfaceLookup(id)` find a surface created **without** `kIOSurfaceIsGlobal` in the same process? | **Yes.** The lookup succeeds for a non-global surface. A byte written through the original surface reads back through the looked-up one. `IOSurfaceIncrementUseCount` on the looked-up ref makes `IOSurfaceIsInUse` true on the original. **Task 3's ring does not need the deprecated global flag.** | `clang++ -fobjc-arc e12.mm` / `e1b.mm` (IOSurfaceCreate 64x64 and 1920x1080 BGRA, then IOSurfaceLookup(IOSurfaceGetID)) |
| E-2 | Does `kVTCompressionPropertyKey_ConstantBitRate` succeed for hardware H.264 and HEVC at 1920x1080@60? | **Yes, for both.** With `RequireHardwareAcceleratedVideoEncoder` set, `VTCompressionSessionCreate` returns 0. `RealTime`, `ExpectedFrameRate=60` and `ConstantBitRate=6000000` each return 0, and `PrepareToEncodeFrames` returns 0. This holds with CBR alone and with `AverageBitRate` also set. **Task 2 takes the CBR branch.** | `e12.mm` |
| E-3 | Does the bundled ffmpeg list `hevc_videotoolbox`, and does a 30-frame lavfi encode exit 0? | **Yes.** The cache key `a7ff16a2b56f5790` (the current lock + script) lists `h264_videotoolbox`, `hevc_videotoolbox` and `prores_videotoolbox`. Both 30-frame encodes exit 0. | `ffmpeg -hide_banner -loglevel error -nostdin -f lavfi -i color=c=black:s=1280x720:r=30 -frames:v 30 -an -c:v {h264,hevc}_videotoolbox -f null -` |

### Browser

These rows used a scratch copy of today's `native/browser-host/main-macos.mm` with spike instrumentation.
The page under test runs `requestAnimationFrame` and writes `n mod 256` into the red channel of a
full-canvas fill. The host reads that value from the centre pixel of every published snapshot and counts
how often it changes per second. **That count is the page's real paint rate as Program would see it.** It
is a different number from the host's `[browser-host] N fps` line, which counts snapshots. Window size
640x360, capture timer 60 Hz.

| Row | Question | Answer |
|---|---|---|
| B-1 | What fps does a rAF page reach off-screen at (-32000,-32000), compared with on-screen? | **Off-screen, the page never paints.** The host reports **40-57 "fps published"**, but every frame is fully transparent (`centre BGRA=0,0,0,0`), with 0.0-0.2 page changes per second over 20 s. **On-screen, the page paints at 60.0 changes/s** while 60.0 fps are published. So WebKit throttles the hidden page, and today's "N fps published" line hides it: the host publishes a stream of blank frames and calls it healthy. **OD-3 applies.** The public API `WKPreferences.inactiveSchedulingPolicy = WKInactiveSchedulingPolicyNone` (macOS 14+) **does not fix it**: the page still never paints, and the published rate drops to 0.3 fps. The SPI `-[WKWebView _setWindowOcclusionDetectionEnabled:NO]` (the selector responds) **fixes it completely**: off-screen it reaches 60.0 page changes/s and 60.0 fps published. Following the plan's OD-3 recommendation, Task 12 applies the SPI inside the one function `keepPaintingWhileOffscreen`, with a test. |
| B-2 | Does a plain `http://<LAN>` page load from the bare executable, and from a bundle with the ATS keys? | **LAN `http://` loads in all three variants.** These were the bare executable, an ad-hoc bundle with `NSAllowsArbitraryLoadsInWebContent`+`NSAllowsLocalNetworking`, and an ad-hoc bundle with **no** ATS keys. Two LAN pages were tried: `http://10.0.0.3:8765/` (this Mac, `python3 -m http.server --bind 10.0.0.3`) and `http://10.0.0.1/` (the LAN router's page). ATS exempts local addresses. **Public `http://` is where ATS bites.** `http://example.com/` loads in the bare executable and in the bundle with the ATS keys. In the bundle **without** the ATS keys it fails with "The resource could not be loaded because the App Transport Security policy requires the use of a secure connection." So the Task 14 helper bundle needs `NSAllowsArbitraryLoadsInWebContent` for any `http://` that is not on the LAN. Mutation M14a's plist check is the right guard: a LAN-only check would stay green. |
| B-3 | Is a Local Network prompt shown, and which app does it name? | **Unverified.** The LAN loads above succeeded on the first try with no visible failure. These were child processes of a terminal session, though, and macOS attributes local-network access to the *responsible* process, which here is the terminal. A clean user account was not available. This stays owed to the Task 14 Step 4 signed run on a clean account (owner). |
| B-4 | Does `loadRequest:` with a `file://` URL render, or only `loadFileURL:allowingReadAccessToURL:`? | **Both render** in an unsandboxed host: a red page, and a page whose sibling stylesheet turns it green. So **mutation M12b (`loadRequest:` for `file://`) is expected to stay GREEN**, because the B-4 premise it relied on does not hold here. Task 12 keeps `loadFileURL:` anyway, since it is the documented API for file access. The task's report will record M12b as not discriminating, not as a passing mutation. |

### NDI (owner hardware)

| Row | Answer |
|---|---|
| N-1 | **No NDI install on this Mac.** `mdfind -name libndi` finds nothing. A filesystem search for `libndi*.dylib` outside /System finds nothing. Neither `/Library/NDI SDK for Apple` nor `/usr/local/lib/libndi*` exists. `NDI_RUNTIME_DIR*` is unset in `launchctl getenv` and in the shell. **Task 8 therefore uses the plan's expected candidates** (`/usr/local/lib/libndi.dylib`, `/Library/NDI SDK for Apple/lib/macOS/libndi.dylib`) plus the environment variables. It stays UNVERIFIED until the owner installs NDI Tools and runs `mdfind -name libndi`. |
| N-2 | **Unverified (no runtime).** The `dlopen` of an NDI runtime from a hardened, Developer-ID binary with `disable-library-validation` is owed to the owner run (Task 8 Step 3). |

### Ingest

| Row | Answer |
|---|---|
| I-1 | **Unverified.** The application firewall on this Mac is **disabled** (`socketfilterfw --getglobalstate`: State = 0), and enabling it needs sudo. "Automatically allow downloaded signed software" is ENABLED, so a Developer ID-signed bundled ffmpeg would *probably* be allowed without a prompt. That is an expectation, not a measurement, and it is owed to the owner live check in Task 11 Step 3. |

### Plan consequences

- Task 2: CBR branch (`ConstantBitRate`), per E-2.
- Task 3: non-global IOSurfaces, per E-1.
- Task 12: `keepPaintingWhileOffscreen` uses the occlusion SPI (OD-3 recommendation taken). Without it the
  shipped host publishes blank frames off-screen. **That is a live defect in today's host, not only a
  throughput issue.** The `[browser-host] N fps` line should also stop calling transparent snapshots
  published frames; the Task 12 report addresses this.
- Task 12 M12b: expected to stay green (B-4).
- Task 14: the ATS key is required for public `http://`, not for LAN (B-2).
- Tasks 8, 9 Step 3, 11 Step 3, 14 Step 4: owner-hardware rows remain open (N-1, N-2, B-3, I-1).

## Baseline (before any task)

`./native/build-metal/corevideo-native-tests` (CI `native-metal-macos` flags, fresh `native/build-metal`,
CLT toolchain + MacOSX26.5.sdk): **2239 passed, 2 failed** of 2241. The two failures were
`AvfProgramTail.SubThresholdVideoSkewIsStillReconciled` and `…AudioStalledBeforeStopIsPaddedWithSilenceNotCut`.
In both, the writer produced an empty file (`n=0`) while other lanes were building. Rerun alone, the suite
gave `AvfProgramTail.*` **7/7 passed**. Recorded as load flakes, not caused by this lane.

## Task 8 (N1a): NDI output in the Mac core, Apple runtime discovery — `eb712af7`

- **Tests.**
  - New: `NdiRuntimeCandidates.*` (5). These are the plan's two cases plus a V6/V5/base order case, an
    unset-or-empty case pinning the whole Apple list, and the header sentence. The sender case drives
    the real adapter through `createNdiOutputSenderWithCandidatesForTest({})`.
  - Also new: `OutputSenderAdapter.DefaultSenderWarnsForANetworkDestinationNoBuildHas` (review finding,
    below).
  - `Ndi*` on Mac: 22/22.
  - Full suite with `-DCOREVIDEO_WITH_NDI_OUTPUT=ON` added to `native/build-metal`: **2246 passed, 0
    failed**. This was the run before the review fix-ups. After the fix-ups, the touched tests were
    rerun and pass.
- **Mutations.**
  - **M8a** (drop the three environment entries) turned `AppleOrderAndEnvironment` and
    `AppleEnvironmentVariablesComeFirstInVersionOrder` red.
  - **M8b** (Windows sentence in the adapter on Apple) turned `MissingRuntimeSentenceNamesNdiTools`
    red.
  - Each was applied alone, rebuilt, and restored with `touch`.
- **One fixture error of mine was fixed:** the sender case first pinned `runtimeDetail` exactly. The
  snapshot appends live counters (`connections=… program=… groups=… instance=… video=…`), so it now
  pins the probe's words as a prefix.
- **Live (no runtime on this Mac):** the built core reports
  `capabilityStates["ndi-output"] = {state: omitted, detail: ndi-runtime-missing, failureScope: process}`.
  The `available` half of Step 3 needs an NDI install (owner hardware, row N-1/N-2).
- **Review** (independent agent, given the diff and the task's mutation list):
  1. *Important:* turning NDI on in the Mac CI job left no CI configuration with RTMP on and NDI off. As
     a result `OutputSenderAdapter.DefaultSenderWarnsWhenRequestedDestinationHasNoModule` compiled to
     `EXPECT_TRUE(true)` everywhere CI runs. Fixed by a flag-independent twin that drives the same
     composite missing-module branch with `webrtc`, which no build has. It asserts the exact sentence.
  2. The adapter's sentence choice is covered only where NDI is built. That is now the Mac CI job, so
     no change.
  3. The header comment claimed the bare leaf lets `DYLD_*` work. A hardened-runtime core without
     `allow-dyld-environment-variables` ignores `DYLD_*`, so the comment was corrected.
  4. The middle of the order is pinned by exact-vector equality. Judged adequate; no change.
- **Licence:** the NDI runtime is not bundled. **A licence read covering OD-1/OD-2 (redistribution,
  attribution, trademark wording) is still owed** before a Mac release ships NDI to operators. This
  task decides nothing about it.
- **Ready for the control lane:** N1a supplies the core half (sender built, `ndi-output` capability).
  The shell toggle and `transport.ndi.*` are Task 9 (N1b).

## Task 1 note found in advance: D11 cannot be done in the args builder

The Step 3 live check ran the bundled ffmpeg 8.1.3 against a local HTTPS PUT server whose certificate
comes from a throwaway CA, with five options:
1. no options;
2. `-tls_verify 1 -ca_file /etc/ssl/cert.pem`;
3. the same with the throwaway CA as `-ca_file`;
4. `-http_opts tls_verify=1:ca_file=/etc/ssl/cert.pem`;
5. `-http_persistent 0` plus the global options.

**The `hls` muxer uploaded every segment and playlist under all five, including verification against
the system bundle.**

The control shows the options themselves work. The same global options on a plain `-method PUT -f
mpegts https://…` output fail with `certificate verify failed` against `/etc/ssl/cert.pem` and succeed
with the throwaway CA.

The cause is in the pinned source. `libavformat/hlsenc.c` `set_http_options` (line 333) builds the
options for every segment and playlist open from `method`, `user_agent`, `multiple_requests`, `timeout`
and `headers` only. No TLS option reaches the tls protocol, and hlsenc has no `http_opts`. **No argument
spelling makes ffmpeg 8.1.3 verify an HLS https endpoint.**

Task 1 therefore ships no HLS verify args, and mutation M1d does not apply. The HLS-over-https
certificate obligation stays open. Candidate fixes, each needing an owner call:
- patch hlsenc in `build-macos-ffmpeg.sh` to forward the format's protocol options;
- refuse `https://` HLS on Mac with a sentence;
- accept and document the gap.

## Task 1 (O3): Apple HEVC policy, a real POSIX encoder probe — shipped **without D11**

**HLS over `https://` (D11) is NOT done and is pending an owner decision.** The coordinator relayed this on
2026-11-23. Until the owner decides, ffmpeg is not patched and `https://` HLS is not refused. An HLS
endpoint over `https://` still uploads **without certificate verification**. The cause and the candidate
fixes are in "Task 1 note found in advance" above. Mutation M1d does not apply.

- **What changed.**
  - `EncoderPolicy.h`, Apple branch only: `hevc_videotoolbox` is supported, and H.265 has hardware.
    `preferredEncoderFor("h265", videotoolbox|auto)` and `encoderCandidatesFor("h265", *)` return
    `hevc_videotoolbox`. AV1 still has no encoder on Apple.
  - The Windows branches are unchanged. `isSupportedEncoder` is shared, but nothing in `native/src`
    calls it (review checked).
  - Admission is unchanged (D1): HEVC is still GPU-direct or nothing.
  - POSIX `ffmpegEncoderIsAvailable` now calls `posixFfmpegEncoderIsAvailable`
    (`PosixFfmpegEncoderProbe.h`, header-only so tests reach it in any build). It runs the Windows argv
    (`NUL` → `-`) through `posix_child::spawn` (guarded), with a 5000 ms budget and
    `stopGroup(pid, 200 ms)` on timeout. Only exit 0 counts as available. Before this, it returned true
    for everything.
  - `build-macos-ffmpeg.sh` audits `hevc_videotoolbox`. **This changes the ffmpeg cache key: the next
    release run rebuilds ffmpeg once (about 80 s).**
  - The `corevideo-posix-child-probe` env mode can carry arguments: `exit <n>`, `argv-to <file>`,
    `stubborn <pidfile>`.
- **Addition beyond the plan: an Apple refusal sentence.** `admitStreamStart`'s no-hardware-encoder
  sentence told a Mac operator to stream from a machine with an NVIDIA GPU. On Apple it now reads, for
  example, "H.265 needs the GPU-direct hardware encoder, which is not available on this Mac
  (no-encoder-texture). Choose H.264." For AV1 it adds "Apple Silicon has no AV1 encoder." Windows
  text is unchanged.
- **Tests (new).**
  - `EncoderPolicy.AppleOffersHevcThroughVideoToolbox`.
  - `EncoderPolicy.WindowsHevcPolicyIsUnchanged`: Windows-only; it does not compile on Mac.
  - `PosixEncoderProbe.*` (5): `TrueOnlyOnExitZero`, `AMissingExecutableIsNotAvailable`,
    `TimesOutAStubbornChildAndReapsIt` (it asserts the fixture invariant: the stubborn child is alive
    before the budget expires), `PassesTheEncoderAndANullSink` (exact argv), and
    `BundledFfmpegEncodesWithBothVideoToolboxEncoders`. The last is opt-in: it **passes** with
    `COREVIDEO_TEST_FFMPEG=<cache a7ff16a2b56f5790>`, and also asserts `libx264` is unavailable.
  - `StreamStartAdmission.AppleRefusalSentencesNameTheMacNotNvidia`.
  - `OutputSenderAdapter.AppleRefusesHevcOffTheGpuPath`, **confirmatory** (rule 8). It pins D1 after
    the flip, and passes with the old policy and with an always-true probe.
  - The Apple branch of `EncoderPolicy.HevcShipsOnWindowsNvencOnly` pinned the old "no HEVC on Mac"
    policy. It is now Windows-only, with a comment pointing at the new test.
- **Mutations** (each applied alone, rebuilt, restored with `touch`):

  | Mutation | Result |
  |---|---|
  | M1a: Apple H.264-only | `AppleOffersHevcThroughVideoToolbox` red |
  | M1b: probe returns true | 4 of 5 `PosixEncoderProbe` red; the opt-in one was skipped |
  | M1c: no timeout | `TimesOutAStubbornChildAndReapsIt` red: "did not give up within its budget" |
  | M1e (added): Apple sentence reverts to the Windows text | `AppleRefusalSentencesNameTheMacNotNvidia` red |

- **Review** (independent agent):
  1. *Important:* on Apple every candidate list has one entry, and `selectFfmpegVideoEncoder` falls back
     to `candidates.front()`. So the probe's verdict cannot change the selection. It costs a 30-frame
     VideoToolbox encode on every ffmpeg start, and up to about 5.2 s if it hangs. On the GPU-direct
     path its answer is overwritten (`gpu-direct-<codec>`).
     - Windows has the same shape for H.265 and AV1 (one candidate, probed before the GPU decision).
     - Kept as the plan specifies. **Recorded as advisory-only on Apple**, and listed as a candidate
       issue: probe after the GPU decision, or make "no candidate opens" a refusal.
     - The probe ends before the GPU encoder starts, so the two do not contend for VideoToolbox.
     - Separately, a VideoToolbox session can be refused while other encoders hold the media engine
       (exit 187 / OSStatus -12903, seen during a parallel AVF test run).
  2. *Important:* `AppleRefusesHevcOffTheGpuPath` left `COREVIDEO_TEST_PROBE_MODE` set after an early
     `ASSERT` return. Fixed with an RAII guard. `FakeFfmpeg` now also restores an inherited
     `COREVIDEO_CHILD_GUARD_PATH` instead of unsetting it.
  3. *Minor:* "this Mac cannot provide one" read wrongly for HEVC, whose policy now says hardware
     exists. Reworded as above.
- **Counts.**
  - Full suite (before the review fix-ups): **2252 passed, 3 failed** of 2255. All three failures are
    `AvfProgramTail.*`, the suite that flaked at baseline. Rerun alone twice after the rebuild:
    **7/7 and 7/7**.
  - Touched suites after the fix-ups (`EncoderPolicy`, `PosixEncoderProbe`, `StreamStartAdmission`,
    `OutputSenderAdapter`, `RtmpCompatibility`, `FfmpegChildRetirement`, `PosixChildProcess`):
    **53/53**.
  - `build-macos-ffmpeg.sh --audit` passes on the cached LGPL ffmpeg, and still refuses Homebrew's build.
## Task 9 (N1b): NDI output as its own output (shell)

Branch `mac-outputs-phase4-n1b`, stacked on slice 1 (#1231). It needs only Task 8, not Task 1.

**Lane split (confirmed by the coordinator).** The control lane's Task 6 owns `transport.ndi.set` and
`transport.ndi.toggle`, the `ndi` feedback/OSC flag, the `contracts/control.platforms.json` rows and the
regenerated manifest. It drives `AppModel.setNdiOutput(_:)`, which this task provides. So the plan's
cases `ndi/control-set-and-toggle` and `ndi/osc-feedback-flag` and the contract/Companion steps live
there, not here.

- **Model (`NdiOutput.swift`).**
  - `NdiOutputSettings{enabled=false, programName="CoreVideo Pro Program", groupName="public"}` is
    persisted in `ShellPrefs.ndiOutput`. It is additive, and the round-trip self-check populates it.
  - `NdiOutputPolicy.validate` is Windows `ValidateNdiOutput` minus the key clauses.
  - `NdiOutputPolicy.status` is Windows `NdiStatus` minus " + key", and it reads `status` exactly as
    Windows does.
  - `destinationSettings` produces a row `{id:"ndi", label:"NDI", ndiName, ndiGroup, fps}`.
- **AppModel.**
  - `setNdiOutput(_:)` validates only when turning on. A refusal changes only the reason and is
    returned. The same value is idempotent. It never touches Stream or Record.
  - `startProgramOutput` appends `ndi` and its row whenever NDI is on.
  - **Stream stop sends `stop-encoder-session` only when Record and NDI are both off** (Review Focus 2).
  - `onConnected` re-arms NDI on every core generation (Review Focus 1). The core-exit handler clears
    `ndiSenders`, so a stopped core reads "Starting", never a stale "Live".
  - Name and group are committed on Return or focus loss. While NDI is on, a name that no longer
    validates turns NDI off, never the stream. While NDI is off, an edit only saves.
  - A failed update logs and pushes a warning; NDI stays on.
  - `ndiStreamFps` is 30, because the Mac stream rows pin 30 until Task 7's picker.
- **UI.** The footer NDI tile is live: the main button toggles, and the flyout holds the editor. The
  Settings tab gains the Windows NDI card after Streaming destinations:
  1. the "NDI output" switch;
  2. the status line;
  3. Program name | Group;
  4. "NDI graphics key is not available on Mac."

  Offscreen renders of the Settings tab and the footer were checked.
- **Health.** `StudioHealthReadout.pressuredSenders` now skips `ndi`, so NDI pressure can never read as
  stream backpressure.
- **Tests: 18 `ndi/*` cases. Shell suite 7930 checks, the 2 pre-existing failures only.**
  - Ports of the Windows telemetry, validation and destination tables.
  - toggle-on-alone, refused-on, stream-stop-keeps-ndi, all-off-sends-stop.
  - rearmed-on-core-generation, stopped-core-reads-starting, blank-name-while-on.
  - not-streaming, never-asks-on-quit, record-start-and-stop-keep-ndi.
  - rename while off and while on, pressure-is-not-stream-health, failed-update-is-loud, prefs.
- **Shell suite.** The base shell at `3529d568`, built in a throwaway worktree, has the same 2 failures:
  `ShellTestsMedia.swift:278/286`, an `.m4a` probe at 1068 ms against 1000±25 ms. That looks like an
  AAC priming change on macOS 27.2.1. They are **pre-existing and not caused by this lane** (candidate
  issue).
- **Self-check and lint.** `COREVIDEO_SHELL_SELFCHECK=1` passes, including the prefs round-trip.
  `lint-mac-design.sh` passes.
- **Mutations** (each applied alone, rebuilt, restored):
  - **M9a** (old stop rule): stream-stop-keeps-ndi red, 3 checks.
  - **M9b** (no re-arm): rearmed-on-core-generation red.
  - **M9c** (status from lifecycle): the status-table parity row red. This is deliberate parity with
    Windows, as the plan notes.
  - **M9d** (added: no `ndiSenders` reset on exit): stopped-core-reads-starting red.
- **Review** (independent agent). All findings were real and are fixed:
  - **A.** A rename while NDI was off sent `stop-encoder-session`, or re-sent a live stream's full state.
    Now save-only, as Windows does; a case was added.
  - **B.** A failed update was silent. Now a warning; a case was added.
  - **C.** Test gaps: Record start/stop with NDI, quit guard, rename while on, a flaky re-arm wait that
    matched any body, and the default name used as a fixture value. All added or fixed.
  - **D.** An NDI sender with pressure could read as stream health. Now filtered; a case was added.
  - **E.** Editor drafts went stale across the two editors, and a dismissed popover lost its edit. Drafts
    now refresh when not focused, and the editor commits on disappear.
- **Owed on owner hardware (Step 3).**
  - Check that NDI Studio Monitor on a second machine sees "CoreVideo Pro Program" with audio, that a
    stream stop/start leaves it running, and that a core kill reads Starting then Live.
  - Record whether a Local Network prompt appears.


## Task 2 (O4a): `VideoToolboxGpuVideoEncoder`

Branch `mac-outputs-phase4-o4`, stacked on slice 2 (`-o3`).

- **What changed.**
  - New `VideoToolboxGpuVideoEncoder.{h,mm}` behind `GpuVideoEncoder`.
  - `createSharedProgramGpuVideoEncoder()` builds its pool from the VideoToolbox factory on Apple
    (`COREVIDEO_HAS_VT_GPU_ENCODER`, defined when the RTMP-output build compiles the `.mm`) and from Media
    Foundation elsewhere.
  - **Nothing streams through it yet.** A sender takes the GPU path only for a frame that carries an
    encoder texture, and Metal exports none until Task 3.
  - The shared pool's H.264 CBR SEI filler (`H264CbrFiller`) applies on Apple exactly as on Windows.
- **Encoder settings.**
  - Hardware is required. Real time on; frame reordering off (D2).
  - Profile: H.264 High/Main/Baseline AutoLevel, HEVC Main AutoLevel.
  - `MaxKeyFrameInterval` = `configuredGopFrames`, plus `MaxKeyFrameIntervalDuration`.
  - BT.709 primaries, transfer and matrix, on the session and as source attachments.
  - Rate control:
    - `AverageBitRate` always.
    - CBR adds `ConstantBitRate` (row E-2).
    - VBR adds a 1 s `DataRateLimits` cap at 1.5x.
- **Frame handling.**
  - `submit` never blocks. A serial queue waits up to 34 ms for `publishedFrameNumber`. A missing surface,
    a wait timeout, an already-rewritten slot or more than 2 frames in flight count as **skips**, never
    failures.
  - PTS = `frameNumber * 10'000'000 / fps`, the same absolute epoch as
    `MediaFoundationGpuVideoEncoder.cpp:685`.
  - Output is Annex B, with SPS/PPS (or VPS/SPS/PPS) leading each keyframe. `requestKeyframe` makes the
    next picture `kVTEncodeFrameOptionKey_ForceKeyFrame`.
  - `healthy()` turns false on an invalid session, an encoder malfunction or 30 consecutive errors.
  - `stop` drains, completes, invalidates, and is idempotent.
- **Defect the tests found in my first cut:** `stop()` let queued frames return without encoding and
  without counting a skip. As a result 1 frame in 11 vanished in three tests. Queued frames now encode
  while stop drains; `running` gates only new submits.
- **Plan fixture corrected (rule 4: suspect the test first).** `BitrateIsHonoured` specified pure LCG
  noise. A raw VideoToolbox spike (scratchpad `p4spike/bitrate.mm`, 1280x720@30, 300 frames, 4 Mbps)
  gave:

  | Content and settings | Bytes (target 5.00 MB) |
  |---|---|
  | Pure noise; CBR; average+cap+CBR; max QP 51; real-time pacing (every combination tried) | **31.27 MB** |
  | Moving gradient + light noise, CBR | **5.01 MB** |

  Noise is incompressible, so no correct encoder could pass. The test now uses the moving gradient
  (still a new picture every frame), and the fixture comment records why.
- **Tests:** `VideoToolboxGpuVideoEncoder.*` **8/8**. Neighbouring `SharedGpuVideoEncoder*`,
  `GpuVideoEncoder*`, `GpuEncodePath*`, `ChooseStreamEncodePath*`, `H264CbrFiller*` and
  `OutputSenderAdapter.*`: 32/32.
- **Mutations** (each applied alone, rebuilt; the restored file is byte-identical to the original):

  | Mutation | Result |
  |---|---|
  | M2a: reordering on | HEVC no-reordering red (pts != dts) |
  | M2b: no parameter sets on keyframes | H.264 and HEVC round trips red, colour red |
  | M2c: keyframe flag ignored | requested-IDR test red |
  | M2d: AverageBitRate and ConstantBitRate both dropped | bitrate test red (7.95 MB > 6 MB) |
  | M2e: 601 matrix | colour test red |
  | M2f: AVCC length prefixes left in | both round trips and colour red |
  | M2g: missing surface fails the encoder | missing-surface test red |

  - M2a also reddened three more tests. With reordering on, the encoder holds frames, the two-in-flight
    limit fills, and frames are skipped. That is a second reason the setting must stay off.
  - **Dropping only one of the two rate keys stays green, and that is correct behaviour:** each key alone
    holds the 4 Mbps target on structured content (spike: CBR alone gives 5.01 MB). The test pins "a rate
    is honoured", not which key carries it.
- **Review** (independent agent):
  1. *Important:* `submit()` could race `stop()`, by dispatching onto the queue stop had just drained and
     released, or reading `session` while stop tore it down. This is not reachable through the shared
     pool, which serializes under its own mutex, but the `GpuVideoEncoder` contract promises it. Fixed: a
     `submitMutex` covers submit's running check, in-flight increment and dispatch, and stop's flip of
     `running`, so the drain is final.
  2. *Important:* the decode helper returned the last good picture's luma, so a stream with broken
     P-frames could pass. It now requires every chunk to decode.
  3. The HEVC `pts == dts` check is marked confirmatory, because `emit` defaults `dts` to `pts`. The
     input-order check is the real evidence.
  4. Lower findings taken:
     - a malformed or truncated VideoToolbox output is counted (`vt-output-malformed`, skip) instead of
       lost silently;
     - a keyframe request survives a refused encode.
  5. Lower finding **not** taken: when the slot was rewritten (published > frameNumber), Windows encodes
     the newer picture under the published number. The Mac skips, because encoding it would duplicate
     that number's PTS when its own submit arrives. This is recorded as a deliberate divergence.
- **Hardware-session starvation (candidate issue, outside this lane).**
  - In one test process, `AvfEncoderRecording.*` + `AvfCaptureArrival.*` + `AvfProgramTail.*` leave
    VideoToolbox answering **-12903** (`kVTVideoEncoderNotAvailableNowErr`) for every later hardware
    session until the process exits. Bisected: no single suite does it; the third one in the sequence
    tips it.
  - `AVFoundationEncoderAdapter.mm` has no `@autoreleasepool`. That is consistent with autoreleased
    writer objects outliving their recordings, but **not proven**.
  - Whether the shipping recorder holds hardware sessions across recordings is **unverified**. If it
    does, a GPU-direct stream could fail to start after several recordings in one core. Owed: a
    record-N-times-then-stream check in Task 6's drill on a quiet machine.
  - Meanwhile the hardware tests probe for a session first. Without one they skip loudly, using the
    repo idiom: `[  SKIPPED ] … - this test did NOT run`. That covers CI virtual machines too. **Run
    alone they all execute: 8/8.**
- **Full native suite** (`-DCOREVIDEO_WITH_NDI_OUTPUT=ON` build, before the skip guard): 2256 passed,
  7 failed. The 7 failures were exactly the hardware tests failing with -12903 as described above. With
  the guard, the AVF+VT ordering runs 40 passed, 0 failed, with 7 loud skips.

### Hardware-session exhaustion: resolved as a PRODUCTION defect — fixed in its own PR #1242 (off `main`)

The coordinator asked for a bounded investigation. The answer is **production code, not the tests**.

- **The composition.** The core wraps the AVFoundation recorder in one `AsyncEncoderSink` for the life of
  the process. Every recording opens and finalizes on that sink's long-lived writer thread, and none of
  its AVFoundation calls ran inside an autorelease pool. Autoreleased writer inputs, adaptors and sample
  buffers were therefore never drained, and they kept their VideoToolbox sessions alive.
- **The test.** `AvfRecorderSessionRelease.FinishedRecordingsGiveTheirHardwareSessionsBack` drives 40
  short recordings through that exact composition and probes for a hardware session after every take.
  - On `main` it fails: **exhausted at take 31 (-12903)**.
  - With the fix (`@autoreleasepool` around every `AvfMp4Writer` entry point) it passes.
- **Results on #1242.** Native suite 2242 passed, 0 failed; the AVF suites 72/72. An independent review
  found the fix correct; its test-hygiene findings were fixed.
- **Effect on this branch.** With #1242's change applied locally, the VideoToolbox encoder tests run, and
  pass, after the AVF suites instead of skipping (157 passed, 0 failed). The skip guard stays: it
  still covers CI virtual machines with no media engine.
- **Follow-ups (review, not done).** These Metal compositor threads call Cocoa per frame without a pool:
  the full-res tap, the vcam tap, and the vcam prep thread.

## Task 3 (O4b): Metal encoder IOSurface ring; the sender accepts an IOSurface

Branch `mac-outputs-phase4-o4b`, stacked on `-o4`.

- **What changed.**
  - `CompositorRenderPlan::encoderTextureExport`. MediaCore sets it only while `rtmp`, `srt` or `hls` is
    in the output set; D3D11 ignores it.
  - **Metal.** A ring of three non-global BGRA IOSurfaces (row E-1) is built on a preparation thread the
    first tick a stream wants it. Until it is ready a tick exports nothing and counts no shed.
  - **Export, every tick (fast and full).** The tick blits Program into the next slot that is not
    `IOSurfaceIsInUse`, in its own command buffer, with no GPU wait. The slot's `publishedFrameNumber`
    is stored in the blit's **completion** handler. The frame carries
    `encoderSharedTexture{iosurfaceId, w, h, B8G8R8A8_UNORM, frameNumber, published}`.
  - A tick with all three slots in use exports nothing and increments `encoderExportShedFrames()`.
  - The ring is freed the first tick the flag drops, and rebuilt on a size change.
  - **Sender.** It treats `sharedHandleHex` **or** `iosurfaceId` as "has encoder texture" and passes
    `iosurfaceId` to the encoder. New test hook: `setGpuVideoEncoderFactoryForTest`.
- **Tests (5):**
  - `MetalEncoderExport.ExportsTheComposedFrameIntoTheRing`: the centre pixel of the exported surface is
    (40,120,200) ±1 once published.
  - `…NeverRewritesAnInUseSurface`: three distinct ids; all in use gives no export and exactly one shed;
    export resumes once released.
  - `…NoExportWhenNoStreamDestination`.
  - `OutputSenderAdapter.IosurfaceEncoderTextureChoosesGpuDirect`: a fake encoder sees id 77; the fake
    ffmpeg is the probe binary in `eof-exit` mode.
  - `MediaCore.EncoderTextureExportFollowsStreamDestinations`: recording alone, false; +srt, true;
    ndi, false; hls, true.
- **Mutations** (each applied alone, restored):

  | Mutation | Result |
  |---|---|
  | M3a: sender checks only `sharedHandleHex` | sender test red |
  | M3b: no in-use check | in-use test red ("tick 2 rewrote an in-use surface") |
  | M3c: export regardless of flag | no-export test red |
  | M3d: publish at commit | **red, against the plan's expectation of green**: the export test reads the surface the moment `published` says so and sees pre-blit pixels, so the ordering is pinned by a unit test, not only by Task 6's frame-identity gate |
  | M3e (added): MediaCore never sets the flag | flag test red |
- **Review** (independent agent):
  1. *Important:* a slot being rewritten kept advertising its old frame number until the new blit
     **completed**. A backlogged encoder could therefore read it mid-blit under the old frame's timestamp:
     Metal's own in-flight blit does not raise the IOSurface use count. Fixed in two places:
     - the compositor stores a sentinel (-2) into the slot's `published` before committing the rewrite;
     - the encoder re-checks `published` after taking its use count.

     New test `ARewrittenSlotStopsAdvertisingItsOldFrameAtCommit`. Mutation **M3f** (no sentinel) turned
     it red in 5 of 5 runs; restored, it passed 5 of 5.
  2. *Important:* `encoderExporting()` stored false at the start of every tick, so a snapshot-thread read
     could see a transient false. Render's early returns could also leave a stale true. Now one store per
     tick, and false on the early returns.
  3. *Minor, not changed:* a frame queued when the ring is released (size change or stream stop) fails
     its lookup. That is a counted skip and sets `lastFailure` to `iosurface-lookup-failed`
     (informational).
  4. *Minor, not changed:* there is no test for a size-change rebuild or for the flag dropping while
     the ring builds.
- **Counts.**
  - Full native suite (`native/build-metal`, NDI on, built before the review fixes): **2268 passed,
    0 failed**. The 7 VideoToolbox tests skipped loudly, because this branch does not carry #1242.
  - After the fixes: `MetalEncoderExport.*` + `VideoToolboxGpuVideoEncoder.*` + the two sender/core
    tests, **14/14, three runs**.

### Metal compositor threads (coordinator follow-up to #1242): no change needed

None of the three threads the #1242 review flagged creates autoreleased Cocoa objects per iteration, and none holds an IOSurface or VideoToolbox object across iterations:
- **Full-res tap thread:** reads a plain (non-IOSurface) Metal texture with `getBytes` into a `std::vector`.
- **Vcam tap thread:** copies an `MTLBuffer.contents` pointer into a vector and hands it to a C++ shared-memory publisher.
- **Vcam prep thread:** runs once, is already pooled, and exits.

The IOSurface-backed pixel and sample buffers are made in the separate virtual-camera helper process (`CmioVirtualCameraSink`). They use balanced CF create/release and a recycling pixel-buffer pool. No PR was opened.

### #1242 CI segfault: a pre-existing decoder race, fixed on #1242

CI's `native-metal-macos` on #1242 segfaulted in `AvfMediaPixel`.
- **Crash report:** `EXC_BAD_ACCESS` in `FigSimpleMutexUnlock` under `-[AVAssetReaderTrackOutput copyNextSampleBuffer]`, on the media decoder's reader queue.
- **Not caused by #1242:** with #1242's recorder change reverted, the suite crashed 1 run in 20, identically.
- **Cause:** the clip reader destructors called `cancelReading` on the owner thread, concurrently with a read.
- **Fix:** `cancelReading` now runs on the reader queue, in both the video and audio readers (commit 2 of #1242).
- **Stress test:** 2 of 5 crashes before the fix; 10 of 10 passes after.
- **With the fix:** `AvfMediaPixel` + `AvfMediaDecoder` had 0 failures in 20 runs.

## Task 4 (O4c): POSIX bitstream queue, writer thread, TS envelope and backpressure

Branch `mac-outputs-phase4-o4c`, stacked on `-o4b`.

- **What changed.** The Windows-only GPU-direct machinery now runs on every platform:
  - the queue (60 chunks / 2 MiB, GOP-tail discard);
  - the writer thread with the `EncodedVideoTransportStream` TS envelope;
  - `observeStreamBackpressure`, the pipe-write telemetry, and the failure state.

  It is lifted, not copied. The only platform piece is `writeSomeToFfmpeg() -> PipeWriteOutcome{written, stopped, broken, error}`:
  - **Windows:** the same blocking `WriteFile`; Stop still uses `CancelSynchronousIo`.
  - **POSIX:** non-blocking stdin plus `poll({stdin POLLOUT, stop pipe POLLIN}, 100 ms)`. Stop writes the stop pipe, joins the writer, and only then closes stdin.
  - **Latent POSIX defect fixed:** the POSIX encoder callback used to write raw Annex B into the pipe on the encoder thread, while the args declared a timestamped TS input. That would have been a garbage mux, and a wedged ffmpeg blocked the shared encoder. The callback now only enqueues.
  - `streamClock` publication stays Windows-only until Task 5's unified TS mux.
- **Tests.**
  - New: `BitstreamQueuePosix.*` (4).
    - The timestamped TS envelope: sync bytes at offsets 0 and 188; first PES PTS 0, then 3000.
    - A wedged reader sheds GOP tails, with encoder `start` count 1 and the sender not failed.
    - Stop takes under 2 s on a wedged pipe, and the stand-in is reaped.
    - Review-added: an ffmpeg that exits breaks the pipe and fails the sender, and Stop is prompt.
  - The four `RtmpOutputSenderBackpressure.*` queue cases (overflow, GOP-tail discard, fresh-IDR wait) were Windows-only. They now run and pass on the Mac.
- **Plan fixture corrected (rule 5).** The plan expected the first PES PTS to be 9000. `EncodedVideoTransportStream` documents that the first video DTS is subtracted from every PID, so the first access unit is PTS 0. The second is 3000, which also pins the packetizer's round-to-nearest tick.
- **Fixture trap found and fixed.** The Task 1 encoder probe runs the same fake `ffmpeg`. In plain `stubborn` mode it blocked every start for its 5 s budget and wrote the pidfile the stop test read. The new probe mode `stubborn-stream` exits 0 for the encoder probe's `-f null` and is stubborn otherwise.
- **Not ported (plan step 1.4).** `StreamBackpressureRateProbeTest` drives the Media Foundation encoder on D3D11, so it stays Windows-only. Its Mac equivalent is Task 6's slow-sink gate.
- **Mutations.**

  | Mutation | Result |
  |---|---|
  | M4a: callback writes the pipe | TS test red; wedged test hangs |
  | M4b: no TS packetizing | TS test red |
  | M4c: no stop-pipe wake | **stays green, by design**: the 100 ms poll timeout re-checks the stop flag, so Stop is bounded without the wake (documented at the poll) |
  | M4c′: no pipe and an infinite poll | hangs |
  | M4d: no backpressure observation | wedged test red |
  | M4e (review-added): broken pipe not recorded | failure-path test red |

- **Counts.**
  - Full native suite (before the review fixes): **2334 passed, 0 failed**.
  - `BitstreamQueuePosix.*`: 4/4, three runs.
- **Review** (independent agent). No production defect.
  - Taken: the failure-path test, saving and restoring the env vars, and the PTS rounding comment.
  - Stop pipe kept: it wakes at once, and the 100 ms timeout is the safety net.
  - Windows log difference: only the transport-audio failure line now prints `err=0` for a zero-byte success, where it used to print a stale `GetLastError()`. There is no behaviour change.

## Task 5 (O4d): AudioToolbox shared AAC; one-clock TS mux on POSIX

Branch `mac-outputs-phase4-o4d`, stacked on `-o4c`.

- **What changed.**
  - **AAC encoder.** `ProgramAacEncoder` has an Apple branch: AudioToolbox `AudioConverter` from Float32 48 kHz stereo to AAC-LC at 160 kbps (D4, the MF encoder's 20000 B/s), one 1024-frame packet at a time, with a hand-written 7-byte ADTS header.
  - **Link.** AudioToolbox is now linked in every Apple build, because the encoder is compiled in every configuration.
  - **Sender.** The `#538 Slice 8` unified-TS decision (shared H.264/HEVC plus shared AAC in one core-stamped TS on stdin) now runs on every platform. When it is on, POSIX no longer opens the fd 3 PCM pipe. The slot clock and the `streamClock` publication run on POSIX too.
- **Priming (a deliberate deviation from the plan).**
  - AudioToolbox primes AAC-LC with **2112** frames (logged at start). That is not a multiple of 1024, so the plan's "first packet's pts100ns is 0" cannot hold exactly.
  - Packet n is stamped `sampleIndex = n*1024 - 2112`. Packets wholly before sample 0 are dropped.
  - So the first kept packet is **sampleIndex 960 / pts 200000**, pinned by a test.
  - Review notes: ffmpeg's ADTS decode applies no skip of its own, so this mapping leaves no constant offset. Windows' MF path stamps raw input time and may carry a constant audio-late offset that Apple now compensates. The live clap gate decides; it is still owed on a quiet machine.
- **Tests.**
  - The existing `EmitsSampleCountedAdtsAtFortyEightKhzStereo` now also runs on Apple.
  - New:
    - `AppleAdtsHeadersAreLc48kStereo`: sync, LC, index 3, channels 2, `frame_length` equals the packet size.
    - `AppleSampleIndexAdvancesBy1024`: steps of +1024 and 213333 ±1 (100 ns); first kept packet at 960 / 200000; `acceptedSamples() == 48000`.
    - `AppleDecodesToTheInputTone`: decoded through ExtAudioFile; RMS within 10% of 0.3536; strongest 10 Hz bin at 1000 Hz.
    - `OutputSenderAdapter.PosixGpuDirectUsesUnifiedTs`: `streamClock.muxInput == "unified-ts"`, and the ffmpeg argv has no `pipe:3`.
- **Mutations.**

  | Mutation | Result |
  |---|---|
  | M5b: ADTS profile 2 | header test and tone test red |
  | M5c: unified TS kept Windows-only | sender test red |
  | M5d (the unit form of the plan's M5a): priming ignored | first-packet assertion red |

  The live clap-gate form of M5a is deferred to a quiet machine, along with the five-run `validate-av-clap.mjs --shared-aac-gate` content gate in Step 5.
- **Counts.** Full native suite **2340 passed, 0 failed**; the touched sender/AAC suites 41/41 after the review fix.
- **Review** (independent agent). No issue at or above threshold.
  - Taken: `close()` of the unused audio pipe is now guarded.
  - Kept: the start event's `audioInput:"pcm"` label in unified mode matches Windows.
  - Noted gap: nothing feeds an encoded AAC packet through the POSIX unified writer in a unit test. The live clap gate is that coverage.
- **Defect found afterwards by the Task 6 end-to-end gate, and fixed on this branch.**
  - **What failed.** The first real run of the Mac GPU-direct stream failed: ffmpeg could not probe the video ("unspecified size") and decoded the AAC as garbage.
  - **Cause.** `config.audioInTransportStream = unifiedTsInput_` had stayed inside `#if defined(_WIN32)`, even though the plan named that line. So on POSIX the unified-TS sender still declared a second `-f aac -i pipe:0` input, and ffmpeg split stdin between the TS and an AAC demuxer.
  - **Why the test missed it.** `PosixGpuDirectUsesUnifiedTs` checked only that `pipe:3` was absent. It asserted a proxy, not the property (rule 10).
  - **The test now asserts the property:** exactly one ` -i `, no `-f aac`, and `-map 0:a:0`. Mutation **M5e** (the guard put back) turns it red.
  - **After the fix**, `scripts/validate-gpu-encode.mjs --build-dir native/build-metal --codec h264 --sink-protocol rtmp` **passes on this Mac**: GPU-direct, decoded h264 1920x1080, 58.4 fps of 60 received, sink speed 1.07x. That run was on a loaded machine, so it is functional evidence only; the quiet-machine gate is still owed.
