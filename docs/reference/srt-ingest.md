# SRT ingest (contribution feeds IN — video + embedded audio, 2026-08-07)

_Moved verbatim from `CLAUDE.md` (#737). Paths are relative to the repo root._

SRT is required in BOTH directions for a pro AV product; delivery shipped first
(`SrtFfmpegArgs.h`), this is the INGEST half. A remote guest/encoder pushes an
MPEG-TS/SRT stream at us and it becomes an ordinary capture source.

- **Shape:** one **ffmpeg decoder subprocess per channel**
  (`modules/SrtIngestCaptureAdapter.cpp`), never libsrt in the core. Video comes back
  as raw **BGRA on stdout** at the channel's configured size/rate and merges into
  `videoFrames` keyed `capture:<deviceId>` — so scenes, multiview, ISO, recording and
  every sender treat it exactly like a camera. Decoders run under a **job object**
  (`JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE`) so a core crash can't orphan an ffmpeg still
  holding the SRT port. **The OUTPUT senders do the same, in their OWN job**
  (`outputJobObject`/`adoptOutputChild` in `modules/RtmpOutputSenderAdapter.cpp`,
  shared by RTMP and SRT egress plus the encoder-availability probe): an orphaned
  EGRESS ffmpeg keeps PUBLISHING to a live destination after the app is gone, which
  is worse than a held port. Separate jobs because ingest's is a file-local static in
  another TU, and because outputs may later need group-killing without touching
  ingest. Assignment is BEST EFFORT — a failure logs loudly and the stream still
  starts. The job never kills anything on its own (the handle is a leaked
  process-lifetime static), so `stopFfmpegProcess()` restarts are unaffected; the
  replacement child is simply assigned to the same job. **Any new long-lived child
  process spawned by the core must be adopted into a KILL_ON_JOB_CLOSE job.**
- **Embedded audio is a SECOND output on the same ffmpeg** — `-map 0:a:0? -vn -f f32le
  -ar 48000 -ac 2` into a **Windows named pipe** the adapter serves
  (`\\.\pipe\corevideo-srt-ingest-audio-<pid>-<n>`; POSIX hands the child an inherited
  fd and uses `pipe:3` — no FIFO file, **not verified on hardware**), drained by a reader thread into
  a ~1s cap (drop-oldest) and emitted from `pollAudioFrames` keyed
  **`capture:<deviceId>` — the same id as the video**, which is what makes it land in the
  existing routing/metering/ISO paths with no special-casing. A contribution feed carries
  its guest's audio inside the transport with no OS device to pair, so it cannot use the
  WASAPI capture-audio path.
- **NO SHELL, EVER — the decoder is an argv VECTOR** (`buildSrtIngestArgv` in
  `SrtFfmpegArgs.h`, POSIX `execvp` / Windows quoted `lpCommandLine` with
  `lpApplicationName` pinned). `buildSrtUrl` deliberately tolerates a **pasted**
  `srt://host:port` because pasting a remote contributor's connection string is the
  intended way to add an ingest — so the URL is attacker-influenced by design. It must
  stay exactly one argument; never rebuild this as a command string.
- **`-y` IS LOAD-BEARING.** FFmpeg sees the audio pipe as an existing FILE and
  interactively prompts `Overwrite? [y/N]`, then EXITS — killing the whole decoder and
  taking **video** down with it. The symptom is "SRT ingest stopped working entirely"
  when you touch the audio output. Never drop `-y` from the ingest command.
- **THE WRAPPER LAW (this bit twice now).** `WinUiCaptureDeviceAdapter` wraps the whole
  capture composite, and `ICaptureDevice`'s defaults are permissive — inheriting the
  default `pollAudioFrames` returned `{}` and SILENTLY swallowed every ingested audio
  frame while video flowed perfectly. Identical shape to the old 1-arg `connect()` bug
  that caused pink tiles. **Any new `ICaptureDevice` method must be forwarded in
  `WinUiCaptureDeviceAdapter`** — the shell bridge carries no audio, but the devices it
  wraps do.
- **Proof:** `node scripts/validate-srt-ingest.mjs [--seconds N] [--port N] [--keep]`
  publishes `testsrc` + a 440Hz `sine` over real SRT and judges **decoded pixels and
  decoded audio in the program recording** — mean luma and audio peak — plus the core's
  own muxer proof counts. It judges output, not status strings, because the adapter this
  replaced counted bytes and threw the packets away: it reported "receiving" while
  emitting frames with NO PIXELS (correction published in `docs/spine-status-2026-08-06.md`).
- **Harness gotcha worth keeping:** `stop-recording-session` returns BEFORE the async
  encoder sink writes the MP4 **moov atom**, and file size stabilises well before the moov
  lands — so a size-based wait reads an unfinalized file that decodes as **zero frames**,
  which looks exactly like a dead feed. Wait until **ffprobe** can read a duration, with
  the core still alive, before killing it.
