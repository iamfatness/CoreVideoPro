# GPU-direct hardware encode — design (#521)

**Status:** approved design, slice 1 scoped for implementation.
**Date:** 2026-09-13.

## Problem

Streaming a 1080p60 program built from up to 8×1080p Zoom sources cannot sustain
60 fps to the encoder. Measured live 2026-09-13: FFmpeg ran at `speed ≈ 0.73–0.95x`
(44–57 fps), so the stream fell progressively behind and YouTube reported "not
receiving enough video to maintain smooth streaming."

The cause was isolated by elimination, with telemetry, not theory:

- **Not bandwidth** — 48 Mbps up; lowering the target bitrate 10→6 Mbps changed nothing.
- **Not CPU** — total ~50%, no single core pegged.
- **Not GPU / NVENC** — GPU ~30%, `nvidia-smi utilization.encoder` **idle at 3–7%**.
  The encoder is starved, not the bottleneck.
- **Not the network path to YouTube** — the decisive test: streaming to a
  **localhost RTMP sink** (no internet, zero RTT) is **also 0.76x / 45 fps**.
- **Not `-re` pacing** — removed from both pipes with no change.

What it IS: the architecture reads the GPU-composited program **back to the CPU**
and pipes **~186 MB/s of raw NV12** (1920×1080 × 60 × 3.1 MB) through a Windows
pipe to a separate `ffmpeg.exe`, which reads it single-threaded and submits to an
idle hardware encoder. That raw-frame CPU hop is the ceiling — not the hardware.
vMix and Vizrt Vectar do this on comparable hardware because they keep the frame
on the GPU and hand the GPU surface straight to the hardware encoder.

Interim fixes already landed on `deploy/streaming-fixes-2026-09-13` (nv12
passthrough to skip a CPU swscale, `-stats` telemetry, `-re` removal, a larger
pipe buffer) lifted it to ~0.95x in one test but do not reach a stable 1.0x; the
raw-frame hop remains. This spec removes the hop.

## Goal

Feed the hardware encoder directly from the compositor's GPU program texture —
vMix/Vectar parity — eliminating the GPU→CPU readback and the raw-frame pipe.
Success = the live stream sustains `speed ≈ 1.0x` / ~60 fps with the hardware
encoder actually working, verified to a localhost sink and to real YouTube.

## Full re-architecture, decomposed into slices

The end state (all outputs GPU-direct) is too large for one spec. It is
decomposed into slices that each ship working software and reuse the same seam.
**This spec fully specifies slice 1; slices 2–4 are context and get their own
spec→plan cycle.**

| Slice | Ships | Notes |
|---|---|---|
| **1 (this spec)** | `GpuVideoEncoder` seam + live **stream** (program) GPU-direct on Windows | Fixes the acute failure; establishes the seam every slice reuses. |
| 2 | Program **recording** on the seam | Recording already feeds the MF hardware MFT a CPU readback; point it at the D3D11 texture. Small delta. |
| 3 | **ISO** recording on the seam | N per-source GPU encoders; hardest (session limits, per-source textures). Last. |
| 4 | **macOS** VideoToolbox implementation of the seam for slices 1–3 | Seam is cross-platform from day one; VT is an implementation, not a redesign. |

The **virtual camera** is out of scope permanently: it needs raw NV12 for a
system DLL, not an encoded output, so it keeps its readback tap.

## Approach decision

Three approaches were considered for how to encode from the GPU texture:

- **A (CHOSEN): Media Foundation hardware-encoder MFT fed the D3D11 shared texture.**
  Reuses the MF hardware H.264 MFT already in the tree (`MediaFoundationEncoderAdapter`,
  which recording uses today) and the `EncoderCapacityProbe` that already enumerates
  hardware MFTs. Cross-vendor on Windows (MF routes to NVENC / QSV / AMF
  automatically — no NVIDIA-only limitation, no separate QSV/AMF path). Structurally
  parallel to macOS VideoToolbox (both are OS hardware-encoder frameworks: hand them a
  platform GPU texture, they emit encoded sample buffers via a callback), so the seam
  maps almost one-to-one and slice 4 is an implementation not a redesign. No GPL.
- **B (rejected): NVIDIA NVENC SDK (`nvEncodeAPI`) directly.** Maximal control, but
  NVIDIA-only (needs a parallel QSV/AMF path for other GPUs), a new SDK dependency, and
  a shape less parallel to VideoToolbox. MF hardware MFTs hit 1080p60 from a D3D11
  texture fine (what OBS/vMix use on Windows), so B's control edge is not needed.
- **C (rejected): libavcodec/libavformat in-process (`d3d11va`).** Encode+mux in one
  library, but pulls FFmpeg libraries into the core binary — a GPL/licensing change
  against the house rule (FFmpeg is only ever an external exe today) and a heavy new
  dependency.

**FFmpeg stays, demoted to a pure muxer/transport.** The encoder emits a ~6 Mbps
H.264 elementary stream; the sender writes it to `ffmpeg -f h264 -i pipe:0 -c:v copy
-f flv rtmp://…`. Every hard-won part of the sender — RTMP/SRT endpoints, the
supervisor, reconnect, the send-proof, audio on its own pipe — is unchanged. The
pipe now carries 6 Mbps compressed instead of 186 MB/s raw, so the process
boundary is a non-issue.

## Architecture & components

One new interface, `GpuVideoEncoder`:

- **Input:** a platform GPU frame handle — Windows: the keyed-mutex D3D11 shared
  texture the compositor already exports (`ProgramFrame::sharedTexture`); macOS
  (slice 4): a Metal texture / `CVPixelBuffer`.
- **Output:** an H.264 elementary-stream bitstream (~6 Mbps) with keyframe /
  SPS-PPS metadata, delivered via a callback.
- **Config:** codec, bitrate, keyframe interval, resolution, fps — the settings the
  sender already resolves (`RtmpFfmpegArgsConfig` / the output settings).

**Windows implementation — `MediaFoundationGpuVideoEncoder`:**

- Runs on a **dedicated D3D11 device + thread**, reusing the exact pattern of the
  existing vcam tap (`D3D11CompositorAdapter::vcamTapLoop` / `vcamThread_`): a second
  device that `AcquireSync`es the program keyed-mutex texture off the render thread.
- Binds the MF hardware H.264 MFT to that device via `IMFDXGIDeviceManager`, wraps the
  acquired texture as an `IMFSample` over the D3D11 surface, and drives the MFT. The
  frame never crosses the CPU boundary; only the compressed bitstream does.
- Emits encoded samples through the callback to the sender.

**Files (slice 1):**
- Create `native/src/modules/GpuVideoEncoder.h` — the seam (interface + config + a
  pure path-selection/profile policy in the `CaptureReaderStallPolicy` shape).
- Create `native/src/modules/MediaFoundationGpuVideoEncoder.{h,cpp}` — the Windows impl.
- Modify `native/src/modules/RtmpFfmpegArgs.h` — a compressed-bitstream input mode
  (`-f h264 -i pipe:0 -c:v copy`) alongside the existing raw-pipe mode.
- Modify `native/src/modules/RtmpOutputSenderAdapter.cpp` — choose the path at stream
  start, consume the bitstream callback, keep the raw path as fallback.
- Possibly a small keyed-mutex program texture the encoder owns (mirroring the vcam
  tap's `vcamShared1_`), if sharing the existing program texture with WinUI/preview
  contends; decided during implementation.

## Data flow & threading

1. **Render tick** — the compositor composites the program to GPU and copies it into
   a keyed-mutex shared texture (it already does this for the vcam). This GPU→GPU copy
   is the only render-thread cost, microseconds.
2. **Encoder thread** (dedicated device, signalled once per delivered program frame by
   the same 60 Hz video-output tick that drives the senders today; never under
   `coreMutex` or on the render thread) — `AcquireSync` the texture → wrap as
   `IMFSample` → drive the MFT → retrieve encoded samples → callback to the sender.
3. **Sender** — writes the ~6 Mbps bitstream to the demoted FFmpeg muxer.

The encoder taps the program texture independently of the 2–3-frame program buffer
(which governs on-screen/output *delivery timing*); it encodes the current
program frame at the output cadence. Backpressure is gone by construction — 6 Mbps
compressed cannot fill the pipe the way 186 MB/s raw did; if the muxer/transport
ever stalls, the bitstream callback drops to newest (the sender's existing
newest-wins), keeping the encoder live.

## Coexistence & the per-output-encoder rule

**Slice 1 changes nothing else.** The new stream encoder is one more independent
consumer of the program texture; recording keeps its readback→MF path, ISO keeps
its path, the vcam keeps its NV12 readback. They already coexist as independent
consumers today.

**Encoders are per output, keyed by encode profile** (codec + resolution + fps +
bitrate + keyframe interval) — the vMix model. Stream (e.g. 6 Mbps) and recording
(e.g. 8.2 Mbps) legitimately want different settings, so a single shared encode
would force a bad compromise; instead each output owns an encoder configured to its
profile, and two outputs with *identical* profiles share one encoder automatically.
The session budget is the **existing `EncoderCapacityProbe` / `IsoEncoderAdmission`**,
which already gates ISO placement; program encoders now draw from the same budget,
with the program stream at priority-1 (like the program recording). For slice 1 this
is simply one encoder.

## Fallback, capacity, error handling

- **Path chosen at stream (re)start, not mid-flight.** If a hardware `GpuVideoEncoder`
  can initialize (hardware MFT present, a free session per `EncoderCapacityProbe`) →
  GPU-direct path; else → **today's raw-NV12-pipe→ffmpeg path, unchanged.** Deciding
  once at start avoids fragile mid-stream re-plumbing. Slice 1 is strictly additive:
  capable machines get 60 fps, everything else behaves exactly as now.
- **Capacity exhaustion** reuses `IsoEncoderAdmission`: the program stream is
  priority-1 and gets a session ahead of ISO; a full budget warns loudly and
  spills/refuses per the existing policy, never a silent degrade. The program stream
  never loses its session to an ISO.
- **Device loss** (TDR, driver update) reuses the `DeviceLossPolicy` generation
  pattern: the encoder's dedicated device is retired by generation; the keyed-mutex
  texture is invalid, so frames fail → the existing `OutputDestinationSupervisor` sees
  the sender unhealthy and runs its bounded restart → on restart the path is
  re-decided (GPU if recovered, else CPU fallback). No new resilience logic.
- **Encode errors:** a single failed frame is dropped newest-wins; sustained failure
  trips the supervisor's health signal → restart → fallback. Every transition is
  logged like the sender's other failures (`[rtmp]` / supervisor lines) and reaches
  the support bundle — never a silent fallback.

Worst case at any point is "back on the path you run today," which is what makes
this safe to deploy on a live tool.

## Testing & proof

Discipline: measure the thing, verify pixels and rate, never a status string.

- **Unit (no GPU):** the pure decisions — path selection (hardware available/session
  free → GPU vs CPU), profile-keying/dedupe, fallback classification — as pure
  policies in the `CaptureReaderStallPolicy` / `DeviceLossPolicy` shape.
- **Real-GPU encode test** (dev-machine, like the existing real-MF
  `EncoderRecordingSession` tests): feed a known D3D11 texture through
  `GpuVideoEncoder` → decode the emitted H.264 → assert **pixels** (mean luma in
  range) and frame count. Proves real video from a GPU texture, not just that it ran.
- **Acceptance metrics (the ones that caught the bug):** FFmpeg `-stats speed ≈ 1.0x`
  and sender **fps ≈ 60** (was 0.73–0.95x / 44–57); `nvidia-smi utilization.encoder`
  now **non-trivial** while total CPU **drops** (no readback + raw pipe).
- **Localhost-sink regression gate:** stream to `rtmp://127.0.0.1`, assert 60 fps /
  1.0x. No network, deterministic, runnable any time. This is the test that proved the
  bug is in the app, now inverted into the gate.
- **Load & live:** `scripts/mac-show-drill.py` gates the render/core stays at 60 with
  8×1080p while the encoder runs; then live acceptance in the test meeting to real
  YouTube — `speed=1.0x`, flat lag, YouTube health green.
- **A/B + operator-forceable fallback:** `COREVIDEO_GPU_ENCODE=0` (the
  `COREVIDEO_FRAME_SYNC` pattern) forces the CPU path, so GPU vs CPU can be A/B'd on
  one build and reverted instantly if a machine misbehaves.

## Global constraints

- **No GPL** in the core binary (the house rule that ruled out approach C); MF and the
  NVENC hardware path via MF are permissive; FFmpeg stays an external exe.
- **Never regress the fallback:** the raw-NV12-pipe path stays working and
  operator-forceable for every non-capable machine and every failure.
- **`coreMutex` discipline:** the encoder runs on its own device/thread, never under
  `coreMutex`, never on the render thread — identical to the vcam tap.
- **Cross-platform seam from day one:** the `GpuVideoEncoder` interface must not leak
  D3D11/MF types to its consumers, so VideoToolbox drops in at slice 4 without changing
  the sender.
- **Loud, never silent:** every path selection and every fallback is logged and reaches
  the support bundle.

## Out of scope (slice 1)

Recording (slice 2), ISO (slice 3), macOS/VideoToolbox (slice 4), the virtual camera
(permanent — raw NV12 to a system DLL), and any change to RTMP/SRT/NDI transport,
reconnect, or the supervisor beyond the compressed-bitstream input mode.
