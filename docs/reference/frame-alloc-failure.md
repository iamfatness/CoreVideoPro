# A frame that cannot be allocated is DROPPED, not fatal (#728, 2026-10-01)

_Moved verbatim from `CLAUDE.md` (#737). Paths are relative to the repo root._

The core died three times in 2.5 minutes in an owner session. All three dumps had one
stack: `FfmpegVideoDecoder::readLoop` -> `make_shared<vector<uint8_t>>` (one 1920x1080
BGRA frame, about 8.3 MB) -> `std::bad_alloc` -> no handler on that thread -> `terminate`.

- **The process was not out of memory; Windows was.** The core held about 1 GB. System
  event 2004 ("low virtual memory condition") was logged in the same second Program froze.
- **What changed:** `modules/MediaFrameReadStep.h` reads one frame per call with a
  non-throwing allocator. A failed allocation still reads that frame's bytes off the pipe
  (into a 64 KB stack buffer, because the pipe has no framing) and returns `Dropped`. The
  clip holds its last picture and logs `[media-decoder] OUT OF MEMORY ... dropped=N`
  (first drop, then at most every 5 s).
- **The other frame paths are guarded the same way (audit, same day).**
  `core/FrameAllocation.h` has `tryMakeFrameBuffer`, `tryResizeFrameBuffer` and a per-site
  `FrameAllocationFailures` counter that logs `[frame-alloc] OUT OF MEMORY at <site>` on the
  first failure and at most every 5 s. Each site drops that one frame and keeps the last:
  SRT/RTMP ingest, native UVC, screen capture (WGC), the Media Foundation clip reader, the
  Zoom frame store and SHM read, the WinUI capture bridge, browser sources, NDI receive, the
  virtual-camera tap, the program-buffer NV12 tap and the ISO GPU conformer. A still that
  cannot be decoded for lack of memory is a failed decode (placeholder stays).
- **Not guarded:** small allocations (strings, JSON, queue nodes, shared_ptr control blocks)
  anywhere; reused scratch buffers on the output side (NDI send convert); `src/zoom/`
  (`ShmFrameReader`, `I420Convert`); every macOS `.mm` path; thread creation (see below).
- **Run under a REAL commit limit (2026-10-01):**
  `python scripts/qa/run-with-memory-limit.py --process-mb N -- <command>` starts the command
  suspended in a job object with a per-process committed-memory cap, so allocations really
  fail. Measured with `program-buffer-recorded-av.mjs --fixture <clip>` (core peak with no
  cap: 727 MB):
  - **600 MB and 300 MB: survived.** `[frame-alloc] OUT OF MEMORY at media-decoder-mf` /
    `program-buffer-nv12`, frames dropped, clean exit 0.
  - **500 MB and 400 MB: the core died, exit `0xC00000FD` (stack overflow).** The dump shows a
    NEW THREAD in `ntdll!LdrpInitializeThread`: its stack could not be committed. That is
    inside the Windows loader; no handler in the core runs. **Dropping frames is not a
    guarantee of survival, only of surviving the failures that reach our code.**
  - Assigning the launcher itself to the job did not carry to its children on this machine;
    the child has to be assigned explicitly (create suspended, assign, resume).
- **So the machine's memory is watched and said out loud.** `core/SystemMemoryPolicy.h`
  samples available commit once a second from `sessionState()`: `low` under 4 GB, `critical`
  under 1 GB, with a 512 MB margin to leave a level. Snapshot node `systemMemory {measured,
  level, commitAvailableMb, commitLimitMb}` (published unconditionally) and one
  `[system-memory] low|critical|ok: N MB of M MB ...` line per transition.
  **Not wired to any operator surface yet**, and a job-object cap is NOT reflected in the
  sample, so the capped runs above could not exercise it; it read `ok` at 103 GB available on
  the live app.
- **The FFmpeg clip reader recycles its frame buffers** (`MediaFrameRecycler`): a retired
  frame is reused only when the reader is its sole owner, so a frame the compositor still
  holds is never overwritten. It used to allocate 8.3 MB per frame.
- **Rules:** an exception escaping a worker thread is a process kill, so anything a worker
  allocates per frame needs a non-throwing path. When the core restarts unexpectedly, read
  the dump and the System event log before the app logs.
- **Tests:** `FrameAllocationTest.cpp` and `MediaFrameReadStepTest.cpp` (pure; an allocator that fails on demand). The
  dropped-frame case fails if the drain is removed.
