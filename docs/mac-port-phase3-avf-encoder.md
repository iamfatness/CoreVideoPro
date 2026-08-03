# macOS port Phase 3 — AVFoundation/VideoToolbox recording encoder

Status: increments 1 AND 2 landed — program A+V (full-res via
wantsFullProgramReadbackForRecording) plus the ISO writers: lazy open at
first frame (NV12 for Zoom I420 / BGRA for capture), per-source dedup,
isoAudioAdvance silence-fill, hasAudio gating (video-only ISOs carry no
fabricated track), independent finalize, session subfolder + manifest via
the shared RecordingArtifactNaming.h helpers, iso-recording capability
advertised. Remaining parity gap vs the six real-MF tests: none functional;
MF-side helper dedup still waits on a verified Windows build. Twin of
`MediaFoundationEncoderAdapter.cpp`; boundary map performed 2026-08-03.

## What the boundary map established

- `IEncoderSink` has only 4 pure-virtual methods (`configureRecording`,
  `start`, `submit`, `session`); ISO/audio/latency methods default no-op, so
  the adapter lands incrementally.
- **Portable and reused verbatim:** `RecordingPtsClock.h` (shared-epoch PTS,
  per-source video dedup, `isoAudioAdvance` silence-fill — zero OS types) and
  `AsyncEncoderSink` (the writer-thread wrapper with per-Kind drop budgets).
- **To extract for sharing in increment 2** (today trapped in the MF
  adapter's anonymous namespace): `sanitizeForFilename`,
  `sessionTimestampFolder`, `jsonEscape`, `i420ToNv12`, `toStereo`. They are
  only needed by the ISO folder scheme (`<prefix>-<yyyymmdd-hhmmss>/`,
  `ISO-NN-<SafeName>.mp4`, `manifest.json`), which must stay byte-identical
  across platforms; increment 1 records a flat program artifact
  (`<prefix>-program-0.mp4`, the stub's naming) and does not touch the MF
  adapter at all — the Windows dev configuration has no CI, so MF-side
  dedup waits for a verified Windows build.
- **The full-resolution fix (M4 from the compositor spec):** the MF adapter
  reads ONLY `ProgramFrame::preview` (320x180) into a 1080p writer — a
  pre-existing Windows oddity. The mac adapter reads `programFullBgra` when
  present and falls back to `preview`. Because `fullProgramReadback` is only
  set for vcam/output today, `ICompositor` gains
  `wantsFullProgramReadbackForRecording()` (default false; Metal returns
  true) so MediaCore requests the full readback during recording WITHOUT
  spinning the Windows vcam tap for nothing — on Apple-silicon shared memory
  the extra readback is a cheap getBytes.
- `encoderName` is a load-bearing adapter-presence signal in tests; the mac
  sink reports `"videotoolbox"` (AVAssetWriter's encode path IS VideoToolbox).

## Design (increment 1: program A+V)

`native/src/modules/AVFoundationEncoderAdapter.mm`, gated
`!COREVIDEO_STUB && COREVIDEO_ENABLE_DEV_ADAPTERS && COREVIDEO_WITH_AVF_ENCODER`
(CMake mirror of the MF gate, APPLE-only, FATAL_ERROR otherwise). Factory
`createAVFoundationEncoderSink()` with the same null-twin pattern;
`createDefaultModules` tries MF then AVF.

- One `Mp4Writer`-shaped class over `AVAssetWriter`: H.264
  (`AVVideoCodecTypeH264`, target bitrate honored) + AAC. BGRA frames arrive
  via `AVAssetWriterInputPixelBufferAdaptor` (`kCVPixelFormatType_32BGRA`),
  audio via `CMSampleBuffer` (float→int16 like the MF writer).
- **The #286 lesson transfers structurally:** `AVAssetWriter` cannot add
  inputs after `startWriting`, and cannot be restarted after
  `finishWriting` — so `open()` REBUILDS the writer object outright (which is
  the honest analogue of "open() resets ALL per-session state"). The
  double-start test mirrors the MF one.
- PTS: `RecordingPtsClock` verbatim; caller-supplied steady-clock origin;
  `setAudioContentLatencySamples` latches into the clock (atomic).
- Loud failures: uncreatable folder → `recordingWarning` + program refusal
  identical to MF semantics; writer errors surface in `recordingWarning`
  (rate-limited stderr), never silent.
- Flat program artifact in increment 1; session subfolder + manifest arrive
  with the ISO writers in increment 2 (via the shared helpers).

Increment 2: ISO writers (lazy open at first frame, NV12 vs BGRA input per
source, per-source dedup, `isoAudioAdvance` silence-fill, independent
finalize, `hasAudio` gating) mirroring the six real-MF tests.

## Tests (increment 1)

`AvfEncoderRecordingTest.cpp`, compiled under the AVF gate, skip pattern =
the MetalCompositorTest macro (factory null → log + early-return; the
vendored gtest has no GTEST_SKIP):

- program session writes a playable MP4: `recordingVideoFrameCount > 0`,
  `recordingAudioPacketCount > 0`, bytes on disk > 0, artifact path correct.
- double-start keeps muxing audio (the #286 mirror).
- full-res: submitting a frame with `programFullBgra` (640x360) records at
  640x360, not the preview size (ffprobe-free check: configured writer dims).
- bad target folder → loud warning, `recordingStatus` reflects failure
  honestly.
- silence/latency: `audioPts` shared-epoch behavior is already covered by the
  portable RecordingPtsClock tests; not re-proven here.
- CI: the `native-metal-macos` job gains `-DCOREVIDEO_WITH_AVF_ENCODER=ON`
  so the AVF tests run on macos-latest.
