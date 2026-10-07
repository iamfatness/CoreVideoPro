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
Only a completed upload matching the current participant id, source epoch,
frame id and dimensions can attach a GPU representation. A superseded upload
cannot replace the authoritative CPU frame or spend a source's playout reserve.

CPU bytes, capture observation time and source identity remain unchanged for
ISO and software consumers. Until a matching GPU representation is ready, the
existing CPU path remains available. The opt-in therefore does **not** assert
that every Program frame avoids CPU uploads. No additional playout FIFO or
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

Release tests compare actual independent-device pixels for both color matrices,
full/limited range and grading; check zero Program CPU source uploads for ready
views; exercise the real decoded-arrival parser and compositor; retain CPU/ISO
frames across slot reuse and reconnect; and delay/fail a selected source's
resource creation while a healthy source continues. These are functional
checks. CPU fallback remains available before a ready view, so opt-in still
does not guarantee that every Program frame avoids CPU uploads. Sustained
delivery, phase-dependent latency, monitor freshness and installed/fleet gates
remain required under #802 and parent #517.
