# A media source with no decoder is LOUD now (#473, 2026-09-12)

_Moved verbatim from `CLAUDE.md` (#737). Paths are relative to the repo root._

Owner, installed beta, 2026-09-10: a ProRes Show Input drew a placeholder for a
whole session. `media-core.log` carried the compositor's `media layer
'media:...' has NO matching frame` every 5 s and **nothing else** — no ffmpeg
process, no reason. The same code in a dev launch decoded it fine.

**What the investigation actually established** (worth keeping, because it rules
out the obvious guesses):

- Media Foundation genuinely cannot decode this ProRes:
  `SetCurrentMediaType(RGB32)` returns `MF_E_TOPO_CODEC_NOT_FOUND` (0xC00D5212).
  Falling through to FFmpeg is correct.
- FFmpeg resolution is NOT the difference. `COREVIDEO_FFMPEG_BIN_DIR` is a
  persistent USER variable on the owner's box (`C:\ffmpeg\bin`), so both launches
  get it; with it cleared, `SearchPathW` still finds the app-local `ffmpeg.exe`
  that `StartCoreVideo.cmd` installs on first run. Diffing the launcher
  environments is a dead end.
- The app-local runtime was installed an hour BEFORE the failing session.
- The exact command the core builds decodes the real asset: exit 0, a full
  1920x1080x4 BGRA frame.

**So the trigger was never identified — because the code could not say.** Three
silent surfaces, all closed here:

1. **FFmpeg's stderr went to `NUL`** (`startup.hStdError`). With `-loglevel error`
   FFmpeg names the cause in one line; we threw it away. It now goes to a
   per-decoder temp file, and the last 2 KB are logged when the decoder dies.
   A temp file and NOT the stdout pipe: stdout carries raw BGRA frames, and a
   stderr line spliced into it would corrupt one. Same lesson the SRT sender
   already carries — read FFmpeg's own stderr before theorising.
2. **A spawn that succeeded and then died reported NOTHING.** `launch()` returned
   true, `readLoop` read 0 bytes and broke, and the source showed a placeholder
   forever. `reportEarlyExit()` now fires once per decoder, with the child's exit
   code and its stderr tail — and only when we did not ask it to stop, so a
   pause, a resume or a teardown stays quiet.
3. **Every `openVideoReader` failure reached only `warnings_`**, which never lands
   in `media-core.log`. The branch decision, the resolved ffmpeg path and which
   of the three rules found it, and a total failure to get any decoder, are all
   `[media-decoder]` lines now. So is the **16-decoder cap refusal** in
   `OwnedMediaFrameSource`, which from outside looks exactly like a broken
   decoder (rate-limited per source so a busy show cannot flood the log).

**Verified by reproducing the class, not by a unit test.** A truncated `.mov`
driven through the real core previously produced only the compositor line; it now
produces the branch, `ffmpeg started pid=... exe=... (via COREVIDEO_FFMPEG_BIN_DIR)`,
and `ffmpeg STOPPED DELIVERING exit=... ffmpeg: moov atom not found`.

**The original ProRes defect is NOT fixed and #473 stays open.** It did not
reproduce on the current install. The next occurrence will name itself in one
line; until then there is nothing honest to fix.
