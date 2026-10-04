# Virtual camera (program feed → a webcam for Zoom/Teams/OBS)

_Moved verbatim from `CLAUDE.md` (#737). Paths are relative to the repo root._

The program appears system-wide as **"CoreVideo Pro Camera"** at native **1080p60**.
It is an out-of-process, user-mode COM Media Foundation source DLL
(`native/virtualcam-dll/` → `corevideo-virtualcam.dll`, CLSID
`{8B4B2C9E-2C4A-4E1D-9C7A-CDEF01234567}`) that the Windows **Frame Server** loads on
demand; the core registers it as a virtual camera via `MFCreateVirtualCamera`.

Pipeline: **core → cross-session shared memory → DLL → Frame Server → app**.

- **Cross-session shared memory is the whole trick.** The core publishes from the user's
  **session 1**, but the Frame Server serves the camera from the **session-0** `FrameServer`
  svchost — so a `Local\`-named mapping is a *different* object in each session and the DLL
  only ever saw the standby slate. Non-elevated processes can't create a `Global\` object
  (`SeCreateGlobalPrivilege`). **Fix = a file-backed mapping** at
  `%ProgramData%\CoreVideoPro\vcam-frame.shm` with a permissive DACL
  (`D:(A;;FRFW;;;WD)(A;;FR;;;AC)` — Everyone + ALL APPLICATION PACKAGES,
  `FILE_ATTRIBUTE_TEMPORARY` so it stays in cache). Same path in every session → the OS
  keeps it coherent. See `openVirtualCameraShmFile`/`mapVirtualCameraShmView` in
  `native/src/modules/VirtualCameraShm.h`; used by the publisher (writer), the DLL's
  `SharedFrameReader` (reader), and the round-trip test. Layout: 32-byte header
  (magic `0x43564643`, then seqlock `seq`/`w`/`h`/`fps`/`byteLen`/`frameNumber` as u64)
  followed by an NV12 payload; the writer uses a seqlock, the reader retries on an odd seq.
- **No flashing:** the DLL caches `lastGood_` and re-serves it on a transient read miss;
  it only falls back to the slate after ~30 missed frames (`MediaStream.cpp`).
- **THE SOURCE PACES DELIVERY (2026-07-12).** The pipeline requests the next sample the
  moment the previous completes — completing `RequestSample` immediately free-runs the
  serve chain at CPU speed (measured ~2000 samples/s = ~6GB/s of 3MB copies through the
  Frame Server + every consumer; Zoom's video process burned 8+ cores and system audio
  glitched whenever the camera was consumed). `MediaStream::RequestSample` now waits
  until the next frame is DUE (high-res waitable timer; plain Sleep quantizes to ~40fps).
  Verify cadence in `%ProgramData%\CoreVideoPro\vcam-serve.log` (Fill lines ≈ 1/s = 60/s).
- **NEVER delete the SHM file** (`openVirtualCameraShmFile`): readers hold the file
  object via FILE_SHARE_DELETE; delete+recreate orphans them on the unlinked file and
  they degrade to frozen frames / the slate forever (program/slate strobing when a stale
  and a fresh instance interleave). The writer opens IN PLACE and re-asserts the DACL
  (`SetKernelObjectSecurity`); the reader self-heals by re-opening by path after ~1s of
  frozen seq (`SharedFrameReader::kReopenAfterUnchangedReads`).
  **AND NEVER RUN A TEST AGAINST THE PRODUCTION SLOT (2026-09-20 incident).** The
  round-trip suite unlinks the slot between cases, and it did so at the real
  `%ProgramData%` path: every `corevideo-native-tests.exe` run on this box while the
  installed beta was live unlinked the file the core was publishing into (the
  delete succeeds under the open writer), the core kept writing the orphaned file
  object and reported healthy, the Frame Server's reader got ERROR_FILE_NOT_FOUND on
  the path and served the standby slate — the owner saw a grey bar in Zoom until they
  toggled the camera, twice in one morning, each time 6 s after another agent's test
  exe was rebuilt. `virtualCameraShmDir()` now honors `COREVIDEO_VCAM_SHM_DIR`, and
  `VirtualCameraShmRoundtripTest.cpp` sets it at static-init time to a per-process
  `%TEMP%\cvp-vcam-shm-test-<pid>` directory for the WHOLE test binary (the DLL reader,
  the real publisher and the test writer all resolve through that one helper);
  `TheSuiteNeverResolvesTheProductionSlotPath` pins it. Production never sets the
  variable. Any new test that touches the slot inherits the redirect for free — never
  hard-code the ProgramData path in a test.
- **Serve diagnostics:** the DLL logs to `%ProgramData%\CoreVideoPro\vcam-serve.log`
  (pre-created by the publisher with a permissive DACL — locked-down Frame Server
  workers cannot write `C:\Windows\Temp`, which left the serving side unobservable).
- **No latency drift:** the DLL stamps each sample with `MFGetSystemTime()` (a live source),
  never an accumulating `nextPts_ += frameDuration_` counter.
- **Dims must match.** The DLL media type is **fixed 1920×1080@60** (`MediaSource.h`), so
  `MediaCore::syncVirtualCamera` HARD-PINS 1920×1080@60 and ignores the shell's command
  w/h/fps — a mismatch makes the DLL reject the frame → slate.
- **Off-thread readback (why it's ~free).** Reading a 4K program back on the render thread
  froze Take/preview (~20ms under `coreMutex`); on the audio worker it starved audio. The
  fix: on the render tick the compositor does a cheap GPU **scale-blit** of the program
  into a *dedicated* 1080p keyed-mutex shared texture (`exportVcamSharedTexture`,
  fullscreen-triangle identity draw — do NOT reuse the program `sharedTexture_`, WinUI
  already holds its keyed mutex and a third consumer deadlocks). A **second D3D device** on
  its own thread (`vcamTapLoop`) does AcquireSync/CopyResource→staging/Map/NV12-convert, and
  the output worker just does a cheap NV12 copy (`takeVcamNv12`). Net render
  cost ≈ 1ms. Rule: GPU→GPU `CopyResource` is microseconds; GPU→CPU-staging map+read is
  ~8–12ms and MUST live on a dedicated device/thread, never under `coreMutex` or the audio
  worker.
- **THE TAP THREAD PUBLISHES — never the output worker (2026-08-07).** The vcam used to be
  published from the ~50Hz audio/output worker, whose 20ms period is an AUDIO constant
  (960 samples at 48k). Gating video on it capped a 60fps program at **50fps** and added up
  to 20ms of quantisation to a path whose entire budget is one 16.7ms frame — measured:
  render 59.7fps, output worker 49.7Hz, **vcam published 50.0fps**. It publishes through
  `ICompositor::setVcamFrameSink` on the tap thread now (**59.9fps** measured, matching the
  DLL's declared 60). `MediaCore` must NOT also publish when
  `compositor->publishesVcamFrames()` or every frame goes out twice, and `~MediaCore` MUST
  clear the sink — `modules_` is declared before `virtualCamera_`, so the publisher dies
  first while the tap thread is still running. Note the OLD claim here ("the last ~10fps is
  the scalar `convertBgraToNv12`") was doubly stale: the GPU convert had already shipped,
  and the real cap was the worker cadence. Verify with
  `node scripts/measure-program-out-latency.mjs`, which reads the same seqlock header the
  DLL reads and attributes the published rate to a stage.
- **PROGRAM VIDEO HAS ITS OWN 60Hz TICK (2026-08-07).** `encoder->submit` used to run on the
  ~50Hz audio worker, so recordings muxed **49.9fps** of a 60fps program — measured properly
  with ffprobe on identical 25s content: **1251 frames before, 1495 after (59.7fps)**.
  `JsonRpcServer` now runs a `videoOutputThread` at 60Hz driving
  `MediaCore::renderVideoOutputTick`, and the audio worker submits **audio only** (guarded by
  `videoOutputTickRunning_`, so direct/unit-test callers keep the old synchronous path).
  Lock order is unchanged and MUST stay so: `coreMutex` (brief snapshot of `lastProgramFrame_`)
  → `audioOutputMutex_` (encoder), never both at once, never reversed — the two workers
  serialise on `audioOutputMutex_`, which the audio side holds only ~13% of the time
  (`work=2.6ms` per 20ms tick). Do NOT instead raise the audio worker to 60Hz: that breaks
  the 960-sample block contract (spec 4.2) its pacer exists to hold.
  **`takeVcamNv12` yields each tap generation exactly ONCE**, so only the video tick may take
  it; it leaves the newest frame in `latestProgramNv12_` and the audio worker reads that for
  the senders. Two callers would starve each other.
  Measured end state: render 59.9fps, video tick 59.8/s, audio worker 50.0/s, vcam 60.0fps.
- **THE VIDEO TICK IS SIGNALLED, NOT PACED (2026-08-08) — three designs were measured and
  only the third is correct.** It waits on `videoOutCv_` until the render thread publishes a
  new program frame (bounded 20ms so it can still deliver a sender stop when the program is
  idle), so the wait IS the pacing.
  1. **60Hz pacer — WRONG, and dangerously plausible.** A 60Hz sampler against a 60Hz
     producer is the frame-pairing problem the Zoom synchroniser exists to fix: it muxed
     **51.7fps** of a 60fps program. The same build on another run read 59.7fps, because it
     depends on the phase the two threads start in — so a single green measurement proves
     nothing here.
  2. **120Hz pacer — fixes the aliasing, breaks the show.** Sampling above Nyquist works,
     but the extra `coreMutex` acquisitions dropped the 8x1080p60 drill to **57.4fps** with a
     **141ms** command p99.
  3. **Condition variable — correct.** One wakeup per real frame: 59.9fps recorded (three
     consecutive runs), drill 60.0fps at 4.3ms hold, command p99 **47.4ms** (BETTER than the
     51.2ms baseline).
  **NEVER notify under `coreMutex`.** The first CV attempt signalled inside the render lock,
  waking a thread that instantly blocked on the lock still held — command p99 51ms → 107ms.
  `MediaCore::notifyProgramFramePublished()` is called by `JsonRpcServer` AFTER the lock
  scope closes, and must stay there.
- **Counters that count SUBMITS are not frame rates.** `recording.proof.programFrameCount`
  counts submits, so it read ~50/s and looked like the muxed rate; it also read 911 on a
  30fps SRT source whose file held 498 frames. When judging a recording's rate, count frames
  in the ARTIFACT (`ffprobe -count_frames`) over its duration — same discipline as "verify
  PIXELS, not stream presence".
- **THE SENDERS ARE SPLIT TOO (2026-08-08): video on the 60Hz tick, audio on the audio
  worker.** FFmpeg takes program video and program audio through **two separate inputs**
  (a rawvideo pipe and a PCM pipe), so they never had to arrive in one call — but
  `sync()` carried both, which pinned the whole stream to the ~50Hz worker. Now
  `renderVideoOutputTick` calls `sync()` (video + destinations + settings) and the audio
  worker calls the new `IOutputSender::submitAudio`. Measured: sender fed **50.0fps
  before, 60.0fps after**.
  Three things this required, each a trap on its own:
  1. **A LAYOUT DECLARES AUDIO — the sender must NOT wait for PCM to learn it exists.**
     This is the one that cost a full debugging round. The FFmpeg arg list bakes in the
     audio input, so if the first `sync()` carries no PCM the process starts with
     `anullsrc`; when audio then arrives it must RESTART — and **an SRT listener accepts
     ONE caller**, so the reconnect is refused (`Connection to srt://... failed: I/O
     error`) and the stream never recovers. It was intermittent because it depended on
     whether the first PCM beat the first sync. `sync()` now latches the layout from
     `audioChannels`/`audioSampleRate` ALONE (`haveRealAudio_`, sticky, never cleared by a
     video-only call), and `renderVideoOutputTick` passes the layout whenever
     `audioRoutingSends_` is non-empty. **Read FFmpeg's own stderr log
     (`ffmpegStderrPath_`, a temp file) before theorising about the sender** — it named
     this in one line after an hour of guessing.
  2. **`Kind::Audio` is never dropped** in `AsyncOutputSender` — video is state (newest
     wins), audio is a timeline. Queued audio MERGES into the newest pending audio item,
     capped at 5s, and never clobbers the session snapshot.
  3. **The video tick must run one tick past the last destination** (`senderSyncActive_`):
     senders are STOPPED by a `sync()` carrying no destinations, so returning early the
     moment outputs clear would strand a live stream running.
  Direct/unit-test callers (no video tick) keep the original single-call path behind
  `videoOutputTickRunning_`.
- **Gate the sender's cadence on its BEST interval, not the median.** The defect is a
  STRUCTURAL cap — video fed from the ~50Hz audio worker can never exceed ~50 on any
  interval (main measures 49.8 median / 50.2 best). A busy machine makes
  `AsyncOutputSender` coalesce and dip (46–53fps observed mid-build), which a
  median-based gate reports as the same failure. The peak separates "capped" from
  "loaded". `validate-srt-output.mjs` also needs a listener head start before the core
  calls — and **never probe the port with a UDP bind to test readiness**: SRT is UDP, so
  the probe steals the port from the listener it is waiting for and turns an intermittent
  race into a reliable failure (tried it; it made things worse).
- **A STREAM'S CONTAINER FPS CANNOT PROVE ITS CADENCE.** FFmpeg pads duplicates up to its
  declared `-r`, so a sender fed at 50fps still emits a stream that ffprobe reads as
  **59.9fps** — identical to a healthy one. The defect is only visible in the sender's OWN
  accepted-frame counter (`framesSent`: ~250 per 5s interval at 50Hz, ~301 at 60Hz), which
  is what `validate-srt-output.mjs` now gates. Same family as the recording counter that
  counted submits: **measure the thing, not a proxy that survives the bug.**
- **Enable it:** control API `POST http://127.0.0.1:8011/invoke
  {"action":"transport.virtualcam.set","args":[true]}` (or the transport toggle in the UI).
- **Verify the feed:** read the 32-byte header of the ProgramData file; `frameNumber`
  delta/sec = the publish fps.

**Rig ops for the DLL (READ before rebuilding it):**
1. Setup installs an administrator-owned, hash-versioned copy in
   `Common Files\CoreVideoProCamera\<sha256>\corevideo-virtualcam.dll`, with
   read/execute access for LocalService and users. HKLM owns the COM registration;
   HKCU is an alias to the same runtime. `Register-VirtualCamera.cmd` invokes the
   ownership-aware helper and requests elevation. Silent setup must already be
   elevated; failure is reported, rather than silently installing a broken camera.
   The dev wrapper requires an explicit `-AppDirectory`; it never selects a stale
   checkout automatically. Startup repairs only a missing/stale alias owned by
   this app, from an installed machine runtime. Unknown ownership and machine
   paths fail with named diagnostics.
2. DLL versions are immutable while installed. Setup never restarts camera
   services underneath consumers. Close/reopen consumers; if Windows still has
   the previous DLL cached, restart Windows. `Install-VirtualCamera.ps1 -Action
   Inspect -AppDirectory <app>` separates registry/hash checks from observed
   module paths. Access-denied module inspection is **unverified**. Even a matching
   loaded module is not proof of pixels reaching a particular receiver.
   Uninstall removes only its owned keys and tries to delete its unreferenced
   runtime after an exclusive write-open and hash check. Loaded images, rollback
   references, and uncertain cases are retained; it never forces removal or
   schedules a reboot delete. No production SHM file is deleted.
3. Build target: `cmake --build native\build-dev --config Release --target
   corevideo-virtualcam corevideo-native corevideo-native-tests`.
4. `native/virtualcam-dll/VcamLog.h` is gated serve-tracing for debugging the DLL side.
