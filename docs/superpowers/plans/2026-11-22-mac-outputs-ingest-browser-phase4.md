# macOS outputs, network ingest and browser sources, Phase 4 (O3, O4, N1, I1, B1, B2): implementation plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or
> superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for
> tracking. `docs/superpowers/plan-authoring-rules.md` is binding: every task names the mutations that must
> turn its tests red, and its report says whether each one did. When a supplied test contradicts a
> reasonable implementation, suspect the test first and say so in the report.

Status: DRAFT for owner review, 2026-11-22. Written in worktree `mac-phase4-plans` at `origin/main`
`e117a8fa`. Execution branch: `mac-phase4-outputs`, cut from `origin/main`. Three sub-lanes can run in
parallel once Task 0 is recorded: **encode** (Tasks 1-7), **NDI + ingest** (Tasks 8-11), **browser**
(Tasks 12-16). Task 17 closes the phase.

**Goal:** A Mac operator can stream 1080p60 H.264 or H.265 from the GPU with per-destination
backpressure, send Program over NDI independently of streaming, bring SRT and RTMP contribution feeds into
slots, and key a transparent web page over Program as a downstream key (staged on Preview first), all
matching Windows CoreVideo Pro 1.0.

**Architecture:** Nothing about output policy moves. The shared sender (`RtmpOutputSenderAdapter.cpp`),
supervisor, lifecycle, admission and backpressure policy are reused. The encode lane adds a VideoToolbox
implementation behind the existing `GpuVideoEncoder` seam, a Metal encoder-texture export, and ports the
Windows-only bitstream writer, MPEG-TS envelope and shared AAC encoder to POSIX. NDI, ingest and the
browser host already have Mac code paths; the work is build gates, child lifetime, packaging, OS
boundaries (ATS, Local Network, hardened runtime) and the SwiftUI surfaces, ported from the Windows
XAML layout.

**Tech Stack:** C++20/Objective-C++ (VideoToolbox, CoreMedia, IOSurface, Metal, AudioToolbox, WebKit,
AppKit), SwiftUI shell (`mac-shell/`), bundled LGPL ffmpeg 8.1.3 (`scripts/build-macos-ffmpeg.sh`),
`corevideo-child-guard`, gtest (`corevideo-native-tests`), the shell's own case runner
(`COREVIDEO_SHELL_TESTS=1`), Node/Python QA harnesses.

**Spec:** `docs/superpowers/specs/2026-11-15-mac-outputs-ingest-browser-design.md` (§5.1 GPU-direct,
§5.2 NDI/ingest, §5.3 browser), sequenced by `docs/superpowers/plans/2026-11-15-mac-parity-master-plan.md`
Phase 4. Prior lanes this builds on: `2026-11-21-mac-streaming-phase2{,-outcomes}.md` (F1, P1, D1) and
`2026-11-22-mac-settings-quickwins{,-outcomes}.md` (O1, O2, Z1, Z2). Executors read the spec, the two
outcomes docs and this plan.

Path shorthand: `N/` = `native/src/`, `M/` = `mac-shell/Sources/CoreVideoProShell/`,
`W/` = `native-shell/CoreVideoPro.WinUI/`, `MC/` = `native-shell/CoreVideoPro.MediaCore/`.

---

## Global Constraints

- **Owner decisions in force:** ship NDI **output**, defer receive (N2 stays Phase 5); browser page audio
  is **silent**, matching Windows; **no 60 fps stream picker until O4 lands** (Task 7 is the first place
  it appears); ffmpeg is the bundled LGPL build (no Homebrew, no GPL, no nonfree).
- **Toolchain:** native builds need `SDKROOT=/Library/Developer/CommandLineTools/SDKs/MacOSX26.5.sdk`.
  On this machine that SDK is in Command Line Tools, not in `Xcode.app` (which now carries only
  MacOSX27.x, whose `.tbd` files `ld-1267` rejects). If it moves, find it with
  `ls /Library/Developer/CommandLineTools/SDKs /Applications/Xcode.app/Contents/Developer/Platforms/MacOSX.platform/Developer/SDKs`.
- **Native configure** (CI `native-metal-macos` flags plus this phase's gates; Task 8, 10 and 14 add the
  last three to `.github/workflows/ci.yml:298-308` and `scripts/release-macos.sh:179-188`):
  ```
  SDKROOT=<26.5 sdk> cmake -S native -B native/build-metal -G Ninja \
    -DCOREVIDEO_STUB=OFF -DCOREVIDEO_ENABLE_DEV_ADAPTERS=ON -DCOREVIDEO_WITH_METAL=ON \
    -DCOREVIDEO_WITH_AVF_ENCODER=ON -DCOREVIDEO_WITH_AVF_MEDIA_DECODER=ON -DCOREVIDEO_WITH_COREAUDIO=ON \
    -DCOREVIDEO_WITH_AVF_CAPTURE=ON -DCOREVIDEO_WITH_SCK=ON -DCOREVIDEO_WITH_RTMP_OUTPUT=ON \
    -DCOREVIDEO_WITH_MAC_VIRTUALCAM=ON \
    -DCOREVIDEO_WITH_NDI_OUTPUT=ON -DCOREVIDEO_WITH_SRT_INGEST=ON -DCOREVIDEO_WITH_RTMP_INGEST=ON \
    -DCOREVIDEO_WITH_BROWSER_HOST=ON
  ```
  Native tests: `./native/build-metal/corevideo-native-tests --gtest_filter=<suite>.*`.
  Shell tests: `cd mac-shell && swift build -c release && COREVIDEO_SHELL_TESTS=1 .build/release/CoreVideoProShell`;
  `COREVIDEO_SHELL_SELFCHECK=1` must PASS; `bash scripts/lint-mac-design.sh` green;
  `npm run contract:check` clean after any manifest change.
- **Every new helper process runs under `corevideo-child-guard`** via `posix_child::spawn`
  (`N/modules/PosixChildProcess.h`). No new `fork`/`execv`. Stops go through `posix_child::stopGroup`
  off every adapter mutex.
- **Anything the core finds beside its executable must also be found when the core runs from
  `/Library/Application Support/CoreVideo Pro/`** (the pkg install, `M/CoreLocation.swift`). The shell
  names such paths in `CoreLaunchEnvironment.extras` (`M/AppModel.swift:6167-6212`) and the shell test
  `streaming/launch-env-names-the-bundled-guard-and-ffmpeg` is the pattern to extend.
- **One-shot state is re-applied on every core generation** from `AppModel.onConnected()`
  (`M/AppModel.swift:1773`): NDI on, browser sources and their DSK/PVW keys, ingest configuration.
- **Secrets never enter `ShellPrefs` or logs:** RTMP ingest URLs (their stream path is a secret, Windows
  `RtmpIngestSource.Summary`) live in the Keychain (`M/KeychainStore.swift`); SRT ingest passphrases are
  session-only exactly as on Windows (no persistence at all).
- **Honest reporting** (`docs/reference/honest-reporting.md`): every status the operator sees is read from
  the core's snapshot; an unavailable feature keeps its Windows position and says why in a sentence.
- **Signing:** every new Mach-O is Developer ID, hardened runtime, timestamped, with an explicit
  identifier; the browser host is a nested app at `Contents/Helpers/CoreVideo Browser Host.app`,
  identifier `us.iamfatness.corevideopro.browser-host`, **no entitlements**. The core keeps
  `disable-library-validation` (it already exists for NDI, `scripts/macos-core.entitlements`).
- **Licensing:** no new third-party code is linked or bundled by this plan. WebKit, VideoToolbox and
  AudioToolbox are OS frameworks. The NDI runtime is **not** bundled (Windows does not bundle it either);
  it is `dlopen`ed from the operator's NDI install. Any change to that is owner decision OD-1.
- **Copy is Windows copy, verbatim,** unless a step gives a Mac sentence. Layout follows the Windows XAML
  structure named in each shell task (rows, column spans, Expander order), not just the control list.
- **Perf and delivery gates run on a quiet machine** (CI GPU is about 30x slower; CI runs correctness
  only). Per-frame delivery is the gate, never mean fps (CLAUDE.md "Frame-delivery acceptance").
- **Injected clocks:** any test that waits on time uses the code's injectable clock or a bounded poll
  with a fixture-chosen deadline; no bare `sleep` as the assertion.
- **Windows code is untouched** except shared `native/` files whose change is pinned by a test that runs on
  both platforms (named in the task).

## Review Focus

Inputs the spec implies that are easy to leave untested; each has its test in the owning task.

1. **Core restart mid-show** with NDI on, a browser overlay keyed DSK-in, and an SRT ingest source in a
   slot: after respawn NDI is re-armed, the browser source is re-added and the DSK key follows it to its
   new `browser:<n>` id, and the ingest config is re-sent. Tests: Task 9 `ndi/rearmed-on-core-generation`,
   Task 15 `browser/readded-and-rekeyed-on-core-generation`, Task 11 `ingest/config-resent-on-core-generation`.
2. **Stopping the stream while NDI is on and not recording** must not stop NDI. Today the stop path sends
   `stop-encoder-session` whenever recording is off (`M/AppModel.swift:4325-4343`). Test: Task 9
   `ndi/stream-stop-keeps-ndi`.
3. **Reload actually reloads, Remove leaves no process, a host that dies five times gives up loudly.** The
   Mac host ignores `reload\n` today and `closeProcessLocked` leaks the host on POSIX. Tests: Task 12
   `BrowserHostMac.ReloadCommandReloadsThePage`, Task 13 `BrowserSourcePosix.*`.
4. **A congested uplink on the GPU path** sheds GOP tails, never rebuilds the encoder, and Stop returns
   within 2 s with a wedged pipe (#597 on POSIX). Tests: Task 4 `BitstreamQueuePosix.*`, Task 6 slow-sink gate.
5. **A plain `http://` LAN graphics URL and a `file://` URL load in the shipped host** on a clean Mac, or
   fail with a sentence (ATS, Local Network privacy, `loadFileURL`). Tests: Task 12
   `BrowserHostMac.LoadsFileAndDataUrls`, Task 14 packaged-host check, Task 0 spike rows B-2/B-3.

---

## Defaults taken (owner may override)

| # | Default | Why | Tasks that change |
|---|---|---|---|
| D1 | **HEVC streams only on the GPU-direct path.** Task 1 flips the Apple policy, but `admitStreamStart` keeps refusing a non-H.264 codec off the GPU path (`N/modules/StreamStartAdmission.h:83`), so the shell offers H.265 only once Task 6 proves GPU-direct (Task 7). | Windows ruling 2026-09-20 ("HEVC/AV1 are GPU-direct or nothing"); changing it would change Windows. | 1, 7 |
| D2 | **The VideoToolbox encoder runs with frame reordering off for H.264 and HEVC.** The operator's B-frames setting still applies to the raw ffmpeg path only. | `GpuVideoEncoderConfig` has no B-frames field, so Windows GPU-direct ignores it too; HEVC must be B-frame-free for FLV (gpu-direct-encode.md). | 2 |
| D3 | **Encoder export is a ring of three BGRA IOSurfaces** blitted after the Program pass, only while an `rtmp`/`srt`/`hls` destination is in the output set. NV12 conversion is left to VideoToolbox unless the Task 6 drill shows the conversion costs render time. | Smallest change that feeds VT; the vcam NV12 kernel stays available if measured necessary. | 3, 6 |
| D4 | **Shared AAC on Apple via AudioToolbox, AAC-LC 48 kHz stereo 160 kbps, ADTS**, enabling the Windows one-clock MPEG-TS mux (#538 Slice 8) on POSIX. | Without it the Mac GPU path muxes timestamped TS video against an untimed PCM pipe, the exact A/V clock problem Windows fixed. MF sets 160 kbps (`N/modules/ProgramAacEncoder.cpp:91,103`). | 5 |
| D5 | **Stream fps picker = the destination rows' `fps`** (24/30/50/60, never above the canvas fps). `streamOutputProfile.fps` stays the canvas fps. 60 is offered only when the core reports capability `gpu-direct-stream-encode` available. A start that still falls back to the raw path at 60 streams (Windows parity: H.264 keeps every path) and the row shows the fallback reason. | Windows sends its Stream FPS in `streamOutputProfile`, which the core writes into the render rate (`N/core/MediaCore.cpp:2507-2513`, spec finding 6); copying that would re-rate Program. The sender already decimates to the row's fps (`videoFramePacer_`, `RtmpOutputSenderAdapter.cpp:975`). | 7 |
| D6 | **NDI runtime is the operator's install**, found by absolute paths plus `NDI_RUNTIME_DIR_V6`/`_V5`/`NDI_RUNTIME_DIR`; missing runtime reads a Mac sentence naming NDI Tools. | Windows parity (no redistribution); keeps the NDI SDK licence question out of the release (OD-1). | 8 |
| D7 | **SRT ingest sources are session-only** (Windows keeps them only in memory: `W/ViewModels/StudioViewModel.cs:822`, no store). **RTMP ingest URLs persist in the Keychain** (Windows persists them DPAPI-protected, `FileRtmpIngestSourceStore`). | Windows parity; the spec's "SRT passphrase in Keychain" has no Windows counterpart. | 11 |
| D8 | **Browser sources are session-only and re-added on every core generation**, re-keyed by URL. Not persisted across app launches. | Windows neither persists them nor re-adds them after a core restart (no `browser-add` outside `MediaCoreSupervisor.AddBrowserSourceAsync`); the Mac re-adds because the house rule says one-shot state survives a respawn. Persisting across launches is OD-4. | 15 |
| D9 | **The browser overlay row gets a Remove button** after Reload. | Windows has no Remove in any view (only `browser.remove`), so a Mac source re-added per generation could never be dropped. Flagged as a deliberate Mac addition; file a Windows parity issue. | 15 |
| D10 | **DSK over a Tiles scene follows Windows:** the temporary DSK route is not workspace-authored, so `SceneWorkspacePolicy.IsAuthoredRoute` drops it on a dynamic-gallery scene (`W/Services/SceneWorkspacePolicy.cs:16-17`). The Mac matches, with a test naming the behaviour. | Parity; fixing it is OD-6 for both platforms. | 15 |
| D11 | **HLS over `https://` verifies the certificate** against `/etc/ssl/cert.pem` (the Phase 2 follow-up), folded into Task 1 because it is the same POSIX args builder. | Same defect class as the RTMPS fix (`ef304d6d`). | 1 |
| D12 | **Mac host exit codes:** 2 usage, 4 unusable URL, 5 shared segment failed, 6 WebKit failed to start. Code 3 stays Windows' "WebView2 missing". `browserHostExitMessage` gains the Mac sentences. | Today the Mac host returns 3 for a segment failure, so the operator reads the WebView2 sentence (`native/browser-host/main-macos.mm`, `N/modules/BrowserHostExitMessage.h:21`). | 12, 13 |

---

## Owner decisions needed

| # | Decision | Recommendation |
|---|---|---|
| OD-1 | Bundle `libndi.dylib` in the app instead of requiring NDI Tools? | **No for this phase.** Bundling is redistribution under the NDI SDK licence (attribution, trademark wording, possibly a licence acknowledgement in About). It needs a legal read first. |
| OD-2 | NDI trademark attribution ("NDI® is a registered trademark of Vizrt NDI AB", link to ndi.video). Neither shell shows it today; the adapter also declares the libNDI ABI locally rather than including SDK headers. | **Ask for a licence read covering both platforms** before the Mac release that turns NDI on. Not a blocker for building Tasks 8-9. |
| OD-3 | If Task 0 shows WebKit stops painting the off-screen host (hidden-page throttling, like Chromium's occlusion on Windows), may the host use WebKit SPI (`_setWindowOcclusionDetectionEnabled:` or the hidden-page throttling preferences)? Developer ID allows it; it can break on an OS update. | **Allow it behind one function with a test**, and keep B3 (ScreenCaptureKit self-capture) as the fallback. |
| OD-4 | Persist browser sources across app launches (D8)? | **No**; match Windows. Revisit as a cross-platform feature. |
| OD-5 | When the GPU path is unavailable, should a 60 fps stream start on the raw path (D5) or be refused/clamped on Apple? | **Start, with the fallback reason shown on the row** (Windows parity). |
| OD-6 | DSK over a Tiles scene is dropped on Windows (D10). Fix on both platforms? | **Yes, as a separate cross-platform issue**; this plan matches today's Windows. |

---

## File structure

| File | Responsibility | Task |
|---|---|---|
| `N/modules/EncoderPolicy.h` | Apple HEVC policy | 1 |
| `N/modules/RtmpOutputSenderAdapter.cpp` | POSIX probe; IOSurface encoder texture; POSIX bitstream writer/TS/backpressure; unified TS on POSIX; encode-path publication | 1, 3, 4, 5, 6 |
| `N/modules/RtmpFfmpegArgs.h` | HLS https certificate verification | 1 |
| `scripts/build-macos-ffmpeg.sh` | audit: `hevc_videotoolbox`; ingest demuxers/decoders/muxers | 1, 10 |
| `N/modules/VideoToolboxGpuVideoEncoder.{h,mm}` (new) | VT implementation of `GpuVideoEncoder` | 2 |
| `N/modules/SharedGpuVideoEncoder.cpp` | platform factory | 2 |
| `N/compositor/MetalCompositorAdapter.mm` | encoder IOSurface ring | 3 |
| `N/modules/Interfaces.h`, `N/core/MediaCore.cpp` | `CompositorRenderPlan::encoderTextureExport`; capability `gpu-direct-stream-encode`; sender `encodePath` | 3, 6 |
| `N/modules/ProgramAacEncoder.cpp` | AudioToolbox implementation | 5 |
| `N/modules/NdiOutputSenderAdapter.cpp` | Apple runtime candidates and sentence | 8 |
| `N/modules/PosixChildProcess.h` | stdout fd mapping | 10 |
| `N/modules/SrtIngestCaptureAdapter.cpp` | ingest spawn through the guard | 10 |
| `native/browser-host/main-macos.mm` | host fixes, self-test dump | 12 |
| `native/browser-host/Info.plist.in` (new) | helper bundle plist | 14 |
| `N/modules/BrowserSourceHostAdapter.cpp`, `N/modules/BrowserHostExitMessage.h` | POSIX supervisor; exit sentences | 12, 13 |
| `native/tests/BrowserHostStub.cpp` (new) | scriptable stand-in host | 13 |
| `scripts/make-macos-shell-bundle.sh`, `scripts/release-macos.sh`, `.github/workflows/ci.yml` | gates, bundling, signing, audits | 8, 10, 14 |
| `scripts/mac-show-drill.py`, `scripts/validate-gpu-encode.mjs`, `scripts/validate-av-clap.mjs` | Mac stream gates | 5, 6 |
| `M/OutputProfiles.swift`, `M/StreamDestinations.swift`, `M/SettingsPane.swift`, `M/StudioFooter.swift` | stream codec/fps/E-RTMP; footer recipe; backpressure readout | 7 |
| `M/NdiOutput.swift` (new) | NDI model, validation, status (ports) | 9 |
| `M/NetworkIngest.swift`, `M/NetworkIngestViews.swift` (new) | SRT/RTMP ingest models, wire, Sources catalogs | 11 |
| `M/BrowserOverlays.swift` (new), `M/Overlays.swift`, `M/SourcesTable.swift`, `M/SourcesWorkspace.swift` | browser model, DSK/PVW key, card, Sources add row, source types | 15, 16 |
| `M/ControlSurface*.swift`, `contracts/control.platforms.json`, generated manifest | `transport.ndi.*`, `browser.*`, `ndi` feedback | 9, 16 |
| `M/ShellTestsPhase4*.swift` (new, one per lane) | shell cases | 7, 9, 11, 15, 16 |

---

## Task 0: Re-verify the spec's UNVERIFIED items and the boundary spikes (S, native + release, no deps)

The master plan's rule: each spec's first slice starts by re-verifying its UNVERIFIED items. These
results decide branches inside Tasks 2, 3, 8, 12 and 14. Record each row's answer, the exact command and
the machine/OS in the phase outcomes doc (`2026-11-22-mac-outputs-ingest-browser-phase4-outcomes.md`)
before the dependent task starts. Throwaway code stays in the scratchpad; nothing is committed except the
outcomes rows.

- [ ] **Step 1: Encode rows.**
  - **E-1** Does `IOSurfaceLookup(id)` find a surface created **without** `kIOSurfaceIsGlobal` inside the
    same process? (Decides whether Task 3's ring needs the deprecated global flag the Program surface uses,
    `MetalCompositorAdapter.mm:441`.)
  - **E-2** Does `VTSessionSetProperty(kVTCompressionPropertyKey_ConstantBitRate)` succeed for hardware
    H.264 and HEVC at 1920x1080@60 on this Mac? (Decides Task 2's CBR branch.)
  - **E-3** Does the bundled ffmpeg (`~/Library/Caches/CoreVideoPro/ffmpeg-lgpl/<key>/bin/ffmpeg`) list
    `hevc_videotoolbox` under `-encoders`, and does a 30-frame `-f lavfi` encode with it exit 0?
- [ ] **Step 2: Browser rows** (ad-hoc build of today's `corevideo-browser-host`, run by hand):
  - **B-1** Achieved fps for a page that increments a counter on every `requestAnimationFrame` and paints it,
    with the host window at (-32000,-32000): read the host's `[browser-host] N fps` line after 20 s. Repeat
    with the window on-screen. If off-screen is under 25 fps and on-screen is not, WebKit's hidden-page
    throttling is the cause (OD-3).
  - **B-2** Does a plain `http://<LAN IP>:<port>/` page load from the bare executable, and from the same
    executable wrapped in a bundle whose Info.plist has `NSAppTransportSecurity` →
    `NSAllowsArbitraryLoadsInWebContent` + `NSAllowsLocalNetworking`? Serve with
    `python3 -m http.server --bind <LAN IP>`.
  - **B-3** Is a Local Network prompt shown on macOS 15+/26 for B-2, and which app does it name (the shell,
    the host, or none)? Run on a clean user account if available.
  - **B-4** Does `loadRequest:` with a `file://` URL render, or only `loadFileURL:allowingReadAccessToURL:`?
- [ ] **Step 3: NDI rows** (owner hardware: needs an NDI install on the Mac).
  - **N-1** Where does the NDI install put `libndi.dylib` on macOS (list every path found with
    `mdfind -name libndi`), and is any `NDI_RUNTIME_DIR*` variable set for GUI apps?
  - **N-2** Does `dlopen` of that path succeed from a hardened-runtime, Developer-ID signed binary with
    `disable-library-validation`?
- [ ] **Step 4: Ingest row.** **I-1** With the macOS firewall on, does a signed bundled `ffmpeg -listen 1`
  (RTMP) or SRT listener trigger an "accept incoming connections" prompt? Record the prompt text.
- [ ] **Step 5: Commit the outcomes rows only.**
  ```bash
  git add docs/superpowers/plans/2026-11-22-mac-outputs-ingest-browser-phase4-outcomes.md
  git commit -m "docs(mac-phase4): record boundary spike results"
  ```

---

## Task 1 (O3, native): Apple HEVC policy, a real POSIX encoder probe, HLS https verification (S, deps: none)

**Files:**
- Modify: `N/modules/EncoderPolicy.h:10-16` (comment), `:43-112`
- Modify: `N/modules/RtmpOutputSenderAdapter.cpp:567-610` (`ffmpegEncoderIsAvailable`, POSIX branch), `:1587`
- Modify: `N/modules/RtmpFfmpegArgs.h:162-168` (HLS branch)
- Modify: `scripts/build-macos-ffmpeg.sh:78` (encoder audit list)
- Modify: `native/tests/PosixChildProbe.cpp` (new mode `argv-to <file>`)
- Test: `native/tests/EncoderPolicyTest.cpp`, `native/tests/GpuVideoEncoderPolicyTest.cpp`,
  `native/tests/RtmpFfmpegArgsTest.cpp`, new `native/tests/PosixEncoderProbeTest.cpp`

**Interfaces:**
- Produces (Apple branch of `EncoderPolicy.h`):
  `isSupportedEncoder("hevc_videotoolbox") == true`;
  `codecHasSupportedHardwareEncoder("h265") == true`, `("av1") == false`;
  `preferredEncoderFor("h265", "videotoolbox"|"auto") == "hevc_videotoolbox"`;
  `encoderCandidatesFor("h265", *) == {"hevc_videotoolbox"}`, `("h264", *) == {"h264_videotoolbox"}`.
  Windows branches unchanged.
- Produces: `bool ffmpegEncoderIsAvailable(const std::string& executable, const std::string& encoderName)` on
  POSIX spawns through `posix_child::spawn` (guarded) with argv
  `-hide_banner -loglevel quiet -nostdin -f lavfi -i color=c=black:s=1280x720:r=30 -frames:v 30 -an -c:v <enc> -f null -`,
  waits at most 5000 ms (same as Windows), `stopGroup(pid, 200ms)` on timeout, and returns true only for
  exit status 0.

**Behaviour.** The policy comment's "Not on macOS" line becomes the Apple record: HEVC encode on Apple
through VideoToolbox, GPU-direct only (D1). Admission is **not** changed. The HLS https endpoint gets
certificate verification against `config.tlsCaFile` using whichever spelling makes ffmpeg 8.1.3's `hls`
muxer verify (try `-tls_verify 1 -ca_file <f>` before `-f hls` first, then `-http_opts` with the same
keys); Step 1's live check decides and the args test pins the winner.

- [ ] **Step 1: Write the failing tests.**
  - `EncoderPolicy.AppleOffersHevcThroughVideoToolbox` (`#if __APPLE__`): every Apple value in the
    Interfaces block, as literals.
  - `EncoderPolicy.WindowsHevcPolicyIsUnchanged` (`#if _WIN32`): `encoderCandidatesFor("h265","auto") == {"hevc_nvenc"}`,
    `("h264","auto") == {"h264_nvenc","h264_mf"}` (confirmatory on Mac; it runs in Windows CI).
  - `PosixEncoderProbe.TrueOnlyOnExitZero`: copy `corevideo-posix-child-probe` into a temp dir as `ffmpeg`;
    with `COREVIDEO_TEST_PROBE_MODE=exit`, arg `0` → `true`; arg `1` → `false`.
  - `PosixEncoderProbe.TimesOutAStubbornChildAndReapsIt`: mode `stubborn <pidfile>`; the call returns `false`
    in under 5600 ms and `kill(pid, 0)` is `ESRCH` within 1000 ms after. Fixture invariant: assert the
    pidfile exists and the pid is alive **before** the deadline, or the test is vacuous.
  - `PosixEncoderProbe.PassesTheEncoderAndANullSink`: mode `argv-to <file>`; the file contains the
    substrings `-c:v hevc_videotoolbox` and `-f null -` (exact argv, not a regex).
  - `PosixEncoderProbe.BundledFfmpegEncodesWithBothVideoToolboxEncoders`: self-skips unless
    `COREVIDEO_TEST_FFMPEG` names an ffmpeg; asserts `true` for `h264_videotoolbox` and `hevc_videotoolbox`.
  - `RtmpFfmpegArgs.HttpsHlsVerifiesTheCertificate`: endpoint `https://origin.example/live/p.m3u8`,
    `tlsCaFile=/etc/ssl/cert.pem` → the argument string contains the chosen verify spelling;
    `http://` endpoint → it does not.
  - `OutputSenderAdapter.AppleRefusesHevcOffTheGpuPath`: SRT sender, `videoCodec:"h265"`, frames carrying full
    Program BGRA and no encoder texture → `lastResultCode == "no-hardware-encoder"`. **Confirmatory** (rule 8):
    it was already red-proof before the flip because the policy said no; it pins D1 after the flip.
- [ ] **Step 2: Run them red.** `./native/build-metal/corevideo-native-tests --gtest_filter='EncoderPolicy.*:PosixEncoderProbe.*:RtmpFfmpegArgs.Https*'`
  Expected: the Apple policy, probe and HLS cases FAIL (probe returns true for everything today).
- [ ] **Step 3: Live HLS check.** Stand up a Python `http.server` subclass accepting PUT behind
  `ssl.wrap_socket` with a self-signed certificate; run the bundled ffmpeg's HLS push to it with each
  candidate spelling. Pick the spelling under which ffmpeg fails with `certificate verify failed` and a
  trusted local CA succeeds. Record it in the outcomes.
- [ ] **Step 4: Implement** the policy branches, the POSIX probe and the HLS verify args; add
  `hevc_videotoolbox` to the audit loop at `build-macos-ffmpeg.sh:78`.
- [ ] **Step 5: Run green**, same filter, plus `OutputSenderAdapter.*`, `RtmpCompatibility.*`,
  `StreamStartAdmission*`, and `scripts/build-macos-ffmpeg.sh --audit <cached ffmpeg>`.
- [ ] **Step 6: Mutations.** M1a: leave `codecHasSupportedHardwareEncoder` H.264-only on Apple → policy test red.
  M1b: POSIX probe returns `true` unconditionally (today's code) → `TrueOnlyOnExitZero` red.
  M1c: probe waits with no timeout → stubborn test red (hangs past 5600 ms). M1d: drop the HLS verify
  args → args test red. Report each.
- [ ] **Step 7: Commit.**
  ```bash
  git add native/src/modules/EncoderPolicy.h native/src/modules/RtmpOutputSenderAdapter.cpp \
    native/src/modules/RtmpFfmpegArgs.h scripts/build-macos-ffmpeg.sh native/tests/
  git commit -m "feat(mac-o3): HEVC VideoToolbox policy, real POSIX encoder probe, HLS https verify"
  ```

---

## Task 2 (O4a, native): `VideoToolboxGpuVideoEncoder` (M, deps: Task 0 rows E-1, E-2)

**Files:**
- Create: `N/modules/VideoToolboxGpuVideoEncoder.h`, `N/modules/VideoToolboxGpuVideoEncoder.mm`
- Modify: `N/modules/SharedGpuVideoEncoder.cpp:237-240` (factory), `native/CMakeLists.txt` (source + test
  under `APPLE AND COREVIDEO_WITH_RTMP_OUTPUT`; link `VideoToolbox CoreMedia CoreVideo IOSurface`)
- Test: create `native/tests/VideoToolboxGpuVideoEncoderTest.mm`

**Interfaces:**
- Consumes: `GpuVideoEncoder`, `GpuVideoEncoderConfig`, `GpuVideoEncoderFrame{iosurfaceId,width,height,frameNumber,publishedFrameNumber}`,
  `GpuEncodedChunk`, `configuredGopFrames(fps, seconds)` (`N/modules/GpuVideoEncoder.h`).
- Produces: `std::unique_ptr<GpuVideoEncoder> createVideoToolboxGpuVideoEncoder();` and, in
  `SharedGpuVideoEncoder.cpp`, `createSharedProgramGpuVideoEncoder()` builds its pool from
  `createMediaFoundationGpuVideoEncoder` on Windows and `createVideoToolboxGpuVideoEncoder` on Apple.
  `lastFailure()` values: `vt-codec-unsupported:<codec>`, `vt-session-create:<OSStatus>`,
  `vt-property:<KeyName>:<OSStatus>`, `vt-prepare:<OSStatus>`, `iosurface-lookup-failed`, `vt-encode:<OSStatus>`.

**Behaviour.**
- `start`: codec `h264` or `hevc` only. `VTCompressionSessionCreate` with
  `kVTVideoEncoderSpecification_RequireHardwareAcceleratedVideoEncoder = true`. Properties: `RealTime=true`,
  `AllowFrameReordering=false` (D2), `ExpectedFrameRate=fps`, `MaxKeyFrameInterval=configuredGopFrames(fps, kis)`,
  `MaxKeyFrameIntervalDuration=kis`, `AverageBitRate=bitrateKbps*1000`; `cbr` adds `ConstantBitRate` when
  row E-2 said it works, else `DataRateLimits=[bitrateKbps*125, 1.0]`. Profile: H.264 `high`/`auto`→High,
  `main`→Main, `baseline`→Baseline (all `_AutoLevel`); HEVC → `HEVC_Main_AutoLevel`. Colour:
  `ColorPrimaries`/`TransferFunction`/`YCbCrMatrix` = `ITU_R_709_2`. Returns true only after
  `VTCompressionSessionPrepareToEncodeFrames` succeeds.
- `submit`: never blocks the caller. Hands the frame to the encoder's own serial queue. The queue waits up to
  **34 ms** (the Windows keyed-mutex consumer timeout, gpu-direct-encode.md) for
  `publishedFrameNumber == frameNumber`, then `IOSurfaceLookup` → `IOSurfaceIncrementUseCount` →
  `CVPixelBufferCreateWithIOSurface` → encode; the use count is decremented in the output callback after the
  sample is released. A missing surface or a wait timeout skips the frame and increments
  `framesSkipped()`; at most 2 frames are in flight, a third is skipped and counted. PTS =
  `(frameNumber - firstFrameNumber) * 10'000'000 / fps` (100 ns); read
  `MediaFoundationGpuVideoEncoder.cpp`'s sample-time rule first and use the same epoch rule, citing the line.
- Output: AVCC/HVCC length-prefixed NAL units rewritten as Annex B (`00 00 00 01`); on keyframes the
  parameter sets from the format description lead the access unit (H.264 SPS, PPS; HEVC VPS, SPS, PPS);
  `keyframe` = sample lacks `kCMSampleAttachmentKey_NotSync`; `pts100ns/dts100ns/duration100ns` from the
  sample, `timingValid=true`.
- `requestKeyframe`: sets a flag the encode queue turns into `kVTEncodeFrameOptionKey_ForceKeyFrame` on the
  next picture; returns true.
- `healthy()`: false after `kVTInvalidSessionErr`, `kVTVideoEncoderMalfunctionErr`, or 30 consecutive encode
  errors; `submit` then returns false so the supervisor restarts the sender.
- `stop`: `CompleteFrames`, invalidate, drain the queue; idempotent; never touches `coreMutex`.

- [ ] **Step 1: Write the failing tests** (`VideoToolboxGpuVideoEncoder.*`, Apple; each creates a global BGRA
  IOSurface, fills it on the CPU, submits frames with `publishedFrameNumber` already equal, and decodes chunks
  with a `VTDecompressionSession` helper in the test file):
  1. `H264RoundTripDecodesTheEncodedGray`: 320x180@30, 2000 kbps, 12 frames of B=G=R=128. First chunk is a
     keyframe whose NAL types in order include 7, 8, then 5, each preceded by `00 00 00 01`. Decoded luma mean
     within 16 of **126** (BT.709 limited range of 128: 16 + 219·128/255).
  2. `HevcRoundTripHasParameterSetsAndNoReordering`: same with `hevc`; first chunk NAL types 32, 33, 34 then
     19 or 20; every chunk has `pts100ns == dts100ns`; luma within 16 of 126.
  3. `RequestKeyframeEmitsAnIdrOnTheNextPicture`: 30 fps, `keyframeIntervalSeconds=2` (GOP 60); submit 10
     frames, `requestKeyframe()`, submit frame 11 → the chunk for frame 11 is a keyframe and chunks 2..10 are
     not.
  4. `PtsAdvancesOneFramePeriod`: 30 fps → consecutive `pts100ns` differ by **333333 ± 1**.
  5. `BitrateIsHonoured`: 1280x720@30, `cbr` 4000 kbps, 300 frames of fixed-seed LCG noise rewritten every
     frame → total chunk bytes within **[4'000'000, 6'000'000]** (10 s at 4 Mbps = 5'000'000 bytes ±20%). This
     pins Windows defect #601 (encoder ignored its bitrate).
  6. `ColourIsBt709`: a solid BGRA red (0,0,255) frame decodes to luma within 6 of **63** (709: 16+219·0.2126;
     601 would be 82).
  7. `StartRefusesAv1WithItsReason`: `codec="av1"` → `start` false, `lastFailure() == "vt-codec-unsupported:av1"`.
  8. `MissingSurfaceIsASkipNotAFailure`: `iosurfaceId` of a released surface → `submit` true, no chunk,
     `framesSkipped() == 1`, `healthy()` true.
  Fixture invariants: frame count, fps and GOP are chosen so the keyframe positions in test 3 are known
  (60 > 11); test 5's noise must change every frame or the encoder legitimately under-spends.
- [ ] **Step 2: Run red.** `--gtest_filter='VideoToolboxGpuVideoEncoder.*'` → link/compile failure, then FAIL.
- [ ] **Step 3: Implement** the class and the platform factory.
- [ ] **Step 4: Run green** plus `SharedGpuVideoEncoder.*` and `GpuVideoEncoderPolicy.*` on Mac.
- [ ] **Step 5: Mutations.** M2a `AllowFrameReordering=true` → test 2 red. M2b no parameter sets on keyframes →
  tests 1 and 2 red. M2c ignore the keyframe flag → test 3 red. M2d drop `AverageBitRate`/CBR → test 5 red.
  M2e matrix `ITU_R_601_4` → test 6 red. M2f leave AVCC length prefixes → test 1 red. M2g fail the encoder on
  a missing surface → test 8 red.
- [ ] **Step 6: Commit.** `git commit -m "feat(mac-o4): VideoToolbox GPU video encoder behind GpuVideoEncoder"`

---

## Task 3 (O4b, native): Metal encoder-texture export; the sender accepts an IOSurface (M, deps: Task 2 interface, Task 0 row E-1)

**Files:**
- Modify: `N/compositor/MetalCompositorAdapter.mm` (new ring beside `harvestAndKickFullResTap`, `:524-567`;
  `encoderExporting()`, `encoderExportShedFrames()` overrides)
- Modify: `N/modules/Interfaces.h` (`CompositorRenderPlan::encoderTextureExport`, comment on
  `ProgramFrame::encoderSharedTexture` `:406-409` naming the IOSurface)
- Modify: `N/core/MediaCore.cpp:6086-6120` (set the flag)
- Modify: `N/modules/RtmpOutputSenderAdapter.cpp:2115` (`frameHasEncoderTexture`), `:2890-2904`
  (`submitFrameToGpuEncoder`)
- Test: create `native/tests/MetalEncoderExportTest.mm`; extend `native/tests/MediaCoreCommandTest.cpp`
  (sender cases)

**Interfaces:**
- Produces: `bool CompositorRenderPlan::encoderTextureExport` (default false; D3D11 ignores it).
  MediaCore sets it true iff `outputDestinations_` contains `rtmp`, `srt` or `hls`.
- Produces: on Metal, while the flag is set, every rendered tick sets
  `frame.encoderSharedTexture = {iosurfaceId, width, height, format "B8G8R8A8_UNORM", frameNumber, publishedFrameNumber}`,
  where `publishedFrameNumber` (one shared atomic per ring slot) is stored in the blit's completion handler.
  `encoderExporting()` returns whether the last tick exported. A tick whose three slots are all in use
  (`IOSurfaceIsInUse`) exports nothing and increments `encoderExportShedFrames()` by one.
- Produces: the sender treats `!sharedHandleHex.empty() || iosurfaceId != 0` as "has encoder texture" in both
  places and copies `iosurfaceId` into `GpuVideoEncoderFrame`.

**Behaviour.** The blit `program_ → ring[slot]` is its own command buffer committed right after the Program
pass on fast and full ticks alike (the stream is a 60 fps output). It never waits on the GPU. Ring
allocation happens on first use off the render thread, the vcam tap's preparation rule
(`MetalCompositorAdapter.mm:631-637`); until ready, ticks do not export and are not counted as sheds.
Use the global-surface flag only if row E-1 said in-process lookup needs it.

- [ ] **Step 1: Write the failing tests.**
  - `MetalEncoderExport.ExportsTheComposedFrameIntoTheRing`: plan with a solid background
    (B=40,G=120,R=200), `encoderTextureExport=true`; render until the ring is ready (bounded 50 ticks);
    `iosurfaceId != 0`, width/height equal the plan; poll `publishedFrameNumber == frameNumber` (≤100 ms);
    lock the surface: centre pixel equals (40,120,200) ±1.
  - `MetalEncoderExport.NeverRewritesAnInUseSurface`: take tick N's id and `IOSurfaceIncrementUseCount` it;
    render 6 ticks: no exported id equals it. Increment the other two as they appear: the next tick exports
    `iosurfaceId == 0` and `encoderExportShedFrames()` rose by exactly 1. Decrement all: export resumes.
    Fixture invariant: ring size 3 is the plan's value; assert three distinct ids were seen before the
    all-in-use step.
  - `MetalEncoderExport.NoExportWhenNoStreamDestination`: flag false → `iosurfaceId == 0`,
    `encoderExporting() == false`.
  - `OutputSenderAdapter.IosurfaceEncoderTextureChoosesGpuDirect`: through the existing test factory seam
    (`createRtmpOutputSenderForTest`, plus a `setGpuVideoEncoderFactoryForTest` hook if none exists), a fake
    encoder that records submits; frames with `encoderSharedTexture.iosurfaceId = 77` → the path chosen is
    GPU-direct and the fake saw `iosurfaceId == 77`.
  - `MediaCore.EncoderTextureExportFollowsStreamDestinations`: `start-program-output` with `["recording"]`
    → flag false; with `["recording","srt"]` → true (read from the plan handed to a recording compositor stub).
- [ ] **Step 2: Run red** (`MetalEncoderExport.*:OutputSenderAdapter.Iosurface*:MediaCore.EncoderTexture*`).
- [ ] **Step 3: Implement.**
- [ ] **Step 4: Run green**, plus the full Metal suites `MetalCompositor*` and `MetalZeroCopy*`.
- [ ] **Step 5: Mutations.** M3a sender checks only `sharedHandleHex` → sender test red. M3b skip the in-use
  check → ring test red. M3c export regardless of the flag → no-export test red. M3d store
  `publishedFrameNumber` at commit instead of completion → **expect it to stay green on a fast GPU**; if it
  does, say so: the 34 ms wait in Task 2 is the guard, proven only by the Task 6 frame-identity gate.
- [ ] **Step 6: Commit.** `git commit -m "feat(mac-o4): Metal encoder IOSurface ring; sender accepts IOSurface textures"`

---

## Task 4 (O4c, native): POSIX bitstream queue, writer thread, TS envelope and backpressure (M, deps: none; integrates with Tasks 2-3)

**Files:**
- Modify: `N/modules/RtmpOutputSenderAdapter.cpp`: the `_WIN32` blocks at `:2147-2161` (encoder sink),
  `:2177-2230` (activate/stop), `:2233-2297` (queue telemetry, GOP discard), `:2330-2409`
  (`observeStreamBackpressure`), the writer loop ending `:2767`, `:2771-2872` (`writeBitstreamToFfmpeg`),
  `:2891-2893`, and the pipe-write telemetry fields in `resetBackpressureForNewRun` (`:2322-2326`).
- Modify: `native/CMakeLists.txt` so `BitstreamQueueOverflowTest.cpp` and `StreamBackpressureRateProbeTest.cpp`
  build and run on Apple (today they skip without `C:\ffmpeg\bin`); `native/tests/PosixChildProbe.cpp`
  (mode `copy-stdin-to <file>`)
- Test: extend `native/tests/BitstreamQueueOverflowTest.cpp`; create `native/tests/BitstreamQueuePosixTest.cpp`

**Interfaces:**
- Consumes: `IBitstreamQueueTestAccess` (`enqueueBitstreamChunkForTest`, `offerBitstreamChunkForTest`,
  `bitstreamQueueSnapshotForTest`, `setBackpressureObservationForTest`), `EncodedVideoTransportStream`
  (portable), `discardableBacklogForArrival` and `StreamBackpressurePolicy` (unchanged).
- Produces: on every platform the GPU-direct encoder sink calls `enqueueBitstream(chunk)`; one writer thread
  per FFmpeg run drains the queue through the TS envelope for `hevc` and timestamped `h264`, exactly the
  Windows loop. The published `backpressure` node and its fields are unchanged.

**Behaviour.** This also fixes a latent POSIX defect: the args already declare a timestamped TS video input
(`config.timestampedVideoInput`, `:1613-1616`) while the POSIX sink writes raw Annex B
(`:2155-2160`), so any POSIX GPU-direct stream today would mux garbage. POSIX writes: `ffmpegStdinFd_` is
set `O_NONBLOCK`; the writer loops on `poll({stdin POLLOUT, stopPipe POLLIN}, 100 ms)`. Stop writes one byte
to the stop pipe, joins the writer, and only then closes stdin (the Windows `CancelSynchronousIo` contract).
The pipe-write telemetry (`inFlightWriteMs`, `maxWriteMs`, `slowWriteCount`) is filled on POSIX too. Queue
bound stays 60 chunks / 2 MiB plus one in flight; overflow and a broken pipe fail the sender to its
supervisor, as on Windows.

- [ ] **Step 1: Write the failing tests** (POSIX; the fake ffmpeg is the probe binary copied as `ffmpeg` under
  `COREVIDEO_FFMPEG_BIN_DIR`, mode chosen by `COREVIDEO_TEST_PROBE_MODE`):
  1. `BitstreamQueuePosix.WritesTheTimestampedTsEnvelope`: mode `copy-stdin-to <file>`; enqueue a keyframe chunk
     with `pts100ns = 1'000'000` then 4 deltas → the file starts with sync byte `0x47` at offsets 0 and 188, and
     the first video PES carries PTS **9000** (1'000'000 × 9 / 1000, 90 kHz).
  2. `BitstreamQueuePosix.WedgedReaderShedsGopTailsWithoutRebuildingTheEncoder`: mode `stubborn` (never reads
     stdin); offer 300 chunks at 60/s (injected clock) with a keyframe every 60 → snapshot
     `backpressure.discardedChunks > 0` and the fake encoder factory's `start` count stays **1**.
  3. `BitstreamQueuePosix.StopReturnsWithinTwoSecondsOnAWedgedPipe`: same wedged reader with the pipe full →
     `stop()` returns in under 2000 ms and the fake ffmpeg pid is gone (guard) within 1000 ms after.
  4. The existing `BitstreamQueueOverflow*` and `StreamBackpressure*` cases run on Mac. Read each test's
     doc comment first (rule 5): any Windows-only assumption (pipe size, `WriteFile`) is adapted, not deleted,
     and the report lists each adaptation.
- [ ] **Step 2: Run red** (`BitstreamQueuePosix.*:BitstreamQueueOverflow*:StreamBackpressure*`).
- [ ] **Step 3: Implement** by lifting the Windows blocks behind a small platform write primitive
  (`writeAllOrStop(const uint8_t*, size_t) -> WriteOutcome{written, stopped, broken}`), not by copying the loop.
- [ ] **Step 4: Run green**, plus `OutputSenderAdapter.*`, `FfmpegChildRetirement.*`, `OutputDestinationSupervisor*`.
  Windows CI must stay green (the lift touches shared code; no Windows behaviour changes).
- [ ] **Step 5: Mutations.** M4a sink writes from the encoder callback (today's POSIX code) → test 3 red.
  M4b skip TS packetizing on POSIX → test 1 red. M4c no stop pipe in `poll` → test 3 red. M4d
  `observeStreamBackpressure` stays Windows-only → test 2 red.
- [ ] **Step 6: Commit.** `git commit -m "feat(mac-o4): bitstream queue, TS envelope and backpressure on POSIX"`

---

## Task 5 (O4d, native): shared AAC on Apple and the one-clock TS mux on POSIX (M, deps: Task 4)

**Files:**
- Modify: `N/modules/ProgramAacEncoder.cpp` (Apple branch of `start/stop/encode/running/acceptedSamples`)
- Modify: `N/modules/RtmpOutputSenderAdapter.cpp:1778-1800` (`unifiedTsInput_`, `muxInputMode_`,
  stream-clock fields), `:991-1005` (`slotClock_`), `:1605-1607` (`audioInTransportStream`), `:644-660`
  (`submitEncodedAudio` transport branch)
- Modify: `native/CMakeLists.txt` (`ProgramAacEncoderTest.cpp` on Apple; link `AudioToolbox`)
- Modify: `scripts/validate-av-clap.mjs` (accept `--build-dir native/build-metal` on macOS; resolve
  ffprobe on PATH)
- Test: `native/tests/ProgramAacEncoderTest.cpp`

**Interfaces:**
- Consumes: `ProgramAacEncoder` (`N/modules/ProgramAacEncoder.h`, unchanged), `ProgramStreamClock`,
  `ProgramSlotClock`, `EncodedVideoTransportStream`.
- Produces: on Apple, `start()` returns true and `encode()` emits `ProgramAacPacket{adts, pts100ns, sampleIndex}`
  for every 1024 accepted samples; `ModuleComposition`'s `sharedAac_` therefore runs on Mac and POSIX
  senders take `mux-input=unified-ts` whenever GPU-direct, shared AAC and real audio are on.

**Behaviour.** `AudioConverterNew` from interleaved Float32 48 kHz stereo to `kAudioFormatMPEG4AAC` (LC),
`kAudioConverterEncodeBitRate = 160000`, 1024 frames per packet; a 7-byte ADTS header (profile LC,
sampling index 3, channel configuration 2) is written by us per packet. The converter's priming
(`kAudioConverterPrimeInfo.leadingFrames`) is subtracted so the first packet's `pts100ns` is 0 and
`sampleIndex` counts input samples, the same meaning the MF path documents ("packet time is the exact count
of accepted PCM frames").

- [ ] **Step 1: Read `ProgramAacEncoderTest.cpp` and the MF implementation** and list which existing cases
  must now run on Apple. Any case that asserts an MF-specific detail is adapted with a comment; none is deleted.
- [ ] **Step 2: Write the failing tests** (Apple):
  - `ProgramAacEncoder.AppleAdtsHeadersAreLc48kStereo`: one second of silence → every packet's first 7 bytes
    parse as sync `0xFFF`, profile 1 (LC), sampling index 3, channel config 2, frame length == packet size.
  - `ProgramAacEncoder.AppleSampleIndexAdvancesBy1024`: consecutive packets' `sampleIndex` differ by 1024;
    `pts100ns` differ by **213333 ± 1**.
  - `ProgramAacEncoder.AppleDecodesToTheInputTone`: 2 s of a 1 kHz sine at amplitude 0.5 → decoded (AAC→PCM
    `AudioConverter` in the test) RMS within 10% of **0.3536**, and the strongest 10 Hz FFT bin is at 1000 Hz.
  - `OutputSenderAdapter.PosixGpuDirectUsesUnifiedTs`: fake encoder + shared AAC on → the sender snapshot's
    `streamClock.muxInput == "unified-ts"` and no fd 3 audio pipe is created (fake ffmpeg mode `argv-to`:
    argv has no `pipe:3`).
- [ ] **Step 3: Run red.**
- [ ] **Step 4: Implement**; lift the stream-clock and slot-clock fields off `_WIN32`.
- [ ] **Step 5: Run green**, then the content gate on a quiet machine:
  `node scripts/validate-av-clap.mjs --build-dir native/build-metal --rtmp-local --shared-aac-gate` five
  times. Pass = every run's RTMP-vs-Program skew within **one 60 fps frame (16.7 ms)**, the gate's own budget.
  Windows measured 0.3-1.7 ms; record all five numbers (gpu-direct-encode.md: "re-run an A/V gate many
  times before calling it green").
- [ ] **Step 6: Mutations.** M5a no priming subtraction → the clap gate shows a fixed offset of about 44 ms
  (2112 samples) and fails; record it. M5b ADTS profile 2 → header test red. M5c `unifiedTsInput_` stays
  Windows-only → sender test red.
- [ ] **Step 7: Commit.** `git commit -m "feat(mac-o4): AudioToolbox shared AAC; one-clock TS mux on POSIX"`

---

## Task 6 (O4e, native + QA): integrate, publish the evidence, prove 1080p60 (M, deps: Tasks 1-5)

**Files:**
- Modify: `N/modules/Interfaces.h` (`OutputSenderState::encodePath{path, reason, codec}`),
  `N/core/CoreStateCapture.cpp` (sender rows), `N/modules/RtmpOutputSenderAdapter.cpp:2104-2121`
- Modify: `N/core/MediaCore.cpp:255-280` (new intrinsic capability `gpu-direct-stream-encode`)
- Modify: `scripts/mac-show-drill.py` (new `--stream-srt PORT`), `scripts/validate-gpu-encode.mjs`
  (`--build-dir`; macOS ffmpeg/ffprobe lookup; process listing via `pgrep` instead of `Get-CimInstance`)
- Test: `native/tests/MediaCoreCommandTest.cpp`, `native/tests/CoreStateCaptureTest.cpp` (or the suite that
  pins sender rows)

**Interfaces:**
- Produces: every sender row carries `encodePath: {"path": "gpu-direct"|"cpu-fallback", "reason": <GpuEncodePathPolicy reason>, "codec": "h264"|"hevc"}`
  once a start decided it (null before). Additive on both platforms.
- Produces: `profile.capabilityStates["gpu-direct-stream-encode"]` = `available` when the platform encoder
  factory is non-null and the compositor implements the encoder export; detail names the implementation
  (`videotoolbox` / `media-foundation`). Task 7 reads it.
- Produces: `mac-show-drill.py --stream-srt 9710` starts an SRT listener receiver (Homebrew ffmpeg as the
  **receiver only**, test tooling) and adds the core's `srt` destination at 1080p60 to the drill.

- [ ] **Step 1: Write the failing tests.** `OutputSenderAdapter.PublishesTheChosenEncodePath` (fake encoder →
  `encodePath.path == "gpu-direct"`, `reason == "gpu-direct"`; no encoder texture →
  `"cpu-fallback"`/`"no-encoder-texture"`); `MediaCore.AdvertisesGpuDirectStreamEncode` (Apple Metal build →
  `available`; stub build → `omitted`).
- [ ] **Step 2: Run red, implement, run green.**
- [ ] **Step 3: Gates on a quiet machine** (record hardware, macOS version, duration, each number):
  1. `node scripts/validate-gpu-encode.mjs --build-dir native/build-metal --seconds 60 --fps 60 --codec h264`
     and `--codec hevc`: GPU path taken (snapshot `encodePath`), received ≥ **58 fps**, sink `speed ≥ 0.97x`.
  2. `--check-bitrate --bitrate 6`: received bitrate within the harness's tolerance.
  3. `--slow-sink --sink-rate 0.85` and `--slow-sink --burst-sink`: shedding visible in `backpressure`, zero
     encoder rebuilds in the core log, the stream decodes after the cut.
  4. `--force-raw`: raw path still streams (30 fps row).
  5. `python3 scripts/mac-show-drill.py --seconds 40 --load 8 --stream-srt 9710` before (main) and after
     (this branch): per-frame Program delivery has **zero** underruns or missed deadlines in the after run;
     the render-time delta of the encoder blit is reported.
  6. Frame identity: the Program pixel verdict (`scripts/qa/source-picture-verdict.py`) on the received SRT
     stream shows no repeat/reorder the pipeline caused.
- [ ] **Step 4: Mutations.** M6a publish `encodePath` from the desired path instead of the started path →
  the fallback case red. M6b capability `available` without the export → capability test red.
- [ ] **Step 5: Commit.** `git commit -m "feat(mac-o4): publish encode path and capability; Mac GPU stream gates"`

---

## Task 7 (O4f, shell): stream codec, fps and Enhanced RTMP; recipe line; backpressure readout (M, deps: Task 6)

**Files:**
- Modify: `M/OutputProfiles.swift:10-18,80-131` (comment; `StreamEncoderSettings` gains `codec`, `fps`,
  `allowEnhancedRtmp`)
- Modify: `M/StreamDestinations.swift:145-185` (rows take codec/fps/E-RTMP from the settings)
- Modify: `M/SettingsPane.swift:103-182` (Outputs card), `M/StudioFooter.swift` (recipe line, stream flyout),
  `M/ShellPrefs.swift` (additive fields)
- Test: create `M/ShellTestsPhase4Encode.swift` (cases `encode/*`)

**Interfaces:**
- Consumes: `profile.capabilityStates["gpu-direct-stream-encode"]` (Task 6), sender `encodePath` and
  `backpressure` (Tasks 4, 6), `canvasFps`.
- Produces: `StreamEncoderSettings.codec` ∈ {`h264`,`h265`} (`av1` listed unavailable with the sentence
  "AV1 has no hardware encoder on Apple Silicon."), `fps` ∈ {24,30,50,60} filtered to ≤ `canvasFps`,
  `allowEnhancedRtmp: Bool`; `ownedKeys` adds `videoCodec`, `fps`, `allowEnhancedRtmp`; SRT rows carry
  `videoCodec` and `fps` too (Windows `BuildStreamDestinationSettings` puts `VideoCodec`/`Fps` on every row).

**Behaviour.** Layout is the Windows Streaming block (`W/Views/ProductionSettingsWindow.xaml:335-420`): Render
size (text "Follows the canvas", D5), FPS, Codec, Bitrate Mbps, Hardware encoder (text "VideoToolbox"),
Keyframe interval, Audio kbps, Rate control, H.264 profile, B-frames, then the check box
**"Enhanced RTMP (H.265 / AV1)"**, then the summary line in the Windows format
`"{size} - {fps} fps - {codec} - {bitrate} Mbps - {encoder}"` (`W/ViewModels/StudioViewModel.cs:1895`).
While the capability is not `available`, FPS shows only values ≤ 30 and Codec only H.264, each with the
sentence "60 fps and H.265 need GPU encoding, which this core does not report." H.265 with an RTMP row and
E-RTMP off is refused before start with the core's own sentence (`RtmpCompatibility`), never silently
downgraded. The footer recipe line reads the live rows, replacing "Follows the canvas at 30 fps". A sender
with `backpressure.discardedChunks > 0` shows "Stream is shedding frames to hold quality" with the divisor
in Health (spec §5.1); a row with `encodePath.path == "cpu-fallback"` shows "CPU encoding: <reason>".

- [ ] **Step 1: Write the failing cases.** `encode/settings-round-trip-and-clamp` (fps 60 with canvas 30 →
  30; unknown codec → h264); `encode/rows-carry-codec-fps-and-ertmp` (rtmp row `videoCodec:"h265"`,
  `fps:60`, `allowEnhancedRtmp:true`; srt row has `videoCodec` and `fps`, and still no keyframe/B-frame keys);
  `encode/60-and-h265-hidden-without-capability`; `encode/h265-rtmp-without-ertmp-refused-before-start`
  (sentence equals `RtmpCompatibility`'s, copied from `N/modules/RtmpCompatibility.h`);
  `encode/recipe-line-reads-the-rows`; `encode/shedding-and-fallback-readouts`.
- [ ] **Step 2: Run red, implement, run green;** self-check PASS; lint green; offscreen render of the Settings
  tab (`COREVIDEO_SHELL_TAB_SNAPSHOT`) compared against the Windows block order.
- [ ] **Step 3: Mutations.** M7a picker ignores the capability → hidden case red. M7b SRT row omits
  `videoCodec` → rows case red. M7c recipe from a literal → recipe case red.
- [ ] **Step 4: Commit.** `git commit -m "feat(mac-o4): stream codec/fps/Enhanced RTMP pickers gated on GPU encode"`

---

## Task 8 (N1a, native + release): NDI output build and Apple runtime discovery (S, deps: Task 0 rows N-1, N-2)

**Files:**
- Modify: `N/modules/NdiOutputSenderAdapter.cpp:173-197` (`ndiLibraryCandidates`, Apple branch), `:372`
  (Mac sentence)
- Modify: `scripts/release-macos.sh:179-188`, `.github/workflows/ci.yml:298-308` (`-DCOREVIDEO_WITH_NDI_OUTPUT=ON`)
- Modify: `M/StudioFooter.swift:93-97` comment (it states the release core has NDI off)
- Test: create `native/tests/NdiRuntimeCandidatesTest.cpp`

**Interfaces:**
- Produces: `std::vector<std::string> ndiLibraryCandidates(const std::function<const char*(const char*)>& getenv)`
  (injectable for the test; the existing zero-argument call passes `std::getenv`). Apple order:
  `$NDI_RUNTIME_DIR_V6/libndi.dylib`, `$NDI_RUNTIME_DIR_V5/libndi.dylib`, `$NDI_RUNTIME_DIR/libndi.dylib`,
  each path row N-1 found (expected to include `/usr/local/lib/libndi.dylib` and
  `/Library/NDI SDK for Apple/lib/macOS/libndi.dylib`; use what the spike observed), then the leaf
  `libndi.dylib`. An unset or empty variable adds nothing.
- Produces: the missing-runtime warning on Apple: "NDI output needs the NDI runtime. Install NDI Tools from
  ndi.video, then restart CoreVideo Pro." `lastResultCode` stays `runtime-missing`.

- [ ] **Step 1: Write the failing tests.** `NdiRuntimeCandidates.AppleOrderAndEnvironment` (fake getenv with
  only `NDI_RUNTIME_DIR_V6=/opt/x` → first entry `/opt/x/libndi.dylib`, last entry `libndi.dylib`, no empty
  path); `NdiRuntimeCandidates.MissingRuntimeSentenceNamesNdiTools` (sender with an empty candidate set →
  `warning` equals the sentence above, `lastResultCode == "runtime-missing"`).
- [ ] **Step 2: Run red, implement, run green;** full `Ndi*` suites on Mac (`NdiPixelConvert`, `NdiSourceName`).
- [ ] **Step 3: Live (owner hardware):** with NDI installed, the release-configured core reports capability
  `ndi-output` = `available`; without it, `omitted` with detail `ndi-runtime-missing`.
- [ ] **Step 4: Mutations.** M8a drop the environment entries → order test red. M8b Windows sentence on Apple →
  sentence test red.
- [ ] **Step 5: Commit.** `git commit -m "feat(mac-n1): NDI output in the Mac core; Apple runtime discovery"`

---

## Task 9 (N1b, shell + control): NDI output as its own output (M, deps: Task 8)

**Files:**
- Create: `M/NdiOutput.swift` (model, validation, status)
- Modify: `M/AppModel.swift:4288-4343` (`startProgramOutput`, `toggleStreaming`, the recording toggle and the
  quit path), `:1773-1830` (`onConnected`)
- Modify: `M/StudioFooter.swift:456-470` (NDI tile and flyout), `M/SettingsPane.swift` (new NDI card),
  `M/ShellPrefs.swift`
- Modify: `M/ControlSurface+Production.swift`, `M/ControlStateProjection.swift`, `contracts/control.platforms.json:27-29,129`,
  regenerate `M/ControlManifest.generated.swift` (`npm run contract:generate`)
- Test: create `M/ShellTestsPhase4Ndi.swift` (cases `ndi/*`)

**Interfaces:**
- Produces: `struct NdiOutputSettings: Codable, Equatable { var enabled = false; var programName = "CoreVideo Pro Program"; var groupName = "public" }`
  (defaults from `W/ViewModels/StudioViewModel.StreamDestinations.cs:66-67`).
- Produces: `enum NdiOutputPolicy { static func validate(programName: String, coreHasNdiOutput: Bool?) -> String?; static func status(enabled: Bool, senders: [OutputSenderRow], programName: String, refusalReason: String?) -> String; static func destinationSettings(_ s: NdiOutputSettings, streamFps: Int) -> [JSONObject] }`.
  `validate` is the Windows `ValidateNdiOutput` minus the graphics-key clause (out of scope on Mac):
  "Configure an NDI program name before turning on NDI output." /
  "NDI output is not available in this core build (missing ndi-output)."
  `status` is `StudioOutputTelemetry.NdiStatus` (`MC/Services/StudioOutputTelemetry.cs:24-36`) without the
  ` + key` suffix: `Off`, `Unavailable · {reason}`, `Starting`, `Live · {name}`, `Warning · {first of warning,lastError,runtimeDetail,lastResultCode}`.
- Produces: `AppModel.setNdiOutput(_ on: Bool) -> String?` (refusal sentence or nil) and
  `startProgramOutput(recording:streaming:)` now appends `ndi` to `destinations` and its row to
  `destinationSettings` whenever NDI is on. The command sent when an operator stops streaming or recording is
  `stop-encoder-session` **only** when recording, streaming and NDI are all off; otherwise it is the
  full-state `start-program-output` for what remains.

**Behaviour.** Windows rules (`docs/reference/stream-destination-control.md` "NDI output"): independent of
Streaming/Recording; on/off persisted, default off; format = the stream profile (Mac: canvas size, stream fps);
a failure while on leaves NDI on and shows `Warning · …`; NDI never starts the footer ELAPSED clock, never
counts as streaming, never prompts on quit; a stopped core shows `Starting`, never a stale `Live`. Layout:
the footer tile next to Virtual camera (already placed, `StudioFooter.swift:456`) becomes live, its flyout
holds "NDI output", the status line, Program name and Group (commit on Return/blur, Windows
`OnNdiTextBoxKeyDown`), and the line "NDI graphics key is not available on Mac." in place of the key rows.
Settings gets the Windows NDI card (`W/Views/ProductionSettingsWindow.xaml:451-494`): title "NDI", switch
"NDI output", status, then a two-column row Program name | Group; the key row is replaced by the same
sentence. Control: `transport.ndi.set {on}` and `transport.ndi.toggle` become `supported`; state fields
`ndiOutputOn`, `ndiOutputStatus`; OSC `/cvp/state/ndi` already maps (`M/ControlOscRouter.swift:46`);
feedback field `ndi` becomes `supported`. `transport.ndi.graphicsKey.set` stays `notApplicable`.

- [ ] **Step 1: Write the failing cases** (port the Windows tables from `MC.Tests/NdiOutputTelemetryTests.cs`,
  `W.Tests/NdiOutputValidationTests.cs`, `NdiOutputActionTests.cs`, `NdiOutputDestinationTests.cs`; every
  expected string is a literal copied from those files):
  `ndi/status-table-matches-windows`, `ndi/validation-matches-windows`,
  `ndi/destination-row-shape` (`id:"ndi"`, `label:"NDI"`, `ndiName`, `ndiGroup`, `fps`),
  `ndi/toggle-on-alone-sends-start-with-only-ndi`, `ndi/stream-stop-keeps-ndi` (Review Focus 2: NDI on,
  streaming, not recording; stop streaming → the sent command is `start-program-output` with
  `destinations == ["ndi"]`, never `stop-encoder-session`), `ndi/all-off-sends-stop`,
  `ndi/rearmed-on-core-generation` (Review Focus 1: NDI on, simulate a new core via `onConnected()` → a
  `start-program-output` containing `ndi` is sent), `ndi/stopped-core-reads-starting`,
  `ndi/not-streaming-in-footer-or-telemetry`, `ndi/control-set-and-toggle` (drives `ControlSurface` through
  its real entry point, rule 10), `ndi/osc-feedback-flag`.
- [ ] **Step 2: Run red, implement, run green;** `npm run contract:check`; Companion module tests in a scratch
  copy (the manifest count changes; Phase 2 outcomes: A1 broke `mac.test.ts` this way).
- [ ] **Step 3: Live (owner hardware):** a second machine running NDI Studio Monitor on the same LAN sees
  "CoreVideo Pro Program" with Program audio; stop/start streaming leaves it running; kill the core → the tile
  reads Starting, then Live again. Record whether a Local Network prompt appeared (row B-3's question for NDI).
- [ ] **Step 4: Mutations.** M9a stop path keeps today's `isRecordingActive ? … : stop` → stream-stop case red.
  M9b no re-arm in `onConnected` → rearm case red. M9c status from `lifecycle` instead of Windows' `status` →
  the table's `Live` row red (this is deliberate parity; note it).
- [ ] **Step 5: Commit.** `git commit -m "feat(mac-n1): NDI output toggle, footer tile, Settings card, control actions"`

---

## Task 10 (I1a, native + release): network ingest under the child guard (M, deps: Task 0 row I-1)

**Files:**
- Modify: `N/modules/PosixChildProcess.h:57-76,151+` (`SpawnRequest` gains `int stdoutFd = -1; int stderrFd = -1;`)
- Modify: `N/modules/SrtIngestCaptureAdapter.cpp:782-797` (`killProcess`), `:1189-1266` (POSIX spawn)
- Modify: `scripts/build-macos-ffmpeg.sh` (audit: demuxers `mpegts flv`, decoders `h264 hevc aac`,
  muxers `rawvideo f32le`), `scripts/release-macos.sh`, `.github/workflows/ci.yml` (ingest gates on)
- Test: `native/tests/PosixChildProcessTest.cpp`, create `native/tests/NetworkIngestLoopbackTest.cpp`

**Interfaces:**
- Produces: `SpawnRequest::stdoutFd`/`stderrFd`: when ≥ 0 the child's fd 1/2 is that descriptor (the paths
  are ignored); otherwise today's path behaviour.
- Produces: the ingest decoder child is spawned with `posix_child::spawn` (video pipe → fd 1, stderr pipe →
  fd 2, audio pipe → fd 3), recorded pid = guard pid, killed with `stopGroup(pid, 0ms)` (the Windows path
  terminates immediately; keep the immediacy).

- [ ] **Step 1: Write the failing tests.**
  - `PosixChildProcess.StdoutFdReachesTheChild`: probe mode `argv-to /dev/stdout` with `stdoutFd` = a pipe →
    the parent reads the argv text from the pipe.
  - `NetworkIngestLoopback.SrtListenerDecodesAPublisherIntoFrames` (self-skips unless `COREVIDEO_TEST_FFMPEG`):
    configure one SRT ingest source `listener 127.0.0.1:<free port>`; publish with the same ffmpeg
    `-f lavfi -i testsrc2=s=640x360:r=30 -f lavfi -i sine=f=1000 -c:v h264_videotoolbox -c:a aac -f mpegts srt://127.0.0.1:<port>`
    for 6 s → the device reports `connected`, ≥ 60 frames at 640x360, and captured audio RMS > 0.05.
  - `NetworkIngestLoopback.RtmpListenerDecodesAPublisher`: same with `rtmp://127.0.0.1:<port>/live/test`.
  - `NetworkIngestLoopback.CoreSigkillLeavesNoDecoder`: run the core binary as a child with one SRT listener
    configured, SIGKILL it → no process whose executable is the test ffmpeg remains after 3 s.
  - `NetworkIngestLoopback.StopReapsTheGuardAndDecoder`.
- [ ] **Step 2: Run red, implement, run green;** existing `SrtFfmpegArgs.*`, `SrtIngestHealthPolicy*`,
  `CaptureIngest*` on Mac. Then `--audit` on the cached ffmpeg.
- [ ] **Step 3: Mutations.** M10a spawn with `fork/execvp` (today) → SIGKILL test red. M10b ignore `stdoutFd`
  → fd test red. M10c drop `flv` from the audit list and rebuild with `--disable-demuxer=flv` in a scratch
  build → audit fails (proves the audit line bites).
- [ ] **Step 4: Commit.** `git commit -m "feat(mac-i1): SRT/RTMP ingest under the child guard; ingest gates on"`

---

## Task 11 (I1b, shell): SRT and RTMP ingest catalogs on Sources (M, deps: Task 10)

**Files:**
- Create: `M/NetworkIngest.swift` (models, validation, wire, status), `M/NetworkIngestViews.swift`
- Modify: `M/SourcesTable.swift:104-122` (page layout), `M/SourcesWorkspace.swift:10-56` (types),
  `M/AppModel.swift` (sync batch, `onConnected`), `M/KeychainStore.swift` usage
- Test: create `M/ShellTestsPhase4Ingest.swift` (cases `ingest/*`)

**Interfaces:**
- Produces: `struct SrtIngestSource { let number: Int; var mode = "listener"; var host = "0.0.0.0"; var port: String; var latencyMs = "120"; var streamId = ""; var passphrase = "" }`
  with `id = "srt-source-%02d"`, `deviceId = "srt-ingest-%02d"`, `name = "SRT \(number)"`, default
  `port = 10000 + number - 1`; max **8**; one source exists at launch (Windows `StudioViewModel.cs:29,822-824`).
- Produces: `struct RtmpIngestSource { let number: Int; var url = "" }` with `id = "rtmp-source-%02d"`,
  `deviceId = "rtmp-ingest-%02d"`, `name = "RTMP \(number)"`; max **8**; `RtmpIngestSourcePolicy.isValid(_:)` and
  `listenerPort(_:)` ported line for line from `W/Models/RtmpIngestSource.cs:34-56`; the wire keeps only valid
  URLs, first per listener port (Windows `DistinctBy`).
- Produces: `configure-srt-ingest-sources {sources:[{id,deviceId,name,mode,host,port,latencyMs,streamId,passphrase}]}`
  and `configure-rtmp-ingest-sources {sources:[{id,deviceId,name,url}]}` in every production sync batch
  (Windows `MediaCoreCommandBuilder.cs:74-75,475-502`), then `connect-capture-device` for each RTMP device as
  Windows `SyncRtmpIngestSourcesAsync` does; gated on capabilities `srt-ingest`/`rtmp-ingest`.
- Produces: `SourceType.srtIngest` ("SRT ingest", picker group "SRT"), `.rtmpIngest` ("RTMP ingest", group
  "RTMP") from capture vendor `srt`/`rtmp` (Windows `ShowInputRosterService.cs:112-113,207-208`).

**Behaviour.** Layout follows `W/Views/SourcesInputsPage.xaml:28-311`: below the existing table and the
Zoom-feed/Capture row, two full-width disclosure groups, **"SRT ingest"** then **"RTMP ingest"**, never inside
another card's scroll view. SRT header: title, "N of 8 SRT sources", a "+" button; each row is the Windows
grid: Name / Summary / runtime health (port of `SrtIngestStatusPolicy.Label`) with Remove at the right; row 2
Mode (listener, caller, rendezvous) | Host | Port | Latency ms; row 3 Stream ID ("Optional receiver routing
ID"), Passphrase (a secure field). Footer line "Configured SRT sources are available in Show inputs and
Routing." RTMP: title, "N of 8 RTMP sources", "+"; each row Name, Summary
("Listener configured — publish to the URL below" / "Enter rtmp://host:port/app/stream to enable this
listener"), Remove; "Listener URL" field with placeholder `rtmp://0.0.0.0:1935/live/stream`, committed on
blur; footer "Publish to the listener URL, then select its RTMP source in Show inputs. Each source needs a
unique port." RTMP URLs persist in the Keychain (account `rtmp-ingest-<n>`); SRT is session-only (D7). The
URL never appears in names, status, logs or the picker (Windows `RtmpIngestSource` comment).

- [ ] **Step 1: Write the failing cases** (fixtures copied from `W.Tests/RtmpIngestSourcesCoordinatorTests.cs`):
  `ingest/rtmp-url-validation-matches-windows` (the six invalid URLs at lines 11-16 refused; a valid one
  accepted), `ingest/rtmp-wire-drops-invalid-and-duplicate-ports`, `ingest/rtmp-numbers-reuse-the-lowest-free`
  (remove 1 of 2, add → `rtmp-ingest-01`), `ingest/srt-defaults-and-cap` (first port 10000, eighth 10007, a
  ninth refused), `ingest/srt-status-label-matches-windows`, `ingest/commands-in-every-sync`,
  `ingest/config-resent-on-core-generation` (Review Focus 1), `ingest/rtmp-url-in-keychain-never-prefs`,
  `ingest/srt-not-persisted`, `ingest/source-types-and-picker-groups`, `ingest/url-never-in-names-or-logs`.
- [ ] **Step 2: Run red, implement, run green;** self-check; lint; Sources tab offscreen render showing both
  groups below the table.
- [ ] **Step 3: Live (owner hardware):** a phone SRT camera app publishing to the Mac's SRT listener reaches a
  slot, the multiview and an ISO recording; OBS publishing RTMP to the listener does the same. Record any
  firewall prompt (row I-1) and the received fps.
- [ ] **Step 4: Mutations.** M11a skip the port de-duplication → wire case red. M11b persist SRT → not-persisted
  case red. M11c send the config only on edit → core-generation case red.
- [ ] **Step 5: Commit.** `git commit -m "feat(mac-i1): SRT and RTMP ingest catalogs on Sources"`

---

## Task 12 (B1a, native): the macOS browser host (M, deps: Task 0 rows B-1, B-4; OD-3 if B-1 failed)

**Files:**
- Modify: `native/browser-host/main-macos.mm`
- Modify: `N/modules/BrowserHostExitMessage.h` (Mac codes and sentences, D12)
- Test: create `native/tests/BrowserHostMacTest.mm`; extend `native/tests/BrowserHostExitMessageTest.cpp`

**Interfaces:**
- Produces host CLI (Windows parity, `native/browser-host/main.cpp:27`):
  `corevideo-browser-host --url U --shm NAME [--width N --height N --fps N] [--dump-bmp PATH --dump-after-ms N --exit-after-ms M]`.
  The dump mode writes a 32-bit BMP of the latest published frame and prints one line
  `dump mean=B,G,R,A samples=...` (same field names as the Windows host).
- Produces exit codes: 0 normal, 2 usage, 4 unusable URL, 5 shared segment failed, 6 WebKit failed to start.
  `browserHostExitMessage(5)` = "The browser source could not open its shared frame buffer. Reload the
  source; if it keeps failing, restart CoreVideo Pro." `(6)` = "WebKit could not start for this browser
  source. Reload the source." `(4)` = "This browser source URL could not be loaded. Check the URL and reload
  the source." Code 3 keeps the WebView2 sentence.

**Behaviour.** stdin lines: `reload` reloads the page (today the Mac host reads and discards stdin); EOF quits.
Nothing is written to stdout (the core's stdout is its RPC channel; the adapter also redirects, Task 13).
`file://` URLs load with `loadFileURL:allowingReadAccessToURL:` (read access = the file's directory); `data:`
and `http(s)://` with `loadRequest:`. Popups are denied (`WKUIDelegate` returns nil), downloads are cancelled
(`WKNavigationDelegate` download policy), all media requires a user gesture
(`mediaTypesRequiringUserActionForPlayback = WKAudiovisualMediaTypeAll`) so the page is silent locally and in
Program. `ProcessInfo beginActivityWithOptions:` (user-initiated, idle-system-sleep-disabled not set) keeps
App Nap off. If row B-1 showed hidden-page throttling, apply the OD-3 answer in one function
`keepPaintingWhileOffscreen(WKWebView*)`. Frames stay premultiplied BGRA (the segment contract,
`BrowserSourceShm.h:12`).

- [ ] **Step 1: Write the failing tests** (Apple; each runs the built host with `--dump-bmp` against a page in
  a temp dir and parses the printed line):
  - `BrowserHostMac.TransparentHalfWhiteBoxIsPremultiplied`: page = transparent body + a full-viewport div
    `background: rgba(255,255,255,0.5)` → dump mean **(128,128,128,128) ± 2** (premultiplied 50% white).
  - `BrowserHostMac.OpaqueRedPageIsRed`: `background:#ff0000` → mean (0,0,255,255) ± 2.
  - `BrowserHostMac.LoadsFileAndDataUrls`: the red page through `file://` and through a `data:text/html,...`
    URL both give the red mean (Review Focus 5).
  - `BrowserHostMac.ReloadCommandReloadsThePage`: page shows a counter from `sessionStorage` incremented on
    load and paints its value as a colour level; write `reload\n` to stdin; the second dump's level is the
    next step (fixture: level 10 then 20 in the red channel).
  - `BrowserHostMac.PageMediaStaysSilent`: page with `<audio autoplay src=tone.wav>` and an `AudioContext`
    oscillator that paints green only if `audio.paused === false || ctx.state === "running"`, red otherwise →
    mean is red.
  - `BrowserHostMac.WritesNothingToStdout`: stdout captured over a full run is empty.
  - `BrowserHostMac.ExitCodes`: no args → 2; unusable URL → 4; an `--shm` name over 31 chars → 5.
  - `BrowserHostExitMessage.MacCodesHaveTheirSentences` (literals above) and `…CodeThreeStaysWebView2`.
- [ ] **Step 2: Run red, implement, run green.**
- [ ] **Step 3: Measure** the achieved fps of the Task 0 rAF page through the shipped capture loop at
  1920x1080 and 1280x720; publish both numbers in the outcomes. Under 30 fps at 1080p opens B3 (Phase 5).
- [ ] **Step 4: Mutations.** M12a ignore `reload` (today) → reload test red. M12b `loadRequest:` for `file://`
  → file test red (per row B-4). M12c drop the media gesture rule → silence test red. M12d return 3 for the
  segment failure (today) → exit-code test red.
- [ ] **Step 5: Commit.** `git commit -m "feat(mac-b1): macOS browser host: reload, file URLs, silent media, exit codes, self-test"`

---

## Task 13 (B1b, native): finish the POSIX browser supervisor (M, deps: none)

**Files:**
- Modify: `N/modules/BrowserSourceHostAdapter.cpp:57-71` (`resolveDefaultHostPath`), `:497-531` (reaping),
  `:575-591` (orphan kill), `:613-673` (POSIX spawn), `:742-760` (`closeProcessLocked`)
- Create: `native/tests/BrowserHostStub.cpp` (+ CMake target `corevideo-browser-host-stub`, path passed to the
  tests like `COREVIDEO_STUBBORN_FFMPEG_PATH`, `native/CMakeLists.txt:1146`)
- Test: extend `native/tests/BrowserSourceTest.cpp`

**Interfaces:**
- Produces: `std::string resolveDefaultHostPath()` POSIX order: `$COREVIDEO_BROWSER_HOST_PATH`; then
  `<core dir>/corevideo-browser-host`; then `<core dir>/../Helpers/CoreVideo Browser Host.app/Contents/MacOS/corevideo-browser-host`
  (the core lives in `Contents/Resources`); empty if none is executable.
- Produces: POSIX spawn through `posix_child::spawn`: stdin = the control pipe's read end, stdout and stderr =
  the core's stderr (`stdoutFd = stderrFd = STDERR_FILENO`, Task 10's field), guarded.
- Produces: `closeProcessLocked` closes the control pipe and moves the pid to a reap list; the supervisor
  thread, holding no adapter lock, calls `stopGroup(pid, 500ms)`. Liveness uses `waitpid(pid, WNOHANG)`
  with the same failure accounting and `browserHostExitMessage(exitCode)` as Windows.
- Stub host modes (`COREVIDEO_TEST_BROWSER_STUB_MODE`): `frames` (writes a 64x36 gradient frame into the
  segment every 33 ms, quits on stdin EOF, records `reload` lines to a file), `exit <n>`, `ignore-term`
  (ignores SIGTERM and stdin EOF), `premul-half-white` (writes (128,128,128,128) frames).

- [ ] **Step 1: Write the failing tests** (`BrowserSourcePosix.*`, POSIX, stub host via `COREVIDEO_BROWSER_HOST_PATH`):
  1. `DeadHostIsReapedAndRestartedWithBackoff`: mode `exit 7`; within the injected-clock schedule the source
     shows `consecutiveFailures` 1, 2, … at 5/10/20/40 s and `gaveUp` after the fifth with
     `lastError == "Browser host exited (code 7)."`; no zombie (`waitpid(pid, WNOHANG)` from the test returns -1/ECHILD).
  2. `RemoveLeavesNoProcess`: mode `ignore-term`; add, wait for frames, remove → the stub pid is gone within
     1500 ms (the group SIGKILL after the 500 ms grace).
  3. `ReloadWritesTheCommandInPlace`: mode `frames`; reload → the stub's command file contains `reload`, and
     no respawn happened (`restartCount` unchanged).
  4. `CoreSigkillLeavesNoHost`: a fake core (probe `fake-core`-style helper that owns an adapter) SIGKILLed →
     stub gone within 3 s.
  5. `HostPathResolvesFromTheHelperBundle`: a temp tree `X/Resources/<test core copy>` and
     `X/Helpers/CoreVideo Browser Host.app/Contents/MacOS/corevideo-browser-host` → resolved to the latter
     with the environment variable unset.
  6. `HostStdoutNeverReachesTheCoreStdout`: stub writes `HELLO` to fd 1; the test process's captured stdout
     does not contain it; its stderr does.
  7. `PremultipliedFrameBlendsCorrectlyOverProgram` (Metal build): mode `premul-half-white`, a scene with a
     full-canvas background (B=200,G=100,R=0) and the browser route on top → Program readback centre
     **(228,178,128) ± 3** (premultiplied over: 128 + c·127/255). A straight-alpha blend would give
     (164,114,64), so the test decides the convention the spec left UNVERIFIED (§5.3).
- [ ] **Step 2: Run red, implement, run green;** existing `BrowserHostRestartPolicy.*`, `BrowserSourceShm.*`,
  `MediaCoreBrowserCommands.*`.
- [ ] **Step 3: Mutations.** M13a POSIX `closeProcessLocked` only nulls fields (today) → test 2 red. M13b no
  POSIX reaping (today) → test 1 red. M13c inherit stdout → test 6 red. M13d drop the helper-bundle candidate
  → test 5 red. If test 7 fails on correct code, the compositor blends straight alpha: fix the Metal content
  pipeline for capture BGRA, not the test, and record the D3D11 follow-up.
- [ ] **Step 4: Commit.** `git commit -m "feat(mac-b1): POSIX browser host supervision under the child guard"`

---

## Task 14 (B1c, release + shell): package, sign and find the browser host (M, deps: Tasks 12, 13; Task 0 rows B-2, B-3)

**Files:**
- Create: `native/browser-host/Info.plist.in`
- Modify: `native/CMakeLists.txt:528-536` (build the target as a bundle `CoreVideo Browser Host.app` with the
  plist; executable name `corevideo-browser-host`)
- Modify: `scripts/make-macos-shell-bundle.sh:162-165` (replace the "deliberately NOT bundled" block),
  signing blocks `:213-268`, the shell plist `:110-136`
- Modify: `scripts/release-macos.sh:179-189` (gate + target), the Mach-O audit
- Modify: `M/AppModel.swift:6167-6260` (`ShellPaths.Paths.browserHostPath`, `COREVIDEO_BROWSER_HOST_PATH`)
- Test: `scripts/tests/test-macos-ffmpeg-bundle.sh` pattern → create `scripts/tests/test-macos-browser-host-bundle.sh`;
  shell case `streaming/launch-env-names-the-bundled-guard-and-ffmpeg` extended

**Interfaces:**
- Produces: `Contents/Helpers/CoreVideo Browser Host.app` with `CFBundleIdentifier us.iamfatness.corevideopro.browser-host`,
  `CFBundleExecutable corevideo-browser-host`, `LSUIElement true`, `LSMinimumSystemVersion 13.0`,
  `NSAppTransportSecurity {NSAllowsArbitraryLoadsInWebContent true, NSAllowsLocalNetworking true}`,
  `NSLocalNetworkUsageDescription "CoreVideo Pro loads browser sources from your local network."`
- Produces: the shell's own plist gains `NSLocalNetworkUsageDescription` ("CoreVideo Pro connects to streaming,
  NDI and browser sources on your local network.") and, if row B-3/Task 9 Step 3 showed it is required,
  `NSBonjourServices ["_ndi._tcp"]`.
- Produces: `COREVIDEO_BROWSER_HOST_PATH` = the helper's executable in `CoreLaunchEnvironment.extras`
  (an operator's own value wins), so the pkg-installed core in `/Library/Application Support/CoreVideo Pro/`
  finds it.

**Behaviour.** Signing order: camera extension, Zoom engine, the helper app (Developer ID, hardened runtime,
timestamp, **no entitlements**, `--identifier us.iamfatness.corevideopro.browser-host`), core, ffmpeg, guard,
then the app; ad-hoc dev bundles sign it ad-hoc. Under `COREVIDEO_REQUIRE_HELPERS=1` a missing helper is
fatal. The release script refuses an app whose helper carries any entitlement or is unsigned.

- [ ] **Step 1: Write the failing checks.** `test-macos-browser-host-bundle.sh` (ad-hoc bundle into a temp
  dir): the helper exists at the path above, `codesign --verify --strict` passes on it and the app,
  `plutil -extract NSAppTransportSecurity.NSAllowsArbitraryLoadsInWebContent raw` = true,
  `codesign -d --entitlements -` on the helper prints none. Shell case: the launch env names
  `COREVIDEO_BROWSER_HOST_PATH` ending in `Helpers/CoreVideo Browser Host.app/Contents/MacOS/corevideo-browser-host`,
  and an inherited value wins.
- [ ] **Step 2: Run red, implement, run green;** `bash scripts/tests/test-macos-ffmpeg-bundle.sh` still 16/16.
- [ ] **Step 3: End to end, lone core.** Extend `scripts/qa/mac-stream-e2e.py` (or a sibling
  `mac-browser-e2e.py`) with `--lone-core`: a copy of the core outside the bundle plus the shell's
  environment adds a browser source for a local `http://<LAN IP>` page; the source reaches `running` with
  `measuredFps > 0`; without the shell environment it reports the "not found" error (proves the path matters).
- [ ] **Step 4: Signed run (owner keychain):** `scripts/release-macos.sh` produces a notarized app; on a clean
  macOS 15 or 26 account, an `http://` LAN page and an `https://` page both render; record whether a Local
  Network prompt appeared and which app it named.
- [ ] **Step 5: Mutations.** M14a drop the ATS keys → the packaged-host LAN page fails in Step 4's check (or the
  plist check red). M14b omit the env var → lone-core run fails. M14c sign the helper with the core's
  entitlements → the bundle test's no-entitlement check red.
- [ ] **Step 6: Commit.** `git commit -m "feat(mac-b1): bundle, sign and locate the browser host helper app"`

---

## Task 15 (B1d, shell): browser overlays card, DSK in/out, Sources add row (M-L, deps: Task 14 for live checks; can start against a scripted core)

**Files:**
- Create: `M/BrowserOverlays.swift` (model, URL rules, key state, re-add)
- Modify: `M/Overlays.swift:374-383` (replace the unavailable card), `M/SourcesTable.swift` (CaptureDevicesSection
  add row), `M/SourcesWorkspace.swift` (`SourceType.browser`), `M/AppModel.swift:3906-3930`
  (`sceneSyncBatch`), `:1773` (`onConnected`), `M/CoreCommands.swift` (builders)
- Test: create `M/ShellTestsPhase4Browser.swift` (cases `browser/*`)

**Interfaces:**
- Produces: `enum BrowserOverlayPolicy { static let presets = [(1920,1080,"1920x1080 (full-canvas overlay)"), (1280,720,"1280x720"), (960,540,"960x540 (widget)")]; static func normalizeUrl(_ s: String) -> String; static func withDefaultScheme(_ s: String) -> String; static func applyKeyState(sceneId: String, routes: [JSONObject], keyedBrowserIds: [String]) -> [JSONObject] }`.
  `normalizeUrl` = Windows `NormalizeBrowserOverlayUrl` (drops every `bg` query key, case-insensitive,
  `W/ViewModels/StudioViewModel.cs:14461-14490`). `withDefaultScheme` prefixes `https://` unless the URL starts
  with `http://`, `https://`, `file://` or `data:` (`:7245-7251`). `applyKeyState` = Windows
  `BrowserOverlayProgramService.ApplyKeyState`: copies the routes and appends, per keyed id, one route
  `{routeId: "\(sceneId)-browser-key-\(count+1)", mode capture, captureDeviceId: id, audioRole "isolated",
  rect (0,0,1,1), fitMode "fit", opacity 1, zIndex: count}`.
- Produces: `AppModel.addBrowserOverlay(url:presetIndex:)` sends `browser-add {payload:{url,width,height,fps:30}}`;
  `reloadBrowserSource(_ id:)`, `removeBrowserSource(_ id:)` send the payload-wrapped commands
  (`CoreCommands.payloadWrappedTypes` already lists them); `toggleBrowserDsk(_ id:)` keeps **one** id on
  Program (Windows `ToggleBrowserOverlayOnAir`); `saveBrowserToScene(_ id:)` adds a full-canvas capture layer
  to the Preview scene (Windows `AddBrowserOverlay` → `AddOverlayLayer("full-canvas|capture:<id>")`).
- Produces: `sceneSyncBatch` wraps Program routes with `applyKeyState(programSceneId, …, onAirIds)` and
  Preview routes with `applyKeyState(previewSceneId, …, previewIds)` (Task 16 fills `previewIds`), then
  applies the Tiles filter exactly as Windows (D10).

**Behaviour.** Card layout = `W/Views/OverlaysPage.xaml:390-470` (full width under the keyer/brand row, as the
Mac pane already places it): title "Browser overlays (DSK)", the Windows subtitle verbatim, a row URL field
("Graphics URL — https://…") | preset picker | accent "Add overlay"; empty readout "No browser sources yet —
add a graphics output URL on the Sources tab."; one row per `browser:<n>` device: name, "format · connection"
line plus the core's `measuredFps` and the host sentence when restarting or gave up, then buttons Reload,
Remove (D9), Save to scene, Preview (Task 16), and the accent DSK in / DSK out. Sources gets the Windows
add row inside the capture-devices card (`SourcesInputsPage.xaml:62-82`): field "Add browser source —
https://…", preset, ghost "Add". Browser devices type as "Browser" (picker group "Browser"). Re-add (D8): the
model keeps `[url, presetIndex, onAir, inPreview]` for the session; `onConnected` re-sends `browser-add` for
each and, when the new snapshot lists a `browser:<n>` with that URL, moves the DSK/PVW keys to the new id.
Support bundles: browser URLs pass through the existing `Redactor` (secret query keys); add a test, not a new rule.

- [ ] **Step 1: Write the failing cases.** Port the four Windows `BrowserOverlayProgramServiceTests` with the
  same routes and expectations as JSON fixtures: `browser/key-preserves-authored-route-when-out`,
  `browser/key-adds-independent-dsk-above-authored`, `browser/key-keeps-duplicates-adds-one`,
  `browser/key-adds-missing-on-air-topmost-full-canvas` (expected `zIndex` values 2 and 1 and opacities as in
  the C# file). Port the two `BrowserOverlayUrlTests` (`…?bg=%23000&theme=news` →
  `http://192.168.1.50:4001/output/ABCD/?theme=news`; the other URL unchanged). Plus:
  `browser/default-scheme`, `browser/add-sends-payload-wrapped-1080p30`, `browser/one-dsk-on-air`,
  `browser/dsk-route-rides-program-sync`, `browser/dsk-dropped-on-tiles-scene` (D10, a named parity
  behaviour), `browser/save-to-scene-adds-full-canvas-layer`, `browser/remove-clears-keys`,
  `browser/readded-and-rekeyed-on-core-generation` (Review Focus 1: old id `browser:1` on air; new core
  snapshot lists `browser:4` with the same URL → Program routes carry `browser:4`),
  `browser/gave-up-shows-host-sentence`, `browser/url-query-secret-redacted-in-bundle`,
  `browser/sources-add-row-and-type`.
- [ ] **Step 2: Run red, implement, run green;** self-check; lint; Overlays and Sources offscreen renders.
- [ ] **Step 3: Live (owner):** a real web graphics tool (for example H2R Graphics on the LAN over `http://`)
  keyed DSK-in over a meeting scene; kill the host process five times → "gave up" sentence, Reload clears it;
  kill the core → the overlay returns keyed.
- [ ] **Step 4: Mutations.** M15a `applyKeyState` replaces authored browser routes → first ported case red.
  M15b allow two on air → one-DSK case red. M15c re-add without re-keying → core-generation case red.
  M15d Tiles filter skipped → tiles case red.
- [ ] **Step 5: Commit.** `git commit -m "feat(mac-b1): browser overlays card, DSK in/out, Sources add row"`

---

## Task 16 (B2, shell + control): Preview staging and `browser.*` actions (S, deps: Task 15)

**Files:**
- Modify: `M/BrowserOverlays.swift`, `M/Overlays.swift` (Preview button), `M/AppModel.swift`
- Modify: `M/ControlSurface+Production.swift` (or a new `ControlSurface+Browser.swift`), `contracts/control.platforms.json:121-123`,
  regenerate the manifest
- Test: `M/ShellTestsPhase4Browser.swift` (cases `browser/pvw-*`, `browser/control-*`)

**Interfaces:**
- Produces: `toggleBrowserPreview(_ id:)` keeps one id staged on Preview (Windows `ToggleBrowserOverlayPreview`);
  labels "Preview" / "Clear PVW", status "Staged on Preview" / "Not staged" (`W/Models/ProductionModels.cs:339-359`);
  Add overlay stages the new source on Preview automatically and sets the status "\(name) staged on DSK Preview
  - use DSK in when ready" (Windows `AddBrowserOverlayFromUrlAsync`, polling the device list up to 20 × 100 ms;
  on timeout "Browser source started, but CoreVideo could not attach its Preview layer.").
- Produces: control actions `browser.add {url, width?, height?}` (width/height pick the nearest preset as
  Windows: (1280,720)→1, (960,540)→2, else 0), `browser.remove {id}`, `browser.reload {id}`, all `supported`
  in `control.platforms.json`; failures return the Windows reasons ("browser.add requires a url argument.").

- [ ] **Step 1: Write the failing cases.** `browser/pvw-one-staged`, `browser/pvw-route-rides-preview-sync`,
  `browser/pvw-add-overlay-stages-on-preview`, `browser/pvw-timeout-sentence`, `browser/control-add-picks-preset`,
  `browser/control-add-requires-url`, `browser/control-remove-and-reload` (each drives the real `ControlSurface`
  entry point, rule 10), `browser/control-manifest-lists-the-three`.
- [ ] **Step 2: Run red, implement, run green;** `npm run contract:check`; Companion module tests in a scratch copy.
- [ ] **Step 3: Mutations.** M16a Preview keys ride the Program sync → pvw-route case red. M16b manifest not
  regenerated → contract check fails. M16c a dispatch `case` removed while the manifest still lists it →
  control case red.
- [ ] **Step 4: Commit.** `git commit -m "feat(mac-b2): browser Preview staging and browser.* control actions"`

---

## Task 17: Phase acceptance (S, deps: all)

- [ ] **Step 1:** Full native suite (`ctest --test-dir native/build-metal -LE realtime`) and the shell suite;
  Windows CI green on the branch (Tasks 1, 4, 5 and 6 touch shared native code).
- [ ] **Step 2:** On a quiet machine, `python3 scripts/mac-show-drill.py --seconds 40 --load 8 --stream-srt 9710`
  with NDI on and a 1080p30 browser source keyed DSK-in: zero Program underruns or missed deadlines; report
  render time, browser ingest render-thread cost (spec gate: under 1 ms per tick, else IOSurface transport),
  and NDI's delivered fps.
- [ ] **Step 3:** Installer from `scripts/release-macos.sh` (notarized), installed via the pkg so the core runs
  from `/Library/Application Support/CoreVideo Pro/`; repeat the stream, NDI, ingest and browser checks there.
- [ ] **Step 4:** Owner live checks (spec §7): YouTube RTMPS 1080p60 H.264 for 30 minutes; H.265 over Enhanced
  RTMP to YouTube; SRT to a venue-style receiver; NDI into a second machine; a phone SRT camera to a slot and
  an ISO; a real web graphics tool keyed over a meeting.
- [ ] **Step 5:** Write the outcomes doc (counts before/after, every mutation result, every unverified item,
  merge notes) and commit it.

---

## Self-review notes

- **Spec coverage.** §5.1 GPU-direct and backpressure: Tasks 2-7. §5.1 HEVC policy/probe: Task 1. §5.2 NDI
  output: Tasks 8-9 (receive stays Phase 5). §5.2 ingest: Tasks 10-11 (the spec's "only the build gates are
  off" was wrong; spawn, stdout mapping and the guard are added). §5.3 browser: Tasks 12-16, including the
  alpha pixel test (Task 13 test 7), silence (Task 12), packaging/ATS/privacy (Tasks 0, 14), re-add per
  generation (Task 15). B3 and O5 stay Phase 5.
- **Carried obligations from Phase 2 outcomes:** HLS-over-https certificates (Task 1, D11); child guard for the
  browser host (Task 13) and ingest (Task 10); the Zoom engine and plugin host under the guard remain open and
  outside this lane; CI build of ffmpeg stays a follow-up. From the settings quick-wins outcomes: O-1 (60 fps
  picker) is answered by D5/Task 7; `StreamEncoderSettings.apply(to:)` stays the one merge point (Task 7
  extends `ownedKeys`).
- **Type consistency.** `encoderTextureExport` (Task 3) is read in Task 3 only; `encodePath` and
  `gpu-direct-stream-encode` (Task 6) are read in Task 7; `SpawnRequest.stdoutFd/stderrFd` (Task 10) is used
  in Task 13; `browserHostExitMessage` codes (Task 12) are consumed in Task 13.
