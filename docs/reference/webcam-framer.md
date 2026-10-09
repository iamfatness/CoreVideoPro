# Built-in webcam framing overlay

Owner request and acceptance: [#839](https://github.com/iamfatness/CoreVideoPro/issues/839).

The virtual-camera options flyout contains **OH Framer (webcam only)**. It is off
by default and persists with production output preferences. Turning the camera
off retains the requested setting but releases its GPU resources once any
in-flight preparation finishes. No file import
is necessary: `native/assets/OH_New_Framer.png` is embedded byte-for-byte by CMake.
Its clear head-shaped opening, translucent black framing mask, gray guides and
white labels
retain the supplied alpha. Video continues underneath; these are framing guides,
not a crop or replacement background. The native 1920x1080 camera fills the frame.

## Output boundary

The compositor's latest NV12 is also consumed by recording/output paths. It must
remain immutable and clean. `AsyncVirtualCameraPublisher` forks pixels on its
existing bounded latest-frame publication worker, immediately before calling the
camera backend. `GpuWebcamFramer` uploads packed NV12, blends the embedded RGBA
texture in a D3D11 compute shader, and reads back a separate NV12 result for the
camera shared-memory ABI. Program, Preview, multiview, recording and streaming
never receive that result. Program sequence and delivery timestamp are preserved.

This stage adds one NV12 GPU upload and readback on the camera worker. It does not
perform CPU per-pixel overlay or a full BGRA round trip. PNG decode/upload happens
once per active stage on one bounded preparation thread; clean video continues
while the stage is preparing. GPU resources are then owned exclusively by the
camera worker and reused at fixed dimensions. The render
thread neither waits for its GPU work nor calls the camera backend. A slow worker
can still replace pending camera frames; isolation is not a promise of unlimited
GPU capacity. Camera delivery must be measured independently.

## Alpha, range and mirror

The camera is studio-range Rec.709 NV12. Overlay RGB is converted to luma/chroma
in the shader, with black at Y=16 and white at Y=235. A zero-alpha sample leaves
its source byte exactly unchanged. Each 2x2 chroma block averages alpha-weighted
overlay chroma contributions. The supplied image contains neutral gray/black/white.
The guide coordinates are reversed before the backend's horizontal mirror, so
video mirrors while guide text and placement remain readable and unchanged.

## Failure and evidence

Snapshots distinguish requested `framerEnabled` from `framerState` (`off`,
`waiting`, `active`, `unavailable`). `framerWarning`, `framerFrames` and
`framerFailures` expose the actual stage outcome. Decode/GPU/device failure is
latched for that activation and publishes clean camera video, with a visible
warning. Toggle off/on or restart the camera to retry. Unsupported builds report
unavailable rather than claiming an overlay was applied. The stage's counters
are not proof of receiver playback.

Tests cover transparent pass-through, translucent blending, visible guides,
mirror readability, immutable clean input, malformed input, command routing and
settings migration/defaults. GPU processing cost is reported separately from
cadence. A matched run must preserve the resolution, active outputs and source
workload with overlay off/on. Any missed scheduled output deadline fails cadence
acceptance. Installed camera receiver pixels and real-show delivery are separate
evidence, not inferred from a shader test or average FPS.

## Worker cadence reference

Build `corevideo-webcam-framer-qa` in Release and run the off/on trials separately,
with no other GPU validation workload running:

```powershell
cmake --build native/build-dev --config Release --target corevideo-webcam-framer-qa
native/build-dev/corevideo-webcam-framer-qa.exe 60 off artifacts/framer-off
native/build-dev/corevideo-webcam-framer-qa.exe 60 on artifacts/framer-on
```

This drives the production camera publication worker at 1080p60 with an immutable
synthetic NV12 input and a test backend. CSV retains every published sequence,
scheduled deadline, actual producer submission, arrival time and sampled pixel
result. A separate producer CSV retains submissions even when the latest-frame
slot replaced them. JSON reports gaps,
pending replacements, startup replacements, missed next-tick deadlines and worst
intervals. The two-second preparation window is separate from the measured run;
startup replacement counters are retained. This does not write the camera SHM or
qualify OS receiver playback, Program/recording load or a real meeting. Keep those
boundaries unverified until their corresponding tests run on the candidate.

[Development evidence](../qa/webcam-framer-development-2026-10-08.md) records the
functional results and the retained cadence failures.
