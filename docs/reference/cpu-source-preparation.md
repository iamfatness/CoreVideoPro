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
Zoom I420 preparation, full-resolution phase-dependent latency, or fleet
support. Those remain required by the render-delivery specification before
enabling this path in a release.
