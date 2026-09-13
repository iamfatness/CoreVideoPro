# GPU-direct encode — slice 1 (live stream) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Encode the live program stream directly from the compositor's D3D11 GPU texture via the Media Foundation hardware H.264 MFT, eliminating the GPU→CPU readback and the 186 MB/s raw pipe that caps 1080p60 streaming at ~0.76x realtime.

**Architecture:** A cross-platform `GpuVideoEncoder` seam takes a platform GPU frame handle and emits an H.264 elementary-stream bitstream via callback. The Windows implementation (`MediaFoundationGpuVideoEncoder`) runs a dedicated D3D11 device + thread (mirroring the existing vcam tap), binds the MF hardware H.264 MFT via `IMFDXGIDeviceManager`, and encodes the acquired keyed-mutex program texture with no CPU readback. The RTMP/SRT sender writes the ~6 Mbps bitstream to FFmpeg demoted to a pure muxer (`-f h264 -i pipe:0 -c:v copy`). The existing raw-NV12-pipe path stays as a start-time, operator-forceable fallback.

**Tech Stack:** C++17, Direct3D 11, Media Foundation (hardware H.264 MFT, `IMFDXGIDeviceManager`, `IMFTransform`), the existing `RtmpOutputSenderAdapter` + `RtmpFfmpegArgs`, `EncoderCapacityProbe`, `DeviceLossPolicy`, `OutputDestinationSupervisor`, GoogleTest.

**Spec:** `docs/superpowers/specs/2026-09-13-gpu-direct-encode-design.md`

## Global Constraints

- No GPL in the core binary; MF and the MF-routed hardware encoder are permissive; FFmpeg stays an external exe.
- The raw-NV12-pipe fallback path must stay working and operator-forceable (`COREVIDEO_GPU_ENCODE=0`) for every non-capable machine and every failure.
- The encoder runs on its own D3D11 device + thread, NEVER under `coreMutex`, NEVER on the render thread — identical to the vcam tap (`D3D11CompositorAdapter::vcamTapLoop`).
- The `GpuVideoEncoder` interface must not leak D3D11/MF types to its consumers (the sender), so VideoToolbox drops in at slice 4 without changing the sender.
- Every path selection and every fallback is logged (`nativeLogf`, `[gpu-encode]` prefix) and reaches the support bundle. Loud, never silent.
- Encoders are per output, keyed by encode profile (codec + resolution + fps + bitrate + keyframe). Slice 1 is one encoder (the stream).
- Multi-config generator: build with `cmake --build native/build-dev --config Release`. Run `corevideo-native-tests.exe`; a stale `build-dev/Release/*.exe` is a different binary — verify new tests appear.
- Verify PIXELS and RATE, never a status string. The acceptance metric is FFmpeg `-stats speed ≈ 1.0x` / ~60 fps with `nvidia-smi utilization.encoder` non-trivial and CPU down.

---

## File Structure

- **Create** `native/src/modules/GpuVideoEncoder.h` — the seam: `GpuVideoEncoderConfig`, `GpuVideoEncoderFrame` (opaque GPU handle), `GpuEncodedChunk`, the `GpuVideoEncoder` abstract interface, and the pure `GpuEncodePathPolicy`.
- **Create** `native/src/modules/MediaFoundationGpuVideoEncoder.h/.cpp` — the Windows impl + a factory `createMediaFoundationGpuVideoEncoder()`.
- **Modify** `native/src/modules/RtmpFfmpegArgs.h` — a compressed-bitstream input mode.
- **Modify** `native/src/modules/RtmpOutputSenderAdapter.cpp` — start-time path choice, bitstream consumption, fallback, capacity + device-loss wiring.
- **Create** `native/tests/GpuVideoEncoderPolicyTest.cpp` — pure policy tests (no GPU).
- **Create** `native/tests/MediaFoundationGpuVideoEncoderTest.cpp` — real-GPU encode-to-pixels test (dev machine).
- **Modify** `native/tests/RtmpFfmpegArgsTest.cpp` — bitstream-mode arg assertions.
- **Create** `scripts/validate-gpu-encode.mjs` — the localhost-sink acceptance gate.
- **Modify** `CLAUDE.md` — document the GPU-encode path, fallback, and the env toggle.

---

## Task 1: The `GpuVideoEncoder` seam + pure path policy

**Files:**
- Create: `native/src/modules/GpuVideoEncoder.h`
- Test: `native/tests/GpuVideoEncoderPolicyTest.cpp`

**Interfaces:**
- Produces:
  - `struct GpuVideoEncoderConfig { int width; int height; int fps; int bitrateKbps; double keyframeIntervalSeconds; std::string rateControl; std::string h264Profile; };`
  - `struct GpuVideoEncoderFrame { std::string sharedHandleHex; uint32_t iosurfaceId; int width; int height; int64_t frameNumber; };` (mirrors `ProgramFrameSharedTexture`; the opaque GPU handle — no D3D11/MF types)
  - `struct GpuEncodedChunk { const uint8_t* data; size_t size; bool keyframe; int64_t frameNumber; };`
  - `using GpuEncodedChunkSink = std::function<void(const GpuEncodedChunk&)>;`
  - `class GpuVideoEncoder { virtual ~GpuVideoEncoder(); virtual bool start(const GpuVideoEncoderConfig&, GpuEncodedChunkSink) = 0; virtual bool submit(const GpuVideoEncoderFrame&) = 0; virtual void stop() = 0; virtual bool healthy() const = 0; };`
  - `struct GpuEncodePathInputs { bool hardwareEncoderAvailable; bool sessionAvailable; bool forcedOffByEnv; bool platformSupported; };`
  - `enum class GpuEncodePath { GpuDirect, CpuFallback };`
  - `struct GpuEncodePathPolicy { [[nodiscard]] static GpuEncodePath choose(const GpuEncodePathInputs&); [[nodiscard]] static const char* reason(const GpuEncodePathInputs&); };`

- [ ] **Step 1: Write the failing test** (`GpuVideoEncoderPolicyTest.cpp`)

```cpp
#include "modules/GpuVideoEncoder.h"
#include <gtest/gtest.h>
using corevideo::modules::GpuEncodePath;
using corevideo::modules::GpuEncodePathPolicy;
using corevideo::modules::GpuEncodePathInputs;

TEST(GpuEncodePathPolicy, GpuWhenEverythingIsAvailable) {
  EXPECT_EQ(GpuEncodePathPolicy::choose({true, true, false, true}), GpuEncodePath::GpuDirect);
}
TEST(GpuEncodePathPolicy, EnvToggleForcesCpuEvenWhenCapable) {
  EXPECT_EQ(GpuEncodePathPolicy::choose({true, true, /*forcedOffByEnv=*/true, true}), GpuEncodePath::CpuFallback);
}
TEST(GpuEncodePathPolicy, NoHardwareEncoderFallsBack) {
  EXPECT_EQ(GpuEncodePathPolicy::choose({false, true, false, true}), GpuEncodePath::CpuFallback);
}
TEST(GpuEncodePathPolicy, NoFreeSessionFallsBack) {
  EXPECT_EQ(GpuEncodePathPolicy::choose({true, false, false, true}), GpuEncodePath::CpuFallback);
}
TEST(GpuEncodePathPolicy, UnsupportedPlatformFallsBack) {
  EXPECT_EQ(GpuEncodePathPolicy::choose({true, true, false, /*platformSupported=*/false}), GpuEncodePath::CpuFallback);
}
TEST(GpuEncodePathPolicy, ReasonNamesTheFirstBlocker) {
  EXPECT_STREQ(GpuEncodePathPolicy::reason({true, true, true, true}), "forced-off-by-env");
  EXPECT_STREQ(GpuEncodePathPolicy::reason({false, true, false, true}), "no-hardware-encoder");
  EXPECT_STREQ(GpuEncodePathPolicy::reason({true, false, false, true}), "no-free-encoder-session");
  EXPECT_STREQ(GpuEncodePathPolicy::reason({true, true, false, false}), "platform-unsupported");
  EXPECT_STREQ(GpuEncodePathPolicy::reason({true, true, false, true}), "gpu-direct");
}
```

- [ ] **Step 2: Run and verify it fails** — `corevideo-native-tests.exe --gtest_filter=GpuEncodePathPolicy.*` → FAIL (header not found / undefined).

- [ ] **Step 3: Implement `GpuVideoEncoder.h`** — the structs/interface above, plus the pure policy. `choose` returns `GpuDirect` iff `platformSupported && hardwareEncoderAvailable && sessionAvailable && !forcedOffByEnv`, else `CpuFallback`. `reason` checks in this order and returns the first failing: `platformSupported`→"platform-unsupported", `forcedOffByEnv`→"forced-off-by-env", `hardwareEncoderAvailable`→"no-hardware-encoder", `sessionAvailable`→"no-free-encoder-session", else "gpu-direct". (Note the test's env case has platformSupported true, so env is reported; keep the order: platform, env, hardware, session.)

- [ ] **Step 4: Run and verify PASS** — same filter, 6/6 pass.

- [ ] **Step 5: Add the test file to `native/tests/CMakeLists.txt`** (follow the existing `*Test.cpp` entries) and rebuild `corevideo-native-tests`.

- [ ] **Step 6: Commit** — `git add native/src/modules/GpuVideoEncoder.h native/tests/GpuVideoEncoderPolicyTest.cpp native/tests/CMakeLists.txt && git commit -m "feat(gpu-encode): GpuVideoEncoder seam + pure path-selection policy"`

---

## Task 2: FFmpeg compressed-bitstream input mode

**Files:**
- Modify: `native/src/modules/RtmpFfmpegArgs.h`
- Test: `native/tests/RtmpFfmpegArgsTest.cpp`

**Interfaces:**
- Consumes: `RtmpFfmpegArgsConfig` (existing).
- Produces: a new field `bool videoBitstreamInput = false;` on `RtmpFfmpegArgsConfig`. When true, `buildRtmpFfmpegArguments` emits `-f h264 -thread_queue_size 512 -i pipe:0` for video and `-c:v copy` (no re-encode, no `-b:v`/`-pix_fmt`/`-g`/encoder args); audio branch unchanged; `-map 0:v:0 -map 1:a:0`, container/endpoint unchanged.

- [ ] **Step 1: Write the failing test** (append to `RtmpFfmpegArgsTest.cpp`)

```cpp
TEST(RtmpFfmpegArgs, BitstreamInputModeCopiesVideoAndSkipsRawEncode) {
  corevideo::modules::RtmpFfmpegArgsConfig config;
  config.videoBitstreamInput = true;
  config.hasAudio = true;
  config.audioInput = "pipe:3";
  const auto args = corevideo::modules::buildRtmpFfmpegArguments(config);
  EXPECT_NE(args.find("-f h264 -thread_queue_size 512 -i pipe:0"), std::string::npos);
  EXPECT_NE(args.find("-c:v copy"), std::string::npos);
  EXPECT_EQ(args.find("-f rawvideo"), std::string::npos);   // no raw video input
  EXPECT_EQ(args.find("-b:v "), std::string::npos);          // no re-encode bitrate
  EXPECT_NE(args.find("-c:a aac"), std::string::npos);       // audio still encoded
  EXPECT_NE(args.find("-stats"), std::string::npos);         // telemetry retained
}
```

- [ ] **Step 2: Run and verify it fails** — `--gtest_filter=RtmpFfmpegArgs.BitstreamInputModeCopiesVideoAndSkipsRawEncode` → FAIL.

- [ ] **Step 3: Implement** — in `buildRtmpFfmpegArguments`, branch on `config.videoBitstreamInput`: when true, emit the h264 input + `-map 0:v:0 -map 1:a:0 -c:v copy` and skip the raw-video input block and all video-encode args (`-b:v/-maxrate/-bufsize/-g/-profile:v/-bf/-pix_fmt/-c:v <encoder>`); keep the audio input branch, `-c:a aac ...`, `-af aresample=async=1:first_pts=0`, container, endpoint, and `-hide_banner -loglevel warning -stats -stats_period 1` exactly as the raw path.

- [ ] **Step 4: Run and verify PASS**; run the whole `RtmpFfmpegArgs.*` suite — all green (no regression to the raw-mode tests).

- [ ] **Step 5: Commit** — `git commit -am "feat(gpu-encode): FFmpeg compressed-bitstream (-c:v copy) input mode for the RTMP/SRT muxer"`

---

## Task 3: `MediaFoundationGpuVideoEncoder` (Windows impl)

**Files:**
- Create: `native/src/modules/MediaFoundationGpuVideoEncoder.h`, `native/src/modules/MediaFoundationGpuVideoEncoder.cpp`
- Test: `native/tests/MediaFoundationGpuVideoEncoderTest.cpp`

**Interfaces:**
- Consumes: `GpuVideoEncoder`, `GpuVideoEncoderConfig`, `GpuVideoEncoderFrame`, `GpuEncodedChunkSink` (Task 1).
- Produces: `std::unique_ptr<GpuVideoEncoder> createMediaFoundationGpuVideoEncoder();` (returns a started-nullptr-on-failure instance via `start()` returning false). A static `bool mediaFoundationHardwareEncoderAvailable(int w, int h, int fps)` used by the path policy inputs (delegates to `EncoderCapacityProbe`/MF MFT enumeration).

**Implementation reference — reuse the existing patterns, do not reinvent:**
- Dedicated device + thread: mirror `D3D11CompositorAdapter::vcamTapLoop` / `vcamThread_` / `ensureVcamTap` and the `AcquireSync(0,0)`/`ReleaseSync(1)` keyed-mutex handshake (`D3D11CompositorAdapter.cpp` ~2060–2280). Open the program texture from `GpuVideoEncoderFrame::sharedHandleHex` via `OpenSharedResource1`.
- MF hardware H.264 MFT + `IMFDXGIDeviceManager`: mirror the MFT enumeration and `MFT_MESSAGE_NOTIFY_BEGIN_STREAMING` / async-MFT unlock dance in `EncoderCapacityProbe` and `MediaFoundationEncoderAdapter.cpp` (the ASYNC MFT `MF_TRANSFORM_ASYNC_UNLOCK`, `SetInputType`/`SetOutputType`, sample submission). Input type is a D3D11 surface (`MFCreateDXGISurfaceBuffer`/`MFCreateVideoSampleFromSurface`); output is H.264 Annex-B.
- Emit each output sample's bytes through `GpuEncodedChunkSink` with `keyframe` from `MFSampleExtension_CleanPoint`.
- `healthy()` reflects the encode thread running + no device loss; `stop()` joins the thread and releases MF/D3D11.

- [ ] **Step 1: Write the failing test** (`MediaFoundationGpuVideoEncoderTest.cpp`) — dev-machine, self-skips without a D3D11 device (print `[ SKIPPED ]` to stderr, like the FFmpeg-dependent tests).

```cpp
// REQUIRES DEV MACHINE: real D3D11 + MF hardware encoder. Self-skips otherwise.
TEST(MediaFoundationGpuVideoEncoder, EncodesAKnownTextureToDecodableH264) {
  // 1. Create a D3D11 device; render a solid mid-gray (luma ~128) 1920x1080
  //    keyed-mutex shared texture; get its shared HANDLE hex.
  // 2. createMediaFoundationGpuVideoEncoder(); start({1920,1080,60,6000,2.0,"cbr","high"}, sink).
  //    If start() returns false (no hardware MFT), print SKIPPED and return.
  // 3. submit() the same frame ~120 times at 60fps cadence; collect chunks.
  // 4. Assert: >=1 keyframe, total bytes > 0, and decoding the collected
  //    Annex-H264 (ffmpeg -f h264 -i - -f rawvideo -) yields frames whose mean
  //    luma is within +/-16 of 128 (real picture, not black/garbage).
}
```

- [ ] **Step 2: Run and verify it fails** — encoder header/impl absent → FAIL (or link error) before implementation.

- [ ] **Step 3: Implement `MediaFoundationGpuVideoEncoder.{h,cpp}`** per the reference above; add both to `native/CMakeLists.txt` (the `corevideo_native` sources) and the test to `native/tests/CMakeLists.txt`. Guard the whole TU with `#if defined(_WIN32)`; the factory returns `nullptr`-equivalent (start()→false) on non-Windows so the seam still links for macOS (slice 4 replaces it).

- [ ] **Step 4: Build `--config Release` and run** — on the dev rig the test encodes and the luma assertion passes; confirm `nvidia-smi utilization.encoder` moves during the run (manual, note in the report).

- [ ] **Step 5: Commit** — `git commit -m "feat(gpu-encode): MediaFoundationGpuVideoEncoder — D3D11 texture -> MF hardware H.264 MFT -> bitstream"`

---

## Task 4: Wire the encoder into the sender with start-time path choice + fallback

**Files:**
- Modify: `native/src/modules/RtmpOutputSenderAdapter.cpp`
- Test: `native/tests/MediaCoreCommandTest.cpp` (sender integration) or a focused `RtmpOutputSenderGpuPathTest.cpp`

**Interfaces:**
- Consumes: `GpuVideoEncoder` + factory (Task 3), `GpuEncodePathPolicy` (Task 1), `RtmpFfmpegArgsConfig::videoBitstreamInput` (Task 2).
- Produces: the sender chooses its path in `ensureFfmpegProcess`/start (`RtmpOutputSenderAdapter.cpp:1058`): build `GpuEncodePathInputs` from `createMediaFoundationGpuVideoEncoder()`'s availability + `EncoderCapacityProbe` + `getenv("COREVIDEO_GPU_ENCODE")=="0"`; if `GpuDirect`, start the encoder with a sink that writes chunks to the FFmpeg stdin pipe and set `config.videoBitstreamInput=true`; else the existing raw path (`writeFrameToFfmpeg`). Log `[gpu-encode] path=<gpu-direct|cpu-fallback> reason=<...>`.

- [ ] **Step 1: Write the failing test** — inject a fake `GpuVideoEncoder` (seam is abstract) + fake availability into the sender; assert: (a) capable → `videoBitstreamInput` true and frames go to `encoder->submit`, not `writeFrameToFfmpeg`; (b) `COREVIDEO_GPU_ENCODE=0` → raw path; (c) encoder `start()` returns false → raw path. (Add a test seam to inject the encoder factory, in the `setStillImageDecoderForTest` style.)

- [ ] **Step 2: Run and verify it fails.**

- [ ] **Step 3: Implement** the path choice, the bitstream→pipe sink (write chunk bytes to the existing FFmpeg stdin handle; reuse the `WriteFile`/`::write` loop, now ~6 Mbps), the fallback, and the injectable factory seam. On the GPU path the per-frame `writeFrameToFfmpeg` raw write is replaced by `encoder->submit(frame.sharedTexture-derived GpuVideoEncoderFrame)`; the frame's `sharedTexture.sharedHandleHex` must be populated (it already is for WinUI/preview).

- [ ] **Step 4: Run and verify PASS**; run the full `*Rtmp*`/`*Output*`/`*Sender*` suites — no regression.

- [ ] **Step 5: Commit** — `git commit -am "feat(gpu-encode): sender chooses GPU-direct vs raw-pipe at start; bitstream to the muxer; env-forceable fallback"`

---

## Task 5: Capacity + device-loss integration

**Files:**
- Modify: `native/src/modules/RtmpOutputSenderAdapter.cpp`, `native/src/modules/MediaFoundationGpuVideoEncoder.cpp`
- Test: `native/tests/MediaFoundationGpuVideoEncoderTest.cpp` + the sender path test

**Interfaces:**
- Consumes: `EncoderCapacityProbe::lookup()` (session availability → `GpuEncodePathInputs::sessionAvailable`), `DeviceLossPolicy` classification (existing), `OutputDestinationSupervisor` (existing, unchanged).
- Produces: the program stream registers as a priority-1 encoder session; on device loss the encoder retires by generation and reports `healthy()==false` so the supervisor restarts the sender, which re-decides the path.

- [ ] **Step 1: Write the failing test** — (a) `sessionAvailable=false` (probe exhausted) → policy chooses `CpuFallback` and the sender logs `reason=no-free-encoder-session`; (b) a simulated device-loss in the encoder flips `healthy()` false and a subsequent `submit` returns false (drives the supervisor's existing unhealthy→restart).

- [ ] **Step 2: Run and verify it fails.**

- [ ] **Step 3: Implement** — feed `EncoderCapacityProbe` availability into the path inputs (program stream priority-1: it asks for a session before any ISO placement); classify device-removed HRESULTs in the encode loop with the existing `DeviceLossPolicy` shape, set `healthy_=false`, retire the device by generation, and let `submit` fail so the supervisor takes over. Loud `[gpu-encode]` logging on every transition.

- [ ] **Step 4: Run and verify PASS**; run supervisor + lifecycle suites — no regression.

- [ ] **Step 5: Commit** — `git commit -am "feat(gpu-encode): capacity (priority-1) + device-loss/supervisor integration for the GPU encoder"`

---

## Task 6: Acceptance gate — localhost-sink regression test + docs

**Files:**
- Create: `scripts/validate-gpu-encode.mjs`
- Modify: `CLAUDE.md`

**Interfaces:**
- Consumes: the running core over the control API / a core-over-stdio harness (mirror `scripts/qa/live-meeting-soak.mjs` / `validate-srt-output.mjs`), FFmpeg `-stats`, a localhost RTMP sink.

- [ ] **Step 1: Write `validate-gpu-encode.mjs`** — start a localhost RTMP sink (`ffmpeg -listen 1 -i rtmp://127.0.0.1:1935/live/test -c copy -f null -`), drive the core (fake engine, 8×1080p, a Tiles wall on Program) to stream at that endpoint, read the sender's FFmpeg `-stats` tail, and **FAIL unless `speed >= 0.97x` and sender fps >= 58** over a 30s window. Print the achieved speed/fps and whether the GPU path was taken (`[gpu-encode] path=` in the log).

- [ ] **Step 2: Run it on the dev rig** — expect PASS on the GPU path (the whole point of the slice). Run with `COREVIDEO_GPU_ENCODE=0` and confirm it takes the raw path (documents the A/B).

- [ ] **Step 3: Update `CLAUDE.md`** — a section under the encoder/streaming notes: the GPU-direct path (MF hardware MFT from the D3D11 texture), the `-c:v copy` bitstream muxer, the `COREVIDEO_GPU_ENCODE=0` toggle, the start-time path choice + fallback, and the acceptance gate (`validate-gpu-encode.mjs`, `speed~1.0x`). Note it is slice 1 of #521 (stream only; recording/ISO/macOS to follow).

- [ ] **Step 4: Commit** — `git add scripts/validate-gpu-encode.mjs CLAUDE.md && git commit -m "test(gpu-encode): localhost-sink acceptance gate (speed~1.0x/60fps) + CLAUDE.md"`

---

## Final acceptance (run before opening the PR)

- [ ] Full native suite green on the Release build (`corevideo-native-tests.exe`, minus timing tests on a loaded box).
- [ ] `scripts/mac-show-drill.py --seconds 40 --load 8` PASSES (render/core still 60 with the encoder running).
- [ ] `scripts/validate-gpu-encode.mjs` PASSES on the GPU path; `COREVIDEO_GPU_ENCODE=0` still streams (raw fallback).
- [ ] Live: test meeting → real YouTube, FFmpeg `speed≈1.0x`, flat lag, YouTube health green; `nvidia-smi utilization.encoder` non-trivial, total CPU down vs the raw path.
