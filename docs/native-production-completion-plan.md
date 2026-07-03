# CoreVideo Pro — Native Production Completion: Plan & Spec

_Status snapshot: 2026-06-21. Owner: production / native media-core. This is the
plan and per-feature spec for closing the gap between the **typed contract surface**
(which is broad and well-tested) and the **real native media pipeline**._

_Reconciled 2026-06-21 against the current source: items 5 (recording), 6 (audio),
7 (RTMP audio), and 8 (scene framing) have landed real implementations and are no
longer synthetic/telemetry — the §1 matrix below reflects this. The remaining
synthetic/unfinished work is real device capture (F1: items 1–3 ingest), overlay/
caption raster (item 9), SRT output, and the dev-rig/hardware validation pass._

This document covers ten capability gaps where the UI, protocol, and state machine
are wired but the native pixel/PCM/transport implementation is not yet production
real:

1. UVC as a native program source
2. AJA / Blackmagic (DeckLink) capture
3. SRT (ingest + output)
4. NDI sender
5. Recording (real A/V + ISO)
6. Audio pipeline (real PCM routing/DSP)
7. RTMP (real program-audio mux + codec compatibility)
8. Scene source framing in native output
9. Overlays / lower thirds / captions rendering
10. Automation / AI director

---

## 0. Governing principles (read first)

Every slice below must obey the boundary discipline this repo already follows.
These are not optional; they are how the codebase stays buildable and testable.

- **Stub-default, dev-gated.** The default in-container build is
  `-DCOREVIDEO_STUB=ON`, `-DCOREVIDEO_ENABLE_DEV_ADAPTERS=OFF` and must stay green.
  All real hardware/SDK/transport code lives behind `COREVIDEO_ENABLE_DEV_ADAPTERS`
  plus a per-feature flag (`COREVIDEO_WITH_*`). See
  [`native/CMakeLists.txt`](../native/CMakeLists.txt). Stubs keep producing
  deterministic synthetic output so contract tests run without hardware.
- **Two mirrors, one shape.** Wire types exist twice and must stay byte-for-byte
  equivalent: the C++ core ([`native/src/core/Protocol.h`](../native/src/core/Protocol.h),
  [`MediaCore.cpp`](../native/src/core/MediaCore.cpp)) and the Node mirror
  ([`native-core/src/protocol.ts`](../native-core/src/protocol.ts),
  [`mediaCore.ts`](../native-core/src/mediaCore.ts)). The renderer mirror
  ([`src/engine/nativeMediaCoreProtocol.ts`](../src/engine/nativeMediaCoreProtocol.ts))
  mirrors the protocol exactly. `tests/.../ContractParityTest` and `npm run typecheck`
  are the parity gates — extend them for every new command/snapshot field.
- **State mutates in the engine, never the renderer.** The renderer serializes
  production state ([`src/domain/production.ts`](../src/domain/production.ts)) into
  transport-neutral commands
  ([`src/engine/nativeMediaCoreCommands.ts`](../src/engine/nativeMediaCoreCommands.ts))
  and reads back immutable `*Snapshot`s. No `electron`/OBS/browser-capture imports
  in any `src/engine/` file.
- **Definition of done (per slice).** A capability is "real" only when: (a) the
  real adapter is implemented behind its dev gate; (b) the stub still round-trips;
  (c) the matching capability string is announced in `profileCapabilities()`
  ([`MediaCore.cpp`](../native/src/core/MediaCore.cpp)) **only when actually built**;
  (d) the snapshot reports real counters (frames/bytes/levels), not synthetic ones;
  (e) tests exist at each touched layer; (f) `README.md`'s "Current Slice"
  inventory is updated.

**The two big multipliers.** Eight of the ten items are blocked on two missing
pieces of plumbing. Build these first; they unlock everything downstream:

- **F1 — Real frame-pixel transport** (sources → compositor → outputs carry actual
  `VideoFrame.pixels` / `ProgramFrame` pixels, not empty buffers or synthetic
  patterns).
- **F2 — Real program audio bus** (a PCM mixing graph that produces a true program
  mix and per-bus taps, replacing the telemetry-only `AudioDsp`).

A third foundation, **F3 — real compositor pixel pipeline** (framing + overlay
text/image rendering), makes the program frame *correct* once F1 feeds it real
pixels. Items 8 and 9 are F3.

---

## 1. Where we are today (status matrix)

| # | Capability | Today | Real? | Blocked on |
|---|---|---|---|---|
| 1 | UVC program source | WinUI MediaCapture preview only; frames never reach native core (`CaptureDeviceFrameRouter` is WinUI-local) | UI-only | F1 + WinUI→native bridge |
| 2 | AJA / DeckLink | Enumeration/probe only; `pollVideoFrames()` returns `{}` | Probe-only | F1 + vendor SDK frame capture |
| 3 | SRT ingest | Real libsrt sockets + packet RX; `pixels` empty, no decoder, no audio | Transport-only | F1 + decoder stage |
| 3 | SRT output | `createSrtOutputSender()` returns `nullptr` (both arms) | None | F2 + libsrt + encoder |
| 4 | NDI sender | No native sender exists; UI/validation + synthetic mock only | Contract-only | F1 + F2 + NDI SDK |
| 5 | Recording | Muxes the **real** composed program frame + **real** program audio; honors the requested resolution/fps (`kDefault*` are fallbacks only, `MediaFoundationEncoderAdapter.cpp:539`) | Real (Windows MF) | dev-rig A/V-sync + 30-min soak proof |
| 6 | Audio pipeline | **Real PCM** routing matrix with program/ISO taps, BS.1770 master meter, bus-insert compressor/limiter; WASAPI monitor dev-gated. ASIO/VST/embedded audio still state-only | Mostly real (= F2) | ASIO capture + VST3 host |
| 7 | RTMP | Real FFmpeg + BGRA program-frame pipe (Windows); feeds the **real** F2 program-audio tap (`anullsrc` only as fallback, `RtmpOutputSenderAdapter.cpp:700`); H.264/AAC compat matrix resolved | Real (dev-gated) | live push proof + E-RTMP opt-in + POSIX pipe |
| 8 | Scene source framing | D3D11 **applies** `fitMode`/`sourceScale`/`sourceOffset`/border (`D3D11CompositorAdapter.cpp:601`), matching the CPU stub | Real | Windows visual parity smoke |
| 9 | Overlays / lower-thirds / captions | Tracked + placed as fixed colored rects; no text/image/animation/brand | Placed, not rendered | this **is** F3 (raster) |
| 10 | Automation / AI | 100% TS heuristics (`autoProductionDirector.ts`, `buildMagicScene`); zero ML; no native involvement | Heuristic | optional model service |

Key source references for the above are inline in §3.

---

## 2. Foundations (build these first)

### F1 — Real frame-pixel transport (sources → compositor → outputs)

**Problem.** `ICaptureDevice::pollVideoFrames()` defaults to returning empty frames
([`native/src/modules/Interfaces.h:314`](../native/src/modules/Interfaces.h)); SRT
ingest and the DeckLink/AJA `MutableCaptureDeviceList` produce metadata-only
`VideoFrame`s with `pixels = {}`; UVC pixels exist only in WinUI
(`CaptureDeviceFrameRouter`). The compositor therefore renders slates for every
non-synthetic source, and the recorder/RTMP path only ever sees synthetic or
program-preview pixels.

**Spec.** A single, shell-agnostic frame-buffer contract carries real pixels end
to end:

- Extend the frame path so a source publishes a real frame buffer (BGRA or a
  documented planar format) with `width/height/stride/timestamp/frameId`. Reuse the
  Zoom spine's shared-memory pattern (`native/src/zoom/ShmFrameReader.*`,
  `I420Convert.*`) rather than inventing a new transport.
- A frame **pool** (ref-counted, bounded) so high-rate sources don't allocate per
  frame and back-pressure is observable as dropped-frame counters in the snapshot.
- `pollVideoFrames()` returns frames whose `pixels` are populated for **all** real
  source kinds; the compositor uploads them as textures
  ([`D3D11CompositorAdapter::uploadLayerTexture`](../native/src/modules/D3D11CompositorAdapter.cpp)).
- A **decoder stage** seam for compressed sources (SRT/NDI-receive later): packets
  in → `VideoFrame.pixels` out, FFmpeg `libavcodec` behind a dev gate.

**Layers touched.** `Interfaces.h` (frame/pool types), each `*CaptureSource`/`*CaptureDevice`,
`MediaCore.cpp` `renderSyntheticTick()` merge path (`pollVideoFrames` consumers,
~`MediaCore.cpp:2058`), `D3D11CompositorAdapter` texture upload, stub
`CpuNoopCompositor`. Snapshot: add real `framesIngested`/`droppedFrames` per source.

**Gate.** A test source (the existing test-pattern adapter) flows **real pixels**
to a captured `ProgramFrame` and a unit test asserts non-empty, correctly-sized
`ProgramFrame` pixels (extend `D3D11CompositorTest` / a CPU-compositor test).

### F2 — Real program audio bus (PCM mixing graph)

**Problem.** `AudioDsp.h` reads frame **metadata** and computes derived numbers
(`outputLevel = inputLevel + gainDb*4`, `loudnessLufs = -24 + masterLevel*0.12`);
`sync-participant-audio-mix` and `sync-audio-routing-matrix` store gain/route
**state** only ([`MediaCore.cpp:1037`, `1104`](../native/src/core/MediaCore.cpp));
monitor playback is `Beep()`; there is no PCM anywhere. Every output that needs
audio (RTMP, recording, NDI, SRT) has nothing real to send.

**Spec.** A real-time mixing graph that produces actual PCM:

- An audio frame now carries a **PCM buffer** (interleaved float or s16, documented
  sample-rate/channels), not just `rmsLevel`/`peakLevel`.
- **Buses** PGM L/R, ISO 1–8, MON, STREAM, AUX 1–2 are real summing buses. The
  routing matrix crosspoints (already validated against this bus list,
  `MediaCore.cpp:1092`) apply per-crosspoint gain to real samples and sum into the
  destination bus.
- **Per-participant chain:** gain → pan → (noise suppression) → (VST insert host) →
  bus sends. Solo/mute act on samples, not state.
- **Master chain:** true-peak **limiter** (real gain reduction, not a `>=88` label)
  and an **ITU-R BS.1770 LUFS** meter (integrated/short-term), replacing the lookup.
- **Program mix tap** exposed to outputs as PCM at 48 kHz stereo, plus per-ISO taps
  for ISO recording.
- **Monitor** plays the MON bus to a real device (WASAPI shared, then ASIO).
- Metrics in the snapshot are **measured** from the real signal.

**Layers touched.** `AudioDsp.h` (real DSP), `Interfaces.h` (`AudioFrame` PCM +
`IAudioMixer` returning a mixed buffer), `MediaCore.cpp` audio command handlers and
tick, new `WasapiMonitorOutput` (dev-gated). Inputs: ASIO/WASAPI capture + embedded
capture-card audio land here (see item 6). Outputs consume the program/ISO taps.

**Gate.** `native-recording-proof.mjs` already asserts `audioPresent === true`
and `audioPacketsObserved > 0` — make that assertion pass against **real** muxed
PCM (today it passes on a synthetic counter, `MediaCore.cpp:1856`). Add a DSP unit
test: known input samples → expected RMS/peak/LUFS within tolerance, and limiter
reduces a hot signal.

**Status (2026-06-20) — monitor output landed.** The `Beep()` placeholder is
gone. `IAudioMixer` now exposes a real stereo MON-bus PCM tap
(`monitorBusPcm()`/`monitorBusSampleRate()`/`monitorBusChannels()`) summed from
participant PCM and brickwall-limited to −1 dBFS in `DevSafeAudioMixer`. A new
`IAudioMonitorOutput` module plays that bus: the default build wires a safe
in-memory stub (`createStubAudioMonitorOutput`), and a dev-gated
`createWasapiMonitorOutput` (`COREVIDEO_WITH_WASAPI_MONITOR`, links `ole32`)
drives a real shared-mode WASAPI render endpoint with device selection, mix-format
conversion (float32/16/32-bit PCM), and overflow-drop. MediaCore opens/stops it
from `sync-audio-monitor` and pushes the MON bus each tick. Covered by
`MediaCoreAudioMonitor.*` tests; the real WASAPI path still needs a dev-rig smoke
pass (audible output to a chosen endpoint).

**Status (2026-06-20) — routing-matrix sample mix + program/ISO taps landed.**
The routing matrix now mixes **real PCM**, not just crosspoint state. New pure
kernel `mixRoutedBuses` (`AudioDsp.h`) runs each source's channel strip (fader
gain → stereo pan → mute/solo) then sums every crosspoint into stereo buses,
brickwall-limited to −1 dBFS. MediaCore's tick builds the sources from the polled
PCM frames + synced participant mix and the crosspoints from the synced routing
sends, then exposes a **program tap** (the stereo `master` bus) and **ISO taps**
(`iso-*`) via `programAudioTapPcm()` / `audioBusTapPcm()`; the routing-matrix
snapshot gains a measured `busTaps` array (per-bus peak/RMS dBFS, frame count).
Covered by `AudioDsp.RoutedBusMix*` and
`MediaCoreAudioMonitor.RoutingMatrixMixesPcmIntoProgramAndIsoTaps`.

**Status (2026-06-20) — recording now muxes real program audio.** The synthetic
capture source emits real PCM tones, and `IEncoderSink` gained `submitAudio()`
(stub sink counts muxed packets/samples). MediaCore's recording tick feeds the
program audio (routed `master` tap, else the default program mix) into the
encoder, and the recording proof's `audioPacketsObserved`/`audioPresent` are now
sourced from **real muxed PCM** (plus new `audioSampleCount`/`audioChannels`/
`audioSampleRate` proof fields), replacing the synthetic frame counter at the old
`MediaCore.cpp:1856`. Verified: `MediaCoreCommand.RecordsRealProgramAudioPcmIntoMux`
(165 native tests green) **and** `native-recording-proof.mjs` passing against the
built `corevideo-native.exe` (program=28/iso=28 frames).

**Status (2026-06-20) — BS.1770 master meter on the program tap.** The
`audioMixSession` snapshot gained a `masterMeter` object measured from the real
program audio each tick (routed `master` tap, else the default program mix):
`momentaryLufs` (400 ms) and `shortTermLufs` (3 s) over a rolling deinterleaved
window via the existing K-weighted kernels, `truePeakDbfs` (4× oversample), and a
spec-gated `integratedLufs`, plus `windowMs`. This replaces the
level-lookup *for the program meter*; the legacy per-channel `loudnessLufs`
estimate is retained for the channel strip readout. The integrated value comes
from a reusable incremental `Bs1770IntegratedMeter` (`AudioDsp.h`): continuous
K-weighting, 400 ms gating blocks at 100 ms hops (75% overlap), absolute (-70
LUFS) then relative (-10 LU) gating per BS.1770-4. Covered by
`MediaCoreCommand.MeasuresBs1770MasterLoudnessOnProgramTap` and
`AudioDsp.IntegratedLoudness*` (169 native tests green).

**Status (2026-06-20) — per-bus inserts act on samples.** Bus insert chains now
process the routed bus PCM each tick (`applyBusInsertChain` in `AudioDsp.h`,
driven from the routing sends' `busPluginInserts`): recognized built-in dynamics
run on the audio — `applyCompressor` (static 4:1 over -18 dBFS) and the limiter
(-1 dBFS) — while EQ/gate/third-party inserts remain acknowledged pass-throughs
until a real VST/AU host lands. Program/ISO taps and the master meter therefore
reflect insert processing. Covered by `AudioDsp.Compressor*`,
`AudioDsp.BusInsertChain*`, and
`MediaCoreAudioMonitor.BusInsertCompressorActsOnRoutedBusPcm` (174 native tests
green). **Remaining F2:** a real VST/AU insert host (load actual plugin binaries —
needs a dev host, not stub-testable) and the MF encoder adapter's own
`submitAudio` (dev-gated, default no-op today).

### F3 — Real compositor pixel pipeline (framing + raster)

This is items 8 and 9; specced there. It is a foundation because **all** outputs
consume the composed `ProgramFrame` — fixing framing/overlay rendering fixes every
downstream output at once.

---

## 3. Per-item specs & plans

Each item lists: **Current**, **Spec (done = )**, **Plan**, **Gate/tests**, **Flag**.

### Item 1 — UVC as a native program source

- **Current.** WinUI captures real BGRA via `MediaCapture`
  (`native-shell/CoreVideoPro.WinUI/Services/CaptureDeviceFrameReaderService.cs`)
  and publishes to `CaptureDeviceFrameRouter`, but only the WinUI preview
  (`VideoSurfaceCoordinator`) consumes it. Native core never sees the frames, so
  UVC works "multiview-ish" (preview) but not reliably in program/output.
- **Spec (done =).** A UVC device selected as a source delivers real frames into the
  native core as a first-class `ICaptureDevice` whose `pollVideoFrames()` returns
  populated pixels, routable via `capture:<deviceId>` into scenes, program, ISO, and
  every output — at parity with the WinUI preview.
- **Plan.**
  1. Decide ingest path (product decision, see §6): **(A)** native UVC capture in
     C++ via Media Foundation `IMFSourceReader` (preferred — single source of
     truth, no cross-process copy), or **(B)** bridge WinUI `CaptureDeviceFrame`
     frames over the existing JSON-RPC/shared-memory transport into the C++ core.
  2. Implement a `UvcCaptureDevice : ICaptureDevice` (F1 frame buffers), behind
     `COREVIDEO_WITH_UVC` + dev gate; register it in
     `createDefaultModules()`/`CompositeCaptureDevice`
     (`native/src/modules/StubModules.cpp:583`).
  3. Embedded UVC audio (if present) feeds the F2 mixer as a participant/source input.
  4. Announce `uvc-capture` capability when built.
- **Gate/tests.** A UVC route renders real pixels into the program frame on a dev
  rig; stub build still green. `nativeCaptureDeviceEngineAdapter` test for the new
  device kind.
- **Flag.** `COREVIDEO_WITH_UVC` (new) + `COREVIDEO_ENABLE_DEV_ADAPTERS`.
- **Status (2026-06-20) — test-pattern ingest path proven (F1 gate).** The stub
  `FakeCaptureDevice` now emits real BGRA frames for connected+signal devices: a
  deterministic 7-bar SMPTE test pattern (`makeTestPatternBgra`) delivered as a
  first-class `VideoFrame` with `participantId "capture:<deviceId>"`. The existing
  tick already merges `captureDevice->pollVideoFrames()` and maps `capture-input`
  routes to `capture:<deviceId>` layers, so a routed capture source now composites
  **real pixels** into the program frame. Covered by `CaptureIngest.*` (184 native
  tests green): the device emits correctly-sized populated pixels, a no-signal
  device stays silent, and a routed capture frame yields non-empty program-frame
  pixels showing the pattern (not the synthetic slate). **Remaining item 1
  (dev-gated):** the real `UvcCaptureDevice`/WinUI bridge delivering live camera
  frames + embedded UVC audio.
- **Status (2026-07-02) — native Media Foundation UVC capture shipped (path A),
  awaiting rig validation.** `UvcCaptureDeviceAdapter.cpp` (behind
  `COREVIDEO_WITH_UVC`, ON in `build-native-dev.ps1`) enumerates VIDCAP devices
  via `MFEnumDeviceSources` (throttled, list-only — never opens a camera),
  negotiates the best native format for the 1080p60 target
  (`pickBestUvcFormat`: resolution > frame rate > NV12/YUY2/MJPG subtype), sets
  NV12 output with `MF_SOURCE_READER_ENABLE_ADVANCED_VIDEO_PROCESSING` (MF
  decodes MJPG / converts YUY2 on the capture thread), and repacks each sample
  into I420 (`nv12ToI420` — byte reshuffle only, no CPU color math) so frames
  ride the existing GPU I420 HLSL convert. The shader is now parametrized for
  limited/full range + BT.709/BT.601 per frame (`VideoFrame.i420FullRange` /
  `i420Bt601`; defaults keep Zoom bit-identical). Device ids are the same
  SHA-256(symbolic link) stable ids the WinUI shell derives
  (`stableCaptureDeviceIdFromSymbolicLink`, parity-tested against
  `CaptureDeviceDiscoveryMapper.CreateStableDeviceId`), so `capture:<id>` route
  keys match either ingest path; `nativeDeviceId` (the symbolic link) now
  travels in the capture-devices JSON for case-insensitive correlation.
  Hot-unplug downgrades the device to `connectionState "error"` + warning and
  stops the session thread — never a crash. The shell prefers the native path
  behind opt-in env `COREVIDEO_NATIVE_UVC=1`
  (`NativeUvcCapturePolicy` + `StudioViewModel.TryConnectNativeUvcCaptureAsync`)
  and falls back to the WinUI MediaCapture→shared-memory bridge on any decline;
  the shm bridge also still supersedes native frames for the same device id in
  `WinUiCaptureDeviceAdapter`. Tests: `UvcCaptureSupportTest.*` (negotiation,
  NV12/YUY2 repack, stable-id vectors, color hints, factory gating),
  `NativeUvcCapturePolicyTests` + parser tests (C#), capability
  `uvc-capture` asserted in `MediaCoreCommand.ProfileMirrorsNativeMediaCoreShape`.
  **Remaining:** live-camera validation on the rig (see runtime checklist in the
  branch report) + embedded UVC audio into the F2 mixer.

### Item 2 — AJA / Blackmagic (DeckLink)

- **Current.** `HardwareCaptureDeviceAdapters.cpp` does COM/SDK **enumeration**
  (DeckLink ~`:117`, AJA ~`:268`) and device control; `pollVideoFrames()` inherits
  the empty default. `MutableCaptureDeviceList` tracks state only. Routes resolve to
  `capture:<deviceId>` (`MediaCore.cpp:1977`) but render slates.
- **Spec (done =).** DeckLink and AJA inputs deliver real SDI/HDMI video frames
  (and embedded audio) into the F1 path and F2 mixer, with signal-present detection,
  input-format/standard reporting, and audio-sync offset actually applied to frame
  timing.
- **Plan.**
  1. DeckLink: implement `IDeckLinkInput` capture callback → F1 frame buffers
     (vendor SDK behind `COREVIDEO_WITH_DECKLINK`, currently enum-only). Convert
     UYVY/v210 → compositor format via the `I420Convert`-style stage.
  2. AJA: implement NTV2 autocirculate capture → F1 frame buffers behind
     `COREVIDEO_WITH_AJA`.
  3. Embedded audio → F2 mixer; apply `setAudioSyncOffset` as a real delay.
  4. Real `signalPresent`/format/dropped-frame counters in the snapshot; announce
     `decklink-capture`/`aja-capture` **only when frames flow** (today they are
     announced on enumeration, `MediaCore.cpp:92`).
- **Gate/tests.** Enumerate + capture one real device on a dev rig (the §53 build
  command stays green in-container). Extend capture-device tests for frame counters.
- **Flag.** `COREVIDEO_WITH_DECKLINK`, `COREVIDEO_WITH_AJA` (exist; expand scope).

### Item 3 — SRT (ingest + output)

- **Current.**
  - Ingest (`SrtIngestCaptureAdapter.cpp`): real libsrt listener/caller sockets,
    bind/accept, packet RX with `bytesReceived`/`packetsReceived` (~`:255`–`:339`),
    but `pollVideoFrames()` emits empty pixels (~`:102`) — "decode handled by the
    decoder stage" which **does not exist**; no audio polling.
  - Output (`SrtOutputSenderAdapter.cpp:7`): `createSrtOutputSender()` returns
    `nullptr` in both arms; never wired into `createDefaultModules()` (only RTMP is).
  - Flags: `COREVIDEO_WITH_SRT_INGEST`, `COREVIDEO_WITH_SRT_OUTPUT`,
    `COREVIDEO_HAS_LIBSRT`. See [`docs/srt-ingest-transport.md`](srt-ingest-transport.md).
- **Spec (done =).**
  - Ingest: received SRT packets are demuxed + decoded (F1 decoder stage) into real
    `VideoFrame.pixels` and PCM audio frames into F2, with caller/listener,
    stream-id, passphrase, latency honored.
  - Output: a real libsrt sender encodes the program frame (F1) + program audio
    (F2) to MPEG-TS and pushes over SRT; wired into the module set and the
    output-sender factory; real frames-sent/bytes/latency/retransmit counters.
- **Plan.** (1) Add FFmpeg `libavcodec` decode behind the F1 decoder seam for
  ingest. (2) Implement `SrtOutputSender : IOutputSender` (encode → TS mux → libsrt
  `srt_sendmsg2`), register in `createDefaultModules()` (`StubModules.cpp:591`),
  and route through `outputSender->sync()` (`MediaCore.cpp:2117`). (3) Audio frame
  polling on the ingest device.
- **Gate/tests.** Ingest: decode a known SRT stream to non-empty frames on a dev
  rig. Output: SRT push to a test ingest, counters increment. Stub: synthetic
  diagnostics unchanged.
- **Flag.** existing SRT flags + `COREVIDEO_HAS_LIBSRT`; FFmpeg runtime for decode.

### Item 4 — NDI sender

- **Current.** No native NDI sender (`createNdiOutputSender` does not exist).
  `ndiOutput.ts` validates source names + estimates bandwidth; `App.tsx` exposes NDI
  destination UI; `SyntheticOutputSender` reports fake `ndi` latency/bitrate
  (`StubModules.cpp:257`). `MediaCoreDestination` includes `"ndi"`
  (`protocol.ts:3`).
- **Spec (done =).** A real NDI sender registers an mDNS source name and submits the
  program frame (F1) + program audio (F2) via the NDI SDK
  (`NDIlib_send_create`/`send_video_v2`/`send_audio_v2`), honoring the validated
  source-name rules; real frames-sent/bitrate/connection counters in the snapshot.
- **Plan.** Implement `NdiOutputSender : IOutputSender` behind a new
  `COREVIDEO_WITH_NDI_OUTPUT` + dev gate; probe libNDI at runtime (mirror the RTMP
  FFmpeg-probe pattern, `RtmpOutputSenderAdapter.cpp:87`); register in the module
  set; announce `ndi-output` only when built. BGRA→UYVY conversion for NDI video.
- **Gate/tests.** NDI Studio Monitor receives the program source on a dev rig;
  source-name validation unit test already exists (`ndiOutput.ts`).
- **Flag.** `COREVIDEO_WITH_NDI_OUTPUT` (new).

### Item 5 — Recording (real A/V + ISO)

- **Current.** `MediaFoundationEncoderAdapter.cpp` writes **synthetic** BGRA via
  `fillSyntheticBgraFrame()`/`writeSyntheticProgramSample()` (~`:43`,`:218`),
  hardcoded `1920x1080`@`30` H.264 (`:32`–`:35`), **no audio muxed** (sets
  `audioCodec="aac"` but writes none), ISO is a synthetic frame count
  (`MediaCore.cpp:1854`). `StubRecordingEncoder` is metadata-only.
- **Spec (done =).** The recorder muxes the **real composed program frame** (F1+F3)
  + **real program audio** (F2) to MP4/MOV, honoring the configured resolution/FPS/
  codec/bitrate (not hardcoded), with correct A/V sync over a 30-min show. **ISO**
  produces one real per-source video file per selected participant from the F1
  per-source frame buffers, each with its routed ISO audio bus.
- **Plan.** (1) Feed `submit(ProgramFrame)` real pixels into the MF sink writer
  instead of regenerating synthetic samples. (2) Add an audio stream to the sink
  writer fed from the F2 program tap. (3) Drive width/height/fps/codec from the
  recording command, not constants. (4) ISO: open N sink writers from per-source F1
  buffers + per-ISO F2 taps; report real `isoFramesWritten`/bytes.
- **Gate/tests.** `native-recording-proof.mjs` passes against a real file: playable
  MP4, real duration, audio track present, no dropped frames; extend
  `EncoderRecordingSessionTest`. ISO files exist and are playable.
- **Flag.** `COREVIDEO_WITH_MF_ENCODER` (exists).

### Item 6 — Audio pipeline (this is F2, expanded)

- **Current.** See F2. Plus: ASIO/embedded-card capture warns it needs the dev
  adapter (`MediaCore.cpp:1602`,`1605`); VST inserts are configured but "scan-only"
  (`:1428`); monitor is `Beep()`.
- **Spec (done =).** F2 graph, plus real input sources and processing:
  - **ASIO** and **WASAPI** capture devices deliver real PCM into the mixer
    (dev-gated `COREVIDEO_WITH_ASIO`).
  - **Embedded capture-card audio** (DeckLink/AJA/UVC) routes into the mixer
    (delivered by items 1–2).
  - **VST3 insert host** processes real samples per participant/bus behind a dev
    VST bridge (`COREVIDEO_WITH_VST`); scan→instantiate→process.
  - **Real noise suppression / gate**, real limiter, real LUFS (from F2).
  - **Monitor** plays the MON bus on the selected device with the configured volume.
- **Plan.** Build F2 first (graph + buses + program/ISO taps + limiter + LUFS +
  WASAPI monitor). Then layer ASIO capture, the VST3 host, and wire embedded
  capture-card audio. Replace `playMonitorPulse()`/`Beep()` with real playback.
- **Gate/tests.** F2 gate; plus a VST round-trip (passthrough plugin alters
  samples), an ASIO capture loopback on a dev rig, and monitor output audible.
- **Flag.** `COREVIDEO_WITH_ASIO`, `COREVIDEO_WITH_VST` (new) + dev gate.

### Item 7 — RTMP (real program audio + codec compatibility)

- **Current.** Closest to real: spawns FFmpeg, pipes real `frame.preview.bgra` to
  stdin (`RtmpOutputSenderAdapter.cpp` `buildFfmpegArguments`~`:513`,
  `writeFrameToFfmpeg`~`:630`), encodes H.264/H.265/AV1 with nvenc/qsv/amf/cpu, FLV
  mux. **Audio is `anullsrc`** (silence, `:522`). Windows-only. H.265/AV1-over-FLV
  is unverified.
- **Spec (done =).** RTMP carries the **real program audio mix** (F2) instead of
  `anullsrc`, with A/V sync; codec/container selection is **verified compatible**
  (H.264/AAC over FLV guaranteed; H.265/AV1 either via a compatible muxer/RTMP
  variant — e.g. enhanced-RTMP — or gated/fallback to H.264 with a clear warning);
  macOS/Linux frame piping implemented (POSIX `fork`/`exec` + pipe).
- **Plan.** (1) Add a second FFmpeg input fed from the F2 program PCM tap (raw PCM
  over a second pipe / named pipe) replacing `-i anullsrc`. (2) Add a codec/container
  compatibility matrix: enforce H.264+AAC/FLV by default; for H.265/AV1 select the
  correct muxer/flags or warn+fallback. (3) Implement POSIX process+pipe path.
- **Gate/tests.** Live RTMP push to a test ingest plays with **real audio** in
  sync; an H.265/AV1 attempt either streams on a verified path or warns+falls back
  deterministically (unit-test the matrix). Validate via `validate-record-stream.mjs`.
- **Flag.** `COREVIDEO_WITH_RTMP_OUTPUT` (exists).
- **Status (2026-06-20) — codec/container compatibility matrix landed.** New pure
  `resolveRtmpCompatibility` (`modules/RtmpCompatibility.h`): H.264+AAC/FLV is the
  guaranteed baseline; H.265/AV1 either ride enhanced-RTMP (advisory warning) when
  opted in, or deterministically fall back to H.264 with a clear warning. The RTMP
  adapter now selects its FFmpeg video encoder from the resolved codec and surfaces
  the compatibility note in `runtimeDetail`. Covered by `RtmpCompatibility.*` (181
  native tests green); compiles under `-DCOREVIDEO_WITH_RTMP_OUTPUT=ON`. Remaining
  item 7: feed the real F2 program-audio tap into FFmpeg (replace `anullsrc`),
  enable the E-RTMP opt-in via a destination setting, and the POSIX pipe path —
  all dev-rig/runtime work.

### Item 8 — Scene source framing in native output (F3, framing)

- **Current.** The data carries it: `SceneRouteState`/`CompositorRenderPlanLayer`
  hold `fitMode`, `sourceScale`, `sourceOffsetX/Y`, `borderStyle/Color/Thickness`
  (`MediaCore.h:80`, `Interfaces.h:123`), copied into the render plan
  (`MediaCore.cpp:1995`). **D3D11 ignores them** — `drawLayer()` sets a viewport
  from `rect` and applies color grade only (`D3D11CompositorAdapter.cpp:429`); a
  16:9 source in a 4:3 rect just stretches. WinUI preview *does* honor framing
  (`SceneCanvasLayerViewModel.cs:40`), so preview and program diverge.
- **Spec (done =).** D3D11 (and the CPU stub) apply per-source framing exactly:
  **fit** (letterbox), **fill** (cover/crop), **stretch**; **source zoom**
  (`sourceScale`) and **offset** (`sourceOffsetX/Y`) as a source-texture
  UV/transform; **borders** (style/color/thickness) drawn around the layer —
  matching the WinUI preview pixel-for-pixel.
- **Plan.** In `drawLayer()`/the vertex+pixel shaders, compute source UVs from
  `fitMode` + source aspect vs. rect aspect, then apply `sourceScale`/offset as a UV
  transform; add a border pass. Mirror the math into `CpuNoopCompositor` so the stub
  preview is correct too. Keep `CompositorLayout.h` helpers as the rect source.
- **Gate/tests.** Extend `D3D11CompositorTest`: a known source + rect + each fit
  mode + zoom/offset produces the expected sampled region (assert on read-back
  pixels). Visual parity check vs. WinUI on a dev rig.
- **Flag.** `COREVIDEO_WITH_D3D11` (exists).

### Item 9 — Overlays / lower thirds / captions (F3, raster)

- **Status (2026-07-02): SHIPPED and rig-validated.** Overlays, lower-thirds,
  and captions render real content from a shared layout resolver
  (`native/src/modules/OverlayTileRaster.{h,cpp}`
  `computeOverlayTileLayout`): brand band + accent bar + image slot + text-line
  geometry defined once. The CPU preview (`ProgramFramePreview.cpp`
  `drawOverlayContentBgra`) rasters it as a full-ASCII 5x7 bitmap-font tile with
  a deterministic image placeholder; the Windows D3D11 compositor
  (`D3D11CompositorAdapter.cpp` `rasterOverlayTexture`, gated with
  `COREVIDEO_WITH_D3D11`) renders the same layout with DirectWrite/D2D
  antialiased text + a real WIC `imageUri` decode, zero-copy into a GPU texture
  via a D2D DXGI-surface render target (premultiplied blend state; pipeline
  state snapshot/restore around EndDraw), signature-cached
  (`overlayContentSignature`). Validated on the dev rig: GPU pixel tests in
  `D3D11CompositorTest.cpp` + live app smoke at 60fps. keyPhase animation stays
  a composite-time transform (rasters cache across a build-in/out sweep).
  Tests: `OverlayTileRasterTest.cpp` + the `StubCompositor` overlay pixel tests
  + the `D3D11Compositor` DirectWrite tests.
- **Original current-state (2026-06-20, superseded).** Overlays are tracked as a count of IDs (`setOverlayAsset` ignores
  text/image/keyer/keyPhase, `MediaCore.cpp:885`) and placed as fixed-position
  `0.92`-alpha **solid colored rects** (`D3D11CompositorAdapter.cpp:365`–`383`);
  captions update `captionText_`/`captionSpeaker_` but are **never rendered**
  (`MediaCore.h:200`). Protocol already carries the rich fields
  (`set-overlay-asset` text/imageUri/title/org/keyPosition/keyPhase/keyer;
  `push-caption-cue`).
- **Spec (done =).** Real compositing of overlay content:
  - **Text** rendering (lower-third title/org, caption text + speaker) via a text
    rasterizer (DirectWrite on Windows) into a texture, brand-styled (BrandKit
    colors/fonts already pushed via `set-brand-kit`).
  - **Image** overlays: decode `imageUri` → texture, aspect-correct.
  - **Animated lower-third keying**: honor `keyPhase` (building-in/on-air/
    building-out) as a real animated transform/alpha, with upstream/downstream
    `keyer` placement.
  - **Captions** rendered as a styled lower band with speaker attribution when
    `captionEnabled`.
- **Plan.** Add an overlay-raster stage to the compositor: DirectWrite/D2D text →
  texture, WIC image decode → texture, an animation clock driving `keyPhase`, and a
  keyer composite pass. Feed BrandKit styling. Mirror minimal placement in the stub
  for tests. Replace the solid-color overlay branch with textured draws.
- **Gate/tests.** Overlay/caption layers render real text/image pixels (assert
  non-uniform overlay region in read-back); keyPhase animates over time; brand color
  applied. Extend compositor tests.
- **Flag.** `COREVIDEO_WITH_D3D11` (exists); optionally `COREVIDEO_WITH_TEXT` if the
  rasterizer is separable.

### Item 10 — Automation / AI director

- **Current.** 100% TypeScript heuristics, zero ML, no native involvement:
  `buildMagicScene()` (`src/engine/mockEngines.ts:94`) builds 5 fixed scenes by
  role/screen-share/health; `autoProductionDirector.ts` (`selectAutomationRule`
  `:118`, `stabilizeSceneRecommendation` `:150`, hold windows `:225`) chooses
  hold/queue/take by participant count + screen-share with anti-thrash holds.
  `aiStudio.ts` is rule-based string templating. No `anthropic`/`openai` usage.
- **Spec (done =).** A director that is robust and (optionally) model-backed while
  staying deterministic-by-default:
  - Keep the heuristic engine as the **always-on, offline fallback** and the
    arbiter of safety/anti-thrash (it already encodes good director instincts).
  - Add an optional **intelligence service** (behind the `AiProductionEngine`
    contract, `src/engine/contracts.ts:66`) that consumes richer signals (speaker
    turns, sentiment, content of screen share, engagement) and proposes
    scene/transition decisions, which the heuristic stabilizer still gates.
  - The model integration uses the latest Claude models via the Anthropic API in a
    `services/` backend (the renderer/native core stay shell-agnostic; the engine
    calls the service through the existing contract, never embeds a client in
    `src/engine/`).
- **Plan.** (1) Define the richer `MagicSceneRequest`/`AutoProduction` signal inputs.
  (2) Implement a `services/`-hosted director that returns scene/transition
  proposals; the renderer's `AiProductionEngine` calls it with a heuristic fallback
  on timeout/error. (3) Keep `autoProductionDirector` as the deterministic gate
  (confidence thresholds + holds). (4) Telemetry: log proposal vs. taken for tuning.
- **Gate/tests.** Existing director/Magic-Scene tests stay green; new tests assert
  the heuristic fallback fires on service failure and the stabilizer still overrides
  unsafe proposals. **Product decision required** (see §6) on whether AI runs
  locally vs. cloud and the privacy posture for meeting content.
- **Flag.** Feature-flag + license tier (`setAndForget` entitlement already exists,
  `licenseClient.ts:46`); service URL/key via config, not committed.

---

## 4. Sequencing & dependency graph

```
            ┌────────────── F1: real frame-pixel transport ──────────────┐
            │   (+ decoder seam for compressed sources)                   │
            ▼                                                             ▼
   Item 1 UVC   Item 2 AJA/DeckLink   Item 3a SRT ingest        Item 8 framing  ┐
        │            │                      │                   Item 9 overlays  ├ F3
        └──────┬─────┴───────────┬──────────┘                        │          ┘
               ▼                 ▼                                    ▼
        (sources feed the compositor)                    correct composed ProgramFrame
                                                                     │
   ┌──────────── F2: real program audio bus (PCM) ───────────┐      │
   │  (= Item 6: buses, limiter, LUFS, ASIO, VST, monitor)   │      │
   └───────┬───────────┬───────────┬───────────┬─────────────┘      │
           ▼           ▼           ▼           ▼                     ▼
     Item 7 RTMP  Item 5 Record  Item 4 NDI  Item 3b SRT out  ◀──────┘
     (real audio) (real A/V+ISO) (sender)    (sender)
                                                          
   Item 10 Automation/AI — parallel track, no native dependency
```

**Recommended phase order:**

- **Phase A (foundations):** F1, F2, F3. Nothing downstream is truly real without
  these. F3 (items 8+9) can proceed in parallel with F1/F2 since it only needs the
  render-plan it already receives; it just needs F1's real pixels to be visibly
  correct.
- **Phase B (sources):** Items 1, 2, 3a — each is "implement `pollVideoFrames` for
  real" on top of F1 (+ decoder for 3a).
- **Phase C (outputs):** Item 7 (smallest delta — swap `anullsrc` for F2 tap),
  then 5, 4, 3b — each consumes F1+F3 frames and the F2 program/ISO taps.
- **Phase D (intelligence):** Item 10, runnable any time; schedule after a usable
  live path exists so there is real signal to direct.

This ordering means the first visible win (Phase A + Item 7) gives a **real,
correctly-composited program with real audio streaming over RTMP** — the alpha
exit bar from [`docs/alpha-plan.md`](alpha-plan.md) §2.

---

## 5. Cross-cutting acceptance gates & test strategy

- **Stub build stays green.** `cmake -S native -B native/build -DCOREVIDEO_STUB=ON &&
  cmake --build native/build && ctest --test-dir native/build` in-container after
  every slice. `npm run test:native-core` green.
- **Parity gate.** `npm run typecheck` + `ContractParityTest` for every new
  command/snapshot field (Protocol.h ↔ protocol.ts ↔ nativeMediaCoreProtocol.ts).
- **Per-layer tests** (the repo's house style): backend command → snapshot
  (`mediaCore.test.ts`/gtest), command builder present/absent under condition,
  mock sync engine populates the new snapshot field, App.tsx readout.
- **Real-path proofs (dev rig):** `native-recording-proof.mjs` (real A/V),
  `validate-record-stream.mjs`, `validate-live-zoom.mjs`, plus the Windows-only
  `test:native-shell*` smoke suites.
- **Capability honesty:** `profileCapabilities()` announces a capability **only when
  the real adapter is built and producing data**, so the diagnostics/support bundle
  never over-reports.
- **README inventory** updated under "Current Slice" per completed slice.

---

## 6. Risks & open product decisions

- **UVC ingest path (Item 1):** native MF capture in C++ vs. bridging WinUI frames.
  Native C++ is cleaner (one source of truth) but duplicates device enumeration
  WinUI already does. **Decide before Item 1.**
- **H.265/AV1 over RTMP (Item 7):** standard FLV does not carry HEVC/AV1
  interoperably; enhanced-RTMP support is ingest-dependent. **Decide:** verified
  enhanced-RTMP path vs. gate these codecs to SRT/recording and fall back to H.264
  for RTMP with a clear operator warning.
- **Audio matrix semantics (Item 6/F2):** crosspoint on/off + gain vs. full fader
  matrix changes DSP scope (echoes `alpha-plan.md` §5). **Lock before building F2.**
- **AI director (Item 10):** local vs. cloud inference and the privacy posture for
  Zoom meeting content (audio/video/transcript leaving the machine). This gates
  whether the service can see content at all. **Product + privacy decision.**
- **Vendor SDK availability:** DeckLink, AJA NTV2, NDI, libsrt, FFmpeg, and a VST3
  SDK must be staged on dev/CI rigs; none ship in-container. Each stays behind its
  dev gate so Linux CI is unaffected.
- **Windows-only validation:** D3D11, Media Foundation, WASAPI/ASIO, and the RTMP
  pipe path can only be visually validated on Windows; pair every such slice with a
  Windows smoke pass.
```
