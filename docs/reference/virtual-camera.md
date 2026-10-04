# Virtual camera (program feed → a webcam for Zoom/Teams/OBS)

## Reader delivery evidence

The #517 delivery evidence replaces the old `Fill` sample with a
`[vcam-delivery-v2]` summary at the same one-per-60-attempts frequency. Counters
belong to one process/stream instance; `run` changes on Start. `fresh`, `held`,
and `slate` count only samples whose Media Foundation event enqueue succeeded;
failed sample creation/enqueue is separate. Read outcomes distinguish unchanged
publication, seqlock contention, unavailable mapping, uninitialized mapping and
invalid header. Format mismatch is separate from a successful SHM read.

`lastPublication` is the last successfully read SHM publication counter, NOT
a Program identity and not necessarily the image last emitted (for example when
dimensions mismatch). The unchanged V1 pixel ABI contains no producer epoch.
Neither fresh reads nor event enqueue prove receiver display or lip sync.
`receiverVerified=0` is deliberate.

`COREVIDEO_DELIVERY_TRACE=1`, sampled when the publisher starts, enables an
optional 80-byte `vcam-correlation-v1.shm` sidecar alongside the pixel mapping.
The shipping 32-byte pixel header and NV12 payload remain unchanged. The sidecar
uses the existing mapping access policy and its own seqlock, carrying a producer
epoch, backing-file identity, pixel seqlock/publication, Program sequence, and
monotonic delivery/publication timestamps. The reader accepts identity only when
stable records before and after its pixel copy match the actual pixel mapping.
Publisher start invalidates previous identity even when tracing is disabled.
Missing, racing, stale or incompatible records produce uncorrelated reads.

Identity travels with the buffered NV12 packet through the publisher's replaceable
pending slot. `lastReadProgramSequence` describes the last correlated read;
`lastProgramSequence`, `epoch`, and `programIdentityVerified` describe the last
successfully enqueued sample. A held sample retains its previous pixel identity,
including after a fresh read with incompatible dimensions. Slate has no Program
identity. `unobservedProgramFrames` counts gaps between correlated reads, without
claiming which upstream boundary lost them. Epoch changes and regressions have
separate counters. Legacy V1 records never establish Program identity.

Run `node scripts/qa/vcam-delivery-evidence.mjs --log <vcam-serve.log>` on a copied
serve log. It reports per-instance/run counter deltas, rejects malformed records
and counter regression, and marks old uninstrumented logs unavailable. Exit 0
means valid evidence was parsed, never that delivery passed. The emission interval
maximum is lifetime data; Stop/Start idle time is excluded. Logging is still the
existing synchronous periodic logger; bounded background tracing, source-to-Program
identity and end-to-end receiver qualification remain separate spec slices.

Tests redirect both SHM and the serve log using `COREVIDEO_VCAM_SHM_DIR`.

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
  Inspect `%ProgramData%\CoreVideoPro\vcam-serve.log` for reader delivery evidence.
  The new `vcam-delivery-v1` counters distinguish emitted fresh, held and slate
  samples; the older once-per-60 `Fill` lines cannot establish frame continuity.
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
1. Registration is HKCU (no admin): `scripts/register-virtualcam.ps1`.
2. **Rebuilding the DLL needs the app stopped AND the Frame Server restarted elevated** — it
   holds an image-section handle to the registered DLL, so the relink fails with `LNK1104`
   even though `tasklist /m` shows no holder. `Start-Process powershell -Verb RunAs
   -ArgumentList 'Restart-Service FrameServer -Force'` (owner approves the UAC).
3. Build target: `cmake --build native\build-dev --config Release --target
   corevideo-virtualcam corevideo-native corevideo-native-tests`.
4. `native/virtualcam-dll/VcamLog.h` is gated serve-tracing for debugging the DLL side.

## Independent camera pixel receiver

For an explicitly selected synthetic QA run only, `COREVIDEO_QA_PROGRAM_COUNTER=1` adds complementary binary Program-sequence markers at the top and bottom of the Program image. This changes output pixels and must never be enabled for a live show. It defaults off. GPU ClearView writes the markers before the normal Program-buffer/NV12 path; a hardware test decodes the resulting delivered NV12 packet and matches its Program identity.

Build `corevideo-vcam-receiver` and run `corevideo-vcam-receiver 90 > receiver.ndjson` while the candidate camera is enabled. The probe enumerates the OS camera and negotiates 1920x1080 NV12 at 60/1 through Media Foundation. It reads no publisher mapping. Each sample records monotonic arrival, media PTS and decoded pixel identity; malformed/complement-mismatched patterns are null. An independent watchdog ends a stalled receiver with incomplete evidence.

`node scripts/qa/camera-pixel-receiver.mjs receiver.ndjson` excludes the first 30 seconds, requires at least 30 measured seconds, rejects incomplete captures, resets, duplicates, gaps, reordering, invalid markers and unexplained arrival intervals over 33.4 ms. This proves only the tested OS receiver pixels, not another application's presentation, audio alignment or the full-workload qualification.

For boundary isolation, `corevideo-vcam-receiver 90 --dll <absolute-DLL-path>` instantiates that DLL's media source directly through Media Foundation without changing COM registration. Its evidence is marked `receiverMode=direct-dll`; the judge always reports `osCameraContinuityVerified=false` for that mode. This diagnostic distinguishes a candidate reader from an older DLL served by Frame Server. Registered paths alone do not prove the loaded module: a conflicting machine-wide CLSID can select a different DLL from the installer's per-user registration.

`COREVIDEO_CAMERA_READ_RETRY=1`, set in the reader process, enables an experimental bounded publication wait. Unchanged or contended reads may wait for the next publication for at most one nominal frame period, with at most 64 high-resolution waits and two payload-copy attempts across the entire sample request. Missing or invalid mappings do not retry. Retry reads do not accelerate orphaned-mapping reopen cadence. Counters report attempted and recovered samples; held samples still count as held. The switch defaults off pending installed qualification and latency/audio evidence; a producer-process environment variable does not configure a separately hosted Frame Server process.

For controlled Windows Frame Server qualification, explicitly build the excluded
`corevideo-virtualcam-retry-qa` target in Release. It produces a separately named
diagnostic DLL with publication retry enabled and logs `[vcam-retry-qa]` when a
stream initializes. Normal builds and release packaging continue to use the
unchanged default-off `corevideo-virtualcam` target. This avoids changing the
Windows service environment to inject a test flag. Record the diagnostic DLL
hash and actual loaded module path; registering a path alone is not provenance.
Preserve and restore the installed camera registration after the trial. An OS
receiver trial with this DLL qualifies that diagnostic combination, not an
unmodified release package.

The excluded `corevideo-vcam-publication-qa` executable and
`corevideo-virtualcam-publication-qa-off` / `-on` DLLs provide an isolated
publication experiment. Build all in Release. Their fixed mapping directory is
`%ProgramData%\CoreVideoPro\camera-publication-qa`; the production mapping is
never opened. The publisher refuses to run while CoreVideo's app/core is live
and uses a single-writer mutex. Invoke it as `SECONDS ODD_US --isolated-test`
(1–180 seconds of scheduled frames, 0–25000 microseconds held in the writing
state). Delays beyond one frame intentionally overrun source cadence and may
lengthen wall-clock runtime. No pixel mapping is deleted on exit.

For OS-hosted trials, preserve the installed camera registration, select the
exact diagnostic DLL, grant only the serving account's required read/execute
access, and verify its loaded module before measuring. Start the publisher
long enough to cover setup, the 30-second warmup and the receiver capture.
Compare otherwise identical off/on cases, retain every trial, and restore the
installed registration afterward. The targets are excluded from normal builds
and the package script copies only the production DLL.

Publisher evidence records actual writing windows and rational 60 Hz deadlines;
receiver evidence includes a host-monotonic arrival timestamp. Run
`node scripts/qa/publication-qa-evidence.mjs PUBLISHER.ndjson RECEIVER.ndjson`
to check source deadline overruns and publication-to-receiver pixel age. This
summary validates evidence, not continuity; run `camera-pixel-receiver.mjs`
separately. A lower delivered sample cadence fails even if successive identities
are consecutive. Compare full content latency and A/V using the final
qualification harness, rather than treating publication age as display latency.
