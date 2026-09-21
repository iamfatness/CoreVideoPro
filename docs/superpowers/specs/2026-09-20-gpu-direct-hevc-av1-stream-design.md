# GPU-direct HEVC and AV1 for the live stream — design

**Date:** 2026-09-20
**Status:** approved design (owner, 2026-09-20), sub-project 1 of 3
**Parent:** `2026-09-13-gpu-direct-encode-design.md` (#521, slice 1 shipped in PR #523)
**Related:** #524 (bitstream write decoupling), #525 (GPU-direct recording/ISO — absorbed by sub-project 3)

## Why

On 2026-09-20 the owner streamed to YouTube with the video codec set to H.265
and YouTube reported it was not receiving enough data. The stream was on the
raw-pipe fallback: the compositor's frame was read back to the CPU as NV12,
piped to an external FFmpeg, and re-encoded there. FFmpeg's own progress line
read `fps=52 ... speed=0.87x` with the audio input lagging by 11 s. Only 87% of
real time reached YouTube. The GPU-direct path that shipped in #521 reads the
compositor's texture straight into the hardware encoder and holds 60 fps at
1.0x, but it is H.264-only, so choosing H.265 or AV1 silently left it.

Worse, the choice did not even stick: `EncoderPolicy.h` ships no HEVC encoder, so
the H.265 request launched `h264_nvenc` on the slow path. AV1 was refused as "not
guaranteed over RTMP" and also became H.264. The operator got H.264 either way,
delivered too slowly.

The owner's rulings, 2026-09-20:

- Ship **both HEVC and AV1** on GPU-direct. This **reverses the 2026-08-06
  encoder policy** that excluded HEVC for patent exposure. The owner accepted
  that exposure explicitly; `EncoderPolicy.h` must say so.
- Fix the raw fallback's throughput too (**sub-project 2**, own spec).
- Offer HEVC/AV1 for Program recording and ISO stems (**sub-project 3**, own
  spec, absorbs #525).

This spec is sub-project 1 only.

## Facts the design rests on (all measured 2026-09-20 on the owner's rig)

| Fact | Where it came from |
|---|---|
| NVIDIA H.264, HEVC and AV1 Encoder MFTs are registered (RTX 4090, driver 616.92) | `HKLM\SOFTWARE\Classes\MediaFoundation\Transforms` |
| FFmpeg (`N-124549`, LGPL, `C:\ffmpeg\bin`) copy-muxes a raw AV1 OBU stream into FLV with `-use_wallclock_as_timestamps 1 -r <fps> -f obu -i pipe -c:v copy -f flv` | local encode + ffprobe: `av1,1280,720` |
| The same for a raw HEVC Annex-B stream **only when the stream has no B-frames** (`-bf 0`); with B-frames the FLV muxer refuses (`Packet is missing PTS`), and `-fflags +genpts` does not help | local encode + ffprobe: `hevc` |
| YouTube accepts HEVC and AV1 over enhanced RTMP | public YouTube documentation (2023 beta, live since) |
| The raw fallback delivers 52 fps into the pipe at 1080p60 on this rig | FFmpeg stats of the 10:10 stream; the loss is upstream of FFmpeg |
| `MediaFoundationGpuVideoEncoder.cpp` already has `subtypeForCodec` (H.264/HEVC) but enumerates with `"h264"` hard-coded | code |
| `EncoderCapacityProbe` workloads carry a codec (`h264`/`hevc`), no `av1` | code |

## Design

### 1. Encoder seam (`native/src/modules/GpuVideoEncoder.h`)

`GpuVideoEncoderConfig` gains:

```cpp
std::string codec = "h264";  // "h264" | "hevc" | "av1" (normalized; "h265" -> "hevc")
```

The seam stays free of platform types. `GpuEncodedChunk` is unchanged: a chunk is
one access unit (H.264/HEVC Annex-B) or one temporal unit (AV1 low-overhead OBU),
and the sink treats it as opaque bytes exactly as today.

### 2. Windows implementation (`MediaFoundationGpuVideoEncoder.cpp`)

- `subtypeForCodec` gains `MFVideoFormat_AV1` (Windows SDK 10.0.26100 `mfapi.h`).
- MFT enumeration uses `subtypeForCodec(config.codec)` instead of `"h264"`.
- Output type per codec: H.264 keeps `eAVEncH264VProfile_High`; HEVC sets
  `eAVEncH265VProfile_Main_420_8`; AV1 sets `eAVEncAV1VProfile_Main_420_8` (all three
  names verified in Windows SDK 10.0.26100 `codecapi.h`). Level is left to the MFT.
- **HEVC: B-frames OFF.** Set `CODECAPI_AVEncMPVDefaultBPictureCount = 0` on the
  MFT's `ICodecAPI` before `SetOutputType`. This is load-bearing: the FLV muxer
  cannot take reordered raw HEVC (fact table above). If the MFT rejects the
  property, `start()` fails and the stream start is refused (section 5) — never
  a silent fall-through to a stream the muxer will reject 20 frames in.
- AV1 and H.264 do not set the B-frame property (H.264 keeps its shipped
  behavior; the AV1 MFT default muxed cleanly in the probe above; the round-trip
  test in section 8 is what proves it per rig).
- Everything else — the dedicated D3D11 device and thread, `IMFDXGIDeviceManager`,
  BGRA→NV12 video processor, the async MFT event loop, retained `NeedInput`
  credits, the 34 ms keyed-mutex consumer timeout — is untouched.

### 3. Muxer (`RtmpFfmpegArgs.h`, bitstream mode)

`videoBitstreamInput` mode gains the raw demuxer per codec:

| codec | demuxer | note |
|---|---|---|
| h264 | `-f h264` | unchanged |
| hevc | `-f hevc` | requires no B-frames (section 2) |
| av1 | `-f obu` | AV1 low-overhead OBU |

`-use_wallclock_as_timestamps 1 -r <fps>` stays for all three (the reasons in the
existing comment apply unchanged). `-c:v copy -f flv` is unchanged; this FFmpeg
writes the enhanced-RTMP `hvc1`/`av01` fourcc on its own. No `-tag:v` is passed.

### 4. Path decision (`GpuVideoEncoder.h` `chooseStreamEncodePath`, `RtmpOutputSenderAdapter.cpp`)

- `chooseStreamEncodePath` replaces `bool codecIsH264` with the resolved codec
  string and a `bool codecHasGpuEncoder` computed by the sender from the
  capacity probe for that codec. `"codec-not-h264"` is retired; the new reason
  for a codec the machine cannot encode on the GPU is `"no-hardware-encoder"`
  (the existing base reason), so support bundles keep one vocabulary.
- The sender passes `compatibility.videoCodec` (the codec actually sent) into
  the encoder config. It already resolves this from the configured codec and the
  enhanced-RTMP checkbox; nothing new is invented.

### 5. Refuse, never downgrade (`RtmpCompatibility.h`, `EncoderPolicy.h`, sender start)

The failure of 2026-09-20 was a silent downgrade that delivered the wrong codec
on the wrong path. Rule: **a stream starts with the codec the operator chose, or
it does not start, and the refusal names why.**

- `resolveRtmpCompatibility` no longer produces `fallbackApplied` for
  HEVC/AV1 with enhanced RTMP off. It produces a **refusal**:
  `result.refused = true`, `result.reason = "enhanced-rtmp-required"`, with the
  operator sentence "H.265 over RTMP needs Enhanced RTMP; enable it in Stream
  settings or choose H.264." The sender reports it through the existing
  start-failure path (`lastResultCode`, `TransportStatusFormatter`), which the
  shell already renders. `fallbackApplied` stays in the struct for older
  consumers but is never set by this resolver.
- `EncoderPolicy.h`: `codecHasSupportedHardwareEncoder("h265")` returns true on
  Windows; `isSupportedEncoder` admits `hevc_nvenc`; `preferredEncoderFor` and
  `encoderCandidatesFor` return `hevc_nvenc` for h265. The header comment gains
  a dated paragraph: the 2026-08-06 HEVC exclusion is reversed by the owner on
  2026-09-20, patent exposure accepted, so the exclusion does not read as an
  oversight and its reversal does not read as one either.
- A GPU encoder `start()` failure for **any** codec on this path refuses the
  stream start with `"gpu-encoder-start-failed"` and the HRESULT in the message.
  It does **not** drop to the raw path, because the raw path cannot hold 1080p60
  today (fact table). This is a deliberate, temporary tightening: sub-project 2
  makes the raw path a real fallback again, and that spec re-enables the
  downgrade **for H.264 only** (HEVC/AV1 have no raw-path encoder we ship with
  B-frames off; keeping them GPU-only is simpler and honest).
- Nothing changes for H.264 on machines that take the raw path today
  (`platform-unsupported`, `forced-off-by-env`): they keep working exactly as
  now. The refusal applies to a codec the operator chose that cannot be honored.

### 6. Capacity probe (`EncoderCapacityProbe`)

The workload codec gains `"av1"` → `MFVideoFormat_AV1`. Prewarm at stream start
probes the chosen codec's workload, not H.264's. `sessions<=N` is reported per
codec. AV1 sessions may be fewer than H.264 sessions on the same card; the probe
reports what it measured and the sender's refusal (section 5) quotes it.

### 7. Shell (`StudioViewModel`, `ProductionSettingsWindow`, `TransportStatusFormatter`)

- The codec picker keeps H.264 / H.265 / AV1. The "Enhanced RTMP (H.265 / AV1)"
  checkbox stays the gate for carrying them over RTMP.
- New start-failure sentences, one per refusal reason (`enhanced-rtmp-required`,
  `no-hardware-encoder` with the codec named, `gpu-encoder-start-failed`), in
  `TransportStatusFormatter` next to the existing ladder. They name the codec,
  never lead with the stream key, and never say "check the server URL" for a
  codec problem.
- The `[gpu-encode] path=... ` log line gains `codec=<codec>`; the send-proof
  JSONL's `videoCodec`/`ffmpegVideoEncoder` fields already exist and now agree
  with what is sent.

### 8. Testing

- **Pure:** `GpuVideoEncoderPolicyTest` — the path decision per codec (GPU
  available / not), the retired `codec-not-h264`, the refusal reasons.
  `RtmpCompatibility` tests — refusal instead of `fallbackApplied`.
  `RtmpFfmpegArgsTest` — demuxer per codec in bitstream mode.
  `EncoderPolicy` tests — HEVC admitted on Windows, still absent on macOS.
- **Real GPU (Windows-only, self-skipping):**
  `MediaFoundationGpuVideoEncoderTest` runs the compositor→encoder→FFmpeg
  round-trip **per codec** (decoded coded-Y luma within 16 of the encoded gray,
  as today); HEVC additionally asserts the FLV copy-mux succeeds, which is the
  B-frames-off proof on that rig. Skips with a `[ SKIPPED ]` line when the MFT is
  absent. Per CLAUDE.md, these must be run on a real `COREVIDEO_WITH_MF_ENCODER=ON`
  build before merge — CI cannot compile them.
- **Gate:** `scripts/validate-gpu-encode.mjs` gains `--codec h264|hevc|av1`
  (sets the stream codec + enhanced RTMP through the same core commands the
  shell sends). Per codec it asserts the GPU path was taken, ≥58 fps received at
  the localhost SRT sink, and sink `speed ≥ 0.97x`. `--force-raw` is unchanged
  and H.264-only.
- **Manual acceptance (owner):** one live YouTube run per codec at 1080p60,
  FFmpeg stats reading `speed≈1.0x` and 60 fps for 5 minutes, YouTube's ingest
  health green, `nvidia-smi utilization.encoder` non-trivial. Recorded in the
  PR.

### 9. Out of scope

Raw-fallback throughput (sub-project 2); recording and ISO (sub-project 3, #525);
macOS VideoToolbox HEVC (the seam carries `codec`, VT is an implementation);
SRT/NDI codec signalling; 10-bit/HDR; any codec beyond H.264/HEVC/AV1; the
bitstream write decoupling (#524, independent, recommended before this ships).

## Risks named

- **HEVC B-frames off costs efficiency** (~10–15% larger streams at the same
  quality vs. B-frames on). Accepted: FLV cannot carry reordered raw HEVC from a
  pipe, and the alternative — the encoder emitting DTS and FFmpeg reading a
  container, not a raw stream — is a bigger change than this sub-project.
- **AV1 session count** on consumer NVIDIA is not the H.264 count. The probe
  reports it; the refusal quotes it; nothing is assumed.
- **Enhanced RTMP at the destination** is the operator's responsibility (the
  checkbox). Twitch/others differ from YouTube; the refusal sentence points at
  the checkbox, not at the destination's policy, which we cannot know.

## Outcome (2026-09-20)

**§2's AV1 half did not survive contact with the hardware. HEVC did.**

- **HEVC: shipped.** GPU-direct at 1080p60, `node scripts/validate-gpu-encode.mjs
  --codec hevc` green (60.0 fps received, sink speed ≥ 1.0x), B-frames off via the
  low-latency ladder as §2 designed.
- **AV1: REFUSED, not shipped.** The NVIDIA AV1 Encoder MFT binds, starts and
  emits samples at the correct cadence, but the access units are **near-empty** —
  ~54 bytes per sample at 1920x1080@60 (≈49 after a normal 5,892-byte keyframe)
  against H.264's ~12,483 on the same build and rig. The muxed stream is
  **~18 kbit/s against a configured 6 Mbps**. A codec that streams at 0.3% of its
  bitrate is exactly the defect §5 exists to remove, so AV1 now refuses at start
  with its own named code, `codec-not-deliverable` — never silently downgraded,
  never shipped broken. `--codec av1` PASSES BY OBSERVING THAT REFUSAL.

### Three hypotheses, ELIMINATED (do not repeat them)

| Hypothesis | Verdict | Evidence |
|---|---|---|
| Deep encoder pipeline (lookahead / alt-ref) | ELIMINATED | low-latency mode accepted (`av1 b-frames off via low-latency-mode`), rate unchanged |
| FFmpeg's `obu` demuxer on a live pipe | ELIMINATED | a 10 s 1080p60 `av1_nvenc` OBU stream piped through the sender's exact flags: 600/600 frames, 1.15 MB in / 1.18 MB out |
| Our async MFT loop reading one output per event | ELIMINATED | instrumented: `av1 output drain: events=1260 samples=1260 mean=1.00 max-per-event=1`, identical to H.264 |

What is left is the encoder producing empty access units at this resolution and
rate — a vendor/driver-level investigation, tracked as
[#565](https://github.com/iamfatness/CoreVideoPro/issues/565), and deliberately
NOT part of this sub-project.

Rig: RTX 4090, driver 616.92, Windows SDK 10.0.26100.

### What flips AV1 back on

`admission.codecKnownNotDeliverable` in `RtmpOutputSenderAdapter::startFfmpegProcess`
is one named predicate with an obvious home for a rig-specific override. The gate
(`node scripts/validate-gpu-encode.mjs --codec av1`) is the thing that decides: it
must stop passing by refusal and start passing by streaming.
