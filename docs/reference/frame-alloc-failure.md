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
  (`ShmFrameReader`, `I420Convert`); every macOS `.mm` path. The FFmpeg clip reader still
  allocates a fresh 8.3 MB buffer per frame instead of recycling.
- **None of it has run under real memory pressure.** The helpers are unit-tested; each call
  site was changed by reading it. The show drill, multiview and ISO-record checks pass the
  same as before on the changed core.
- **The show drill's source->render gate fails on main, before and after this change**
  (p50 65 to 67 ms measured on both cores, 2026-10-01; every other drill line passes). Do not
  read that line as a regression from whatever you just changed without an A/B.
- **Rules:** an exception escaping a worker thread is a process kill, so anything a worker
  allocates per frame needs a non-throwing path. When the core restarts unexpectedly, read
  the dump and the System event log before the app logs.
- **Tests:** `FrameAllocationTest.cpp` and `MediaFrameReadStepTest.cpp` (pure; an allocator that fails on demand). The
  dropped-frame case fails if the drain is removed.
