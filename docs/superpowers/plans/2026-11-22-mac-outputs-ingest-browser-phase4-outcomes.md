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
