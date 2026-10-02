# A failing stream must name its own reason (owner report, 2026-09-12)

_Moved verbatim from `CLAUDE.md` (#737). Paths are relative to the repo root._

The owner could not start a stream and was told *"RTMP output failed. Check the
server URL, stream key, and network."*, so they re-entered credentials that were
already correct. FFmpeg had written the real reason to its stderr temp file on
the FIRST attempt:

```
[out#0/flv] Error opening output rtmp://a.rtmp.youtube.com/live2/<key>: I/O error
Error opening output files: I/O error
```

The destination refused the connection. **The stream key in that URL is the same
key that worked two minutes later** — it decrypts cleanly from
`production-output-preferences.json` (DPAPI, v12) and was never wrong. Nothing
read that file, so the one line naming the cause was thrown away, and the failure
was undiagnosable from the app alone. This is the gap #473/#494 closed for the
media decoder, and the rule this file already states twice for this very sender:
**read FFmpeg's own stderr before theorising.**

- **`modules/FfmpegSenderDiagnostics.h`** is the ONE place FFmpeg's stderr becomes
  an operator sentence (the `ZoomJoinFailureMessage.h` shape: pure, header-only,
  testable without a process). `RtmpOutputSenderAdapter::ffmpegStderrTail()` reads
  it on FAILURE paths only — never per frame — and every `ffmpeg-exited` /
  `ffmpeg-write-failed` message now carries `ffmpeg: <reason>`.
- **THE SECRET IS STRIPPED AT THE SOURCE, not by the snapshot redactor.**
  FFmpeg echoes the FULL output URL, stream key included, and `lastError` reaches
  `/snapshot`, the support bundle and the log. `redactFfmpegDiagnostics` replaces
  the configured stream key and SRT passphrase by value (so it cannot depend on
  URL parsing), and the adapter now holds `configuredPassphrase_` for no other
  purpose. The HOST deliberately survives — it is the diagnostic, not a secret.
  A secret under 6 characters is NOT used as a pattern: it would match inside
  ordinary words and shred the message the redaction exists to protect.
- **The tail is trimmed from the FRONT.** FFmpeg names the fatal reason LAST, so
  budgeting by cutting the end drops the only line that matters. Caught by
  `OnlyTheTailIsKeptWhenTheLogIsLong` on the first green run.
- **An ABSENT tail leaves the generic sentence exactly as it was.** Inventing
  detail we do not have is the same lie pointing the other way.

**A stream start RACES Program's first pixels, and warming is not failing.** Two
of the owner's three attempts that day died on `frame-pixels-missing` ("RTMP
sender is waiting for composed BGRA program pixels") and the third succeeded with
nothing changed — so a healthy configuration read as "the encoder will not
start". `TransportStatusFormatter.IsStreamingStartStillWarming` is the pure
classifier, and it is deliberately NARROW: only the program-pixel readiness case
waits, and an explicit refusal outranks it, because waiting on a real failure
only delays an honest answer. The wait window is ~5 s while warming and the
original ~1.2 s otherwise; if it never produces a frame the SAME honest message
is returned rather than a different invented cause.

**THE EAGER BAIL EXISTED IN THREE PLACES, and only the LIVE re-test found the
third.** `TryFormatStreamingStartHealthFailure` is consulted before
`WaitForStreamingStartProofAsync` on the primary path, inside that wait, AND
again on the backpressure RETRY path (`RetryStreamSyncAsync`). Fixing the first
two left the retry path still rolling back on the first poll — and a start
deferred for sync backpressure comes through exactly there, which is what the
owner's session hit. Verified live 2026-09-12 against a refusing endpoint: the
process log now carries `stream: toggle requested` with NO health-proof failure
and NO rollback, where the previous build rolled back every time.

**Two message-ladder defects fixed with it** (`TransportStatusFormatter`), both
from sniffing prose instead of reading the wire result code:

1. **A destination refusal no longer leads with the stream key.** `error opening
   output` / `i/o error` / `connection refused` now read "The streaming
   destination refused the connection. Check the destination is live and
   accepting (a YouTube/Twitch stream has to be started there first), then the
   stream key and network." Compact readout: `Destination refused`.
2. **Every FFmpeg exit used to report "Program video is not ready."** The ladder
   matched the substring `"program frame"`, which also appears in *"FFmpeg
   process exited before accepting program frames"* — so a dead encoder sent the
   operator to put a source on Program. The readiness branch now requires
   `waiting for` as well, because the sender is only reporting readiness when it
   says it is WAITING; an exit is reporting a death. Found by a test that failed
   for the wrong reason, which is the only reason it was found at all.

**STOP THE PROCESS, THEN READ THE STDERR.** A failed stdin write is observed the
instant the pipe breaks, which is BEFORE FFmpeg has flushed the line saying why.
Measured live 2026-09-12 against a refusing endpoint: reading at the write
failure gave a 72-byte file holding only "Guessed Channel Layout: stereo", and
the file NEVER gained its error lines because the process was killed first.
`stopFfmpegProcess()` closes stdin (FFmpeg's EOF) and waits for exit, so the file
is complete once it returns — after the reorder the same run produced the whole
chain: `Connection to tcp://... failed` | `Cannot open connection` | `Error
opening output ... Error opening output files`.

**The proof line no longer contradicts itself.** Where FFmpeg never started,
`ffmpegVideoEncoder` reported `ffmpegVideoEncoderFor(configuredVideoCodec_, ...)`
— the RAW codec — so a proof whose own `runtimeDetail` said "falling back to
H.264" simultaneously reported `av1_nvenc`, sending a reader diagnosing a stream
failure to chase an encoder that is never selected. It goes through
`resolveRtmpCompatibility` now.

Tests: `native/tests/FfmpegSenderDiagnosticsTest.cpp` (redaction, bounding,
composition — the redaction case uses the owner's real stderr) and
`StudioViewModelAudioStatusTests.FormatStreamingFailureStatus_ADestinationRefusalDoesNotLeadWithTheStreamKey`.
**Verified live in the test meeting, 2026-09-12**, against `rtmp://127.0.0.1:1935`
with nothing listening and a marker stream key: `lastError` carried FFmpeg's full
refusal chain, and the marker key appeared **twice in FFmpeg's own temp file and
zero times** in `media-core.log`, `launch.log`, the send-proof JSONL and
`/snapshot`.
