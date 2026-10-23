# Program-to-multiview handoff: cause and repair

Issue: [#864](https://github.com/iamfatness/CoreVideoPro/issues/864).
Work order remains in [BACKLOG](../BACKLOG.md).

October 22 follow-up under #1016/#997: the dedicated isolated copy owner now selects the
current delivered slot and waits for its completed immutable publication within
the existing next-slot deadline. The earlier explicit one-frame selection offset
is removed for that path; exact identity, lease ownership and Program playout
timing remain. Inline readers retain the preceding-slot selection.
See the [measured comparison](../qa/source-composition-deadline-2026-10-21.md).

## Confirmed cause

The Program cell samples one mutable, keyed-mutex display texture. The exporter
acquires that texture, submits a GPU copy, flushes and waits for GPU completion
before releasing key 1. The multiview consumer attempts `AcquireSync(1, 1)`:
a one-millisecond wait. On timeout it draws its previously copied local Program
image. At the next delivery the exporter can acquire key 1 itself, reclaiming and
overwriting the unread publication. Repeated overlap at the same cadence starves
the reader while the multiview canvas, Preview and source cells continue updating.

This is a publication/consumption contract failure: a bounded wait on a repeatedly
overwritten surface does not guarantee delivery of a completed Program image.
Total CPU/GPU headroom does not guarantee access during that one-millisecond
window. Program render/queue counters and canvas completion counters cannot detect
the stale Program pixels by themselves.

The relevant paths are `D3DProgramBuffer::exportLoop`,
`D3D11Compositor::retainedProgramForMultiview`, and the monitor request's
`deliveredProgram`/`deliveredProgramOwner` fields. Preview and source cells do not
use this Program publication handoff.

## Causal evidence

October 11, 2026: separate synthetic counter-only 1080p60 processes using the
installed production core reproduced unread reclaim with both isolated and inline
monitor rendering. The isolated branch completed 1,759 exports and reclaimed
1,676 unread publications; the monitor worker still completed 1,777 canvas jobs.
These are ownership/worker measurements, not physical display FPS.

A private Release/O2 diagnostic executable then added HRESULT/wait counters to
the compositor. It linked the unchanged validated core library and main object
with the instrumented compositor object. Only the reader's requested wait budget
changed in an A/B/A comparison; isolation and two-frame Program buffering stayed
constant. Each process ran for 30 seconds, with approximately 29 seconds represented
by the final periodic log entry.

| Case | Attempts | Acquired | WAIT_TIMEOUT | Other errors | Program underruns/overflows |
|---|---:|---:|---:|---:|---:|
| A: 1 ms | 1,759 | 43 | 1,716 (97.6%) | 0 | 1 / 1 |
| B: 5 ms | 1,755 | 1,535 | 220 (12.5%) | 0 | 4 / 4 |
| A again: 1 ms | 1,757 | 52 | 1,705 (97.0%) | 0 | 0 / 0 |

The timeout result was `0x00000102`; no abandoned-mutex or other acquisition
errors occurred. The production-core reference observed a maximum exporter GPU
query wait of 3.2596 ms. The A/B/A result confirms the acquisition budget/ownership
overlap causes the reader starvation; 5 ms still fails and is not a shipping fix.
Exact sub-frame GPU/CPU phase traces remain useful for validation, but the failure
is no longer inferred solely from combined display counters.

The consumer's one-millisecond behavior predates the October 7 optional publication
change in #809 (`1810d627`), whose exporter holds ownership through GPU completion.
#862 enabled monitor isolation by default, but disabling it also reproduces the
failure. A specific first bad release has not been established; do not attribute
this exclusively to #862 without a historical workload comparison.

The uninstrumented references retained three Program underruns/overflows each;
the diagnostic cases' separate delivery failures are recorded in the table. Even
the case with zero Program queue misses had 97.0% display acquisition timeouts.
The live app
also had one Preparing-stage Program miss at 20:36:54 EDT, before the first
synthetic reference. These remain separate #802/#517 delivery concerns. This
investigation does not qualify zero-miss output, decoded A/V or other hardware.

Raw evidence and the diagnostic source/build identity are retained in the local
`installer-upgrade-retirement/artifacts/installer-retirement/` directory:
`program-handoff-*`, `program-acquire-probe/build.json`, and
`build-program-acquire-probe.ps1`. The probe was not installed or published.

## Repair design

Replace the multiview branch's repeatedly overwritten publication with a bounded
pool of completed Program snapshots and explicit consumer leases. Extract a focused
GPU publication type; do not add this lifecycle to `MediaCore.cpp`.

1. The optional exporter reserves a free publication slot and copies from its
   immutable Program input lease. GPU completion must be proved before the slot
   becomes readable. Release the producer's keyed-mutex ownership before exposing
   ready metadata. No copying, allocation or GPU wait moves onto Program delivery.
2. A publication carries generation, slot identity, delivered Program sequence,
   render/source frame identity, scene/render-plan identity and timeline timestamps.
   A monitor request retains the exact publication, not a pointer to a mutable
   latest texture. The exporter cannot overwrite a published, queued, acquired or
   GPU-reading lease.
3. Derive the pool bound from published, request-construction, pending-mailbox,
   executing and GPU-retiring holders. Prove the bound and record allocated bytes;
   avoid an arbitrary large queue. Program buffering remains at the user's 2/3
   frames. This pool provides ownership storage, not extra Program playout delay.
4. The monitor imports slot resources once per generation and caches views by
   generation/slot. Reuse its private local image and views; cycling slot handles
   must not create textures every frame or trigger WinUI binding refreshes. The
   external multiview canvas handle remains stable until a structural change.
5. Establish an explicit monitor presentation target from the Program timeline.
   Consume the matching completed identity and account for any bounded display
   offset. A latest-wins sample that silently repeats/skips delivered identities
   does not meet acceptance. Measure display age and latency; do not silently add
   buffering to conceal stalls or change camera/encoder audio alignment.
6. Retain the publication until the monitor's GPU read completes. Recycle on the
   owning worker. Resize, restart and device loss retire whole generations safely;
   uncertain GPU completion quarantines the lease. A stalled optional reader may
   degrade explicitly, but must never block Program delivery or grow memory without
   bound. Preview/source rendering remains independent of Program acquisition.

Add bounded observations for this branch: copies submitted/completed, publications
consumed/reclaimed/refused, acquisition HRESULT classes, requested/copied/presented
Program identity, retained-image reuse/age and generation. Worker/canvas progress
must not imply fresh Program-cell presentation. Preserve actual held identity.

## Implementation and verification plan

Implement the publication pool and lease contract first, with the multiview Program
reader as its real consumer. Add generation/slot import caching and retirement in
that same change. Then expose bounded observations through the existing native
snapshot, managed observations and support diagnostics.

Deterministic tests must cover the overlap demonstrated by the probe: exporter
ownership outlasting the old 1 ms wait, multiple render/delivery phase offsets,
unread publications, slow readers, queued jobs, delayed GPU completion, mailbox
replacement, shutdown, resize and device-loss quarantine. Verify that no leased
surface changes bytes/identity, and no incomplete surface is advertised ready.
Use real GPU synchronization tests for those properties, not only fake mutexes.

Run matched 1080p60 pixel tests with a changing Program counter inside the multiview
Program region. Decode that region independently; compare it with the authoritative
Program timeline and independent OS camera pixels. Preview/source-cell identities
must also be checked. API frame numbers, perceived smoothness and average FPS are
insufficient. Retain every capture, including startup and failed runs.

Begin with the reported one-capture-source configuration, then test 8 Zoom feeds
plus 2 capture feeds, Take/resize, and grading/ROI/overlays under the reference
hardware. Complete a 30–60 minute installed meeting run with enabled production
outputs and A/V synchronization checks. Preserve resolution, enabled outputs and
quality; a larger timeout, disabling isolation or increasing Program buffering is
not acceptance.

## Acceptance

- Zero unaccounted repeated, skipped, invalid or reordered Program-cell identities
  at the declared monitor cadence after a fixed, disclosed startup interval.
- No sustained acquisition starvation across tested phase offsets. Any optional
  failure/held image reports its real identity and age.
- Zero Program delivery misses or output identity gaps in the release-qualification
  workload; #802/#517 failures remain open if this display repair passes separately.
- Independent webcam continuity and decoded A/V do not regress.
- Bounded GPU memory/leases through slow-reader, resize and device-loss tests;
  no per-frame resource creation or UI surface rebinding.
- Installed beta evidence identifies binary/source hashes, hardware, workload,
  duration, display offset and failures. A finite reference-machine pass does not
  establish fleet-wide guarantees.

## Implemented contract and reference checks

The internal branch now uses eight non-keyed immutable GPU surfaces: two timeline
publications, request construction, pending mailbox, executing request, two GPU
reads and one writer. The 1080p BGRA pool owns 66,355,200 GPU bytes; imports alias
those resources. The reader owns one private image and two reusable EVENT queries.
Publications retain identity metadata, not CPU thumbnails/NV12 output packets.

The exporter proves GPU copy completion before publication. Requests lease the
exact image; the reader caches imports by generation/slot and keeps its lease
until its GPU copy completes. Unknown reads are quarantined on failure/shutdown;
exhaustion refuses optional publication without waiting on Program delivery.
The shell-facing canvas handle remains stable.
Identity preparation occurs before taking the delivery mutex; publishing swaps
fixed history entries without allocation. No display GPU work runs under that lock.

Selection targets the render's scheduled production slot minus Program depth
minus one. This explicit monitor offset is one 60 Hz frame (16.667 ms); camera
and audio playout depth remain unchanged. CPU-arrival-time selection caused a
jump/repeat near a frame boundary in an early probe and was removed. Canvas
export and OS presentation latency are separate measured requirements.

Native snapshots and managed support bundles expose requested/copied identity,
held-image age, generation, copies/completions, reuse/refusal, skipped/reordered
identities and pool/read bounds. Age is recalculated at serialization. These
observations explicitly do not claim OS display presentation.

On the RTX 4090 reference (driver 32.0.16.1742), the focused GPU suite passed ten
tests. Pixel checks cover 1080p60, both Program depths, both monitor modes and
2/4/10/14 ms producer phases. The independent receiver samples at the fixed
16 ms position of each 60 Hz slot. All 1,600 measured pixels matched the expected
timeline image with zero repeats/gaps after a fixed twenty-slot startup interval.
Startup pixels and every earlier failure remain in local evidence, including an
immediate-after-submit read of an unfinished outer canvas export. Those earlier
captures are not clean passes.

Immutable-lease/exhaustion and unretired-read shutdown checks also passed, along
with related isolated pixel/resize, worker, cache and timeline tests. The legacy
source-cache suite passes in inline mode; its participant-export assertion fails
with isolation enabled on both unchanged baseline and candidate. Managed bundle
tests passed 35/35; the full managed suite passed 2,382/2,382. The broader native
run passed 1,569 and failed three fixtures whose same failures reproduce on the
unchanged beta with isolation enabled and pass inline (tracked in #869).
Installed OS-camera, mixed-source meeting, decoded A/V,
hardware device-loss and long-run qualification remain separate gates. These
focused results do not close #802/#517 or establish release acceptance.

An additional independent-device generation test preserves old pixels while
their pool retires, verifies resized pixels, proves GPU completion releases the
old lease, and checks that another slot reuses the private image/view. It passed
without changing the packaged runtime binary.

The packaged 690fe1e core also ran a 75-second synthetic native-clock reference.
After its fixed 15-second startup interval, 3,580 delivered frames and display
copies completed with zero delivery misses, render skips, monitor supersession,
image reuse, refused copies or copied-identity skips/reordering. It is copy/counter
evidence, not an OS-camera or physical-display capture. Two startup underruns and
two overflows remain retained: the first render's encoder-export initialization
cost 40.840 ms and delayed the initial publication beyond its deadline. This is
not an all-run zero-miss qualification; #802/#517 remain open.

The local beta `beta-2026-10-11-690fe1e` was installed on October 12 after the
administrator prompt was accepted. Setup exited zero, the installed native core
matches the packaged SHA256, the stable desktop shortcut targets this beta, and
machine camera registration points to its matching protected DLL. The installer
retired the previously registered `491e3f7` beta. Service-loaded DLL identity was
not observable from the unelevated inspection and remains a separate check.

The installed 75-second OS-camera reference passed after its fixed 30-second
warmup: 2,701 independently decoded counter frames over 45 seconds had zero
invalid, repeated, missing or reordered identities; maximum arrival interval was
20.664 ms. Native observations recorded 2,869 delivered frames and completed
Program display copies with zero measured delivery misses, reuse, refusal,
skips or reordering. Startup observations remain retained. This synthetic pass
does not establish physical Program presentation or mixed-source/A/V acceptance.

The installed app joined the authorized real test meeting, obtained raw-recording
privilege and received eight Zoom feeds alongside one live 1080p capture. The
operator reports that Program now looks smooth. The longer observation failed
after 187.64 seconds: 11,249 delivered frames, three new underruns, two overflows,
four reused/held display copies and four skipped copied identities. Later app
counters continued increasing. All samples and the first failure are retained;
this is not zero-miss acceptance.

At the first failure, render lock-wait maximum rose to 31.8028 ms while render
CPU work maximum stayed at 8.2923 ms. Empty `media-core-sync` status responses
spent 34–43 ms constructing snapshots and held the core lock for 35–43 ms,
coinciding with source-stage misses. Memory health stayed OK and Zoom inputs
continued arriving. This identifies a separate snapshot/render-lock blocking
path, tracked with a focused repair plan in #870; attribution to a specific
nested snapshot operation remains open. Response string serialization is
already outside that lock. #868 remains draft; #517 and #864 remain open.
