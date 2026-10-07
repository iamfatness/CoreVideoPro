# CPU source GPU preparation

The optional Windows SHM BGRA path uses the existing
`ShmCapturePreparation` owner. `COREVIDEO_CPU_SOURCE_PREPARATION=1` requests
this representation and registers the D3D compositor as a GPU consumer. The
flag is off by default and independent of `COREVIDEO_GPU_CAPTURE`, which
continues to control native GPU capture.

The owner creates its D3D device on a registered production consumer's
adapter. Each mapping generation owns a three-image `D3DVideoFramePool`.
`BgraSourcePreparation` uploads the immutable BGRA bytes with their actual
row stride, submits an event query and polls on the preparation owner. Program
does not create these images, upload these bytes or poll this producer context.
Each completion publishes a uniquely attributed wrapper through the original
CPU frame's weak token. Late completion remains attributable even after newer
CPU arrivals. It cannot replace the authoritative CPU frame or spend a source's
playout reserve; Program reports an older admitted completion as held.

CPU bytes, capture observation time and source identity remain unchanged for
ISO and software consumers. The enabled Program path uses completed images or
an explicit hold/slate, without a CPU source upload fallback. The default-off
path retains its original CPU behavior. No additional playout FIFO or
Program buffer is introduced. The existing 2 ms owner polling can affect when
a representation becomes usable; latency qualification must measure that
boundary rather than assume it is free.

Three images bound each source. Pool exhaustion refuses an upload without
waiting for a reader; another source has its own pool. Images share the
existing 512 MiB logical source-GPU residency limit, including external leases.
Normal SHM retirement now requires both CPU leases and GPU pool/query leases
to retire. Dimensions are fixed per mapping generation; reconnect creates a
new owner and fences late work from the old mapping. Failed pool admission is
counted and retried only through a new mapping generation. Device creation
failure retains CPU delivery. GPU query or device failure recovery and
consumer replacement/demand retirement are not qualified by this slice.

The preparation owner's `Stats` distinguishes the requested flag, completed
matching representations, busy refusals, failed admissions and superseded
completions. These GPU fields are not yet exported through the support-bundle
contract; they are owner diagnostics, not presentation evidence.

Release tests exercise real SHM-to-compositor pixels with zero source CPU
uploads for ready representations, retained CPU/ISO identity, every pixel byte
in padded BGRA rows, bounded held-image exhaustion with another source still
preparing, late epoch rejection, and resize/reconnect retirement with a GPU-only
external lease. They establish these behaviors on the tested Windows rig.
They do not establish sustained 60 fps, installed delivery, physical display,
full-resolution phase-dependent latency, or fleet
support. Those remain required by the render-delivery specification before
enabling this path in a release.

## Decoded Zoom I420 arrival

The same opt-in requests `I420SourcePreparation` from `ZoomEngineRuntime`.
The real SHM decoded-arrival copy offers its immutable CPU planes outside the
runtime mutex, before the existing source playout reserve and guest A/V trim.
Neither preparation nor GPU completion changes CPU frame selection. A token
travels with the selected CPU frame; Program resolves its view only after
selection, matching source id, epoch, frame id, observation time and dimensions.
Observation time is local arrival observation, not sender acquisition time.
Reconnect assigns a new source epoch and clears old guest-trim pictures even
when the new stream reuses an old frame id.

A resource owner initializes/imports three R8 plane images per slot. A separate
GPU owner submits tight I420 planes and polls actual event-query completion;
slow resource creation for one source does not block another source's uploads.
Admission is bounded to 64 source records, 16 active pools and 28 weak pending
tokens per source. Only Program demand activates a pool. Five seconds without
demand retires it; external GPU leases defer release. These planes share the
existing 512 MiB logical source-GPU budget with BGRA images.

## Native Media Foundation I420 capture

The same launch opt-in requests a preparation owner shared by the native UVC
adapter's capture sessions. `I420CaptureArrival` offers the immutable repacked
NV12/YUY2 planes on the capture thread, outside the snapshot mutex. Each real
arrival gets a local observation timestamp and increasing frame identity.
Reconnect or dimension change assigns a new epoch, even if frame numbering
restarts. The routing alias is fixed before the capture thread starts, so the
token and source descriptor use the same `capture:` identity. Adapter polling
copies the held descriptor and token; it does not offer another arrival.

CPU/ISO bytes and negotiated color hints remain authoritative. Disabled
preparation retains the CPU path. This adds no capture FIFO or Program buffer.
It does not qualify CPU BGRA media, browser or native WGC fallback producers.

The I420 owner retains attributable submitted completions when a newer CPU
selection arrives. A newer selection cannot relabel or erase an in-flight
image. Unsubmitted obsolete tokens are explicitly superseded so Program can
replace a pending identity that will never complete. On upload pressure the
owner releases its oldest completed image while retaining the current one;
consumer/GPU-read leases still prevent slot reuse. CPU tokens do not pin slots.
This matters for unbuffered 60 fps capture, where GPU completion commonly
occurs after the next CPU arrival. Tests force that interleaving and compare
the older admitted pixels, followed by the newer identity.
Event-query polling on this preparation worker allows D3D query progress.
On the physical 1080p60 capture rig, DONOTFLUSH polling left uploads pending for
hundreds of milliseconds despite the initial submission Flush; allowing query
progress restored approximately 60 completed images per second. This polling
uses the preparation context and never Program's context. Initial GPU
submission still does not certify a ready image; publication requires S_OK
and a completed event.
Decoded/capture arrival and resource handoff notify the GPU owner immediately.
While writes are pending it requests 100 microsecond completion polls; idle
owners retain a two-millisecond maintenance wait for demand and retirement.
These are requested waits, not scheduler guarantees. Program neither wakes nor
waits for this owner. Sparse source-selection age can diagnose a preparation
delay but does not certify end-to-end acquisition or presentation latency.
The owner admits the CPU-selected identity and at most one future arrival.
That one image can be ready before selection. Further future arrivals remain
weak CPU tokens in the bounded queue; older unsubmitted tokens are explicitly
abandoned. Preparing all arrivals ahead of CPU playout
can recycle a future image before its eventual selection, especially with
guest trim. A submitted older selection still completes and remains readable
with its original identity. Selection changes no CPU playout or ISO descriptor.

`scripts/qa/native-i420-capture.py` drives one explicitly selected native
MF/UVC device in an owned development core. Supply the exact native device id,
Release binary and source commit, and a new output directory. It pins 1080p60
Program, two-frame buffering, GPU capture off and monitor isolation on. The
CPU preparation flag is an explicit argument. It preserves warm-up samples,
requires advancing CPU input even with preparation disabled, scores GPU source
admission separately from native buffer delivery, and can repeat
disconnect/reconnect in the same core. Each reconnect must expose a new admitted
epoch; native transition counters include disconnect and subsequent warm-up.
It stops only its own core and writes binary/harness hashes with the evidence.
This proves sampled preparation/Program behavior, not every physical input
pixel, scanout, receiver playback, acquisition latency or installed/fleet support.

Every completed frame has a unique immutable wrapper. CPU/ISO tokens hold only
weak GPU publication references. Reusing a plane slot cannot make a retained
old CPU token resolve newer pixels. Program's existing read queries protect
the admitted wrapper until GPU reads complete. Optional monitor snapshots
drop the token and cannot acquire production slots later.

Invalid dimensions, incomplete planes, duplicate/out-of-order identities,
same-epoch dimension changes, capacity exhaustion and resource creation failure
refuse preparation while preserving CPU delivery. Query failure or producer
device removal stops this preparation owner and logs the HRESULT. Pending
writes remain charged and unreusable; shutdown drains them for at most two
seconds, then quarantines unresolved pools. This slice does not recreate a
failed device or rebuild views when a compositor consumer is replaced. Those
recovery cases, actual hardware device loss and the diagnostics export contract
remain unqualified.

A failed per-source resource build reports preparation-failed through its
selected token. Device initialization refusal stops preparation explicitly;
neither failure remains labeled as pending. Failed or stopped preparation
invalidates Program's held image for that source.

Release tests compare actual independent-device pixels for both color matrices,
full/limited range and grading; check zero Program CPU source uploads for ready
views; exercise the real decoded-arrival parser and compositor; retain CPU/ISO
frames across slot reuse and reconnect; and delay/fail a selected source's
resource creation while a healthy source continues. These are functional
checks. The enabled Program path admits only compatible GPU-completed views. Before
readiness, it holds one GPU-only last-valid descriptor from the same nonzero
epoch and dimensions, never newer than CPU selection; otherwise it draws the
unavailable slate and reports its reason. Unsupported or failed preparation
does not fall back to a Program CPU upload. CPU/ISO descriptors remain original.
The bounded 64-source hold cache expires after 300 unused Program frames and
is separate from inline optional-pass caches. Held color hints belong to the
actual image, while operator framing/grade still follow the current plan.
One additional selected-identity token per source can observe a late completion;
its descriptor retains no CPU/ISO payload. Expired publication, failure,
source epoch and size changes invalidate that pending identity. A completed
older selection is explicitly held, with its actual identity, never relabeled
as the latest requested source frame. BGRA mapping completions use unique
identity wrappers and weak publication just like I420. The producer retains
at most two completed BGRA images within its existing three-slot pool,
independent of CPU/ISO token lifetime; pressure retires the oldest owner-held
completion while GPU read leases still protect any consumer's pixels.
Inline participant source exports are suspended in this mode with the explicit
source-exports-require-monitor-isolation warning; enable monitor isolation for
independent source exports. The opt-in never silently changes that flag.
Snapshot programSourceAdmission version 1 carries requested and actual source
epoch/frame/observation-time identities and ready/held/unavailable reasons for
the completed shell snapshot. Unavailable actual identities are null. It does
not prove display presentation, source acquisition or native output freshness.
The default-off path retains its original CPU behavior. Sustained
delivery, phase-dependent latency, monitor freshness and installed/fleet gates
remain required under #802 and parent #517.
