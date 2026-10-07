# CPU source preparation

`CpuSourcePreparation` owns bounded D3D11 preparation for immutable CPU I420
and BGRA arrivals. `I420SourcePreparation` remains a compatibility alias.
The launch override `COREVIDEO_CPU_SOURCE_PREPARATION=1` creates one shared
owner in module composition for Zoom, native UVC, WGC CPU fallback, media playback and stills.
The override defaults off. Unsupported platforms do not publish GPU views.

Media decoding calls `CpuVideoArrival` on its decoder thread before pushing
the existing CPU presentation queue. Still decoding calls it on the cache
worker, once per source alias and decoded image. Both keep the original CPU
buffer and playback timestamp. A held poll retains its arrival observation;
rewind, new pixels with a repeated identity, size or format changes fence the
old generation. Aliases may share CPU pixels but have distinct GPU identities.

Program selects an arrival token and resolves only its exact completed view.
It performs no upload, resource creation or GPU query for these sources.
The resource owner creates and imports three slots; the GPU owner uploads and
checks the event query before publishing an immutable attributed wrapper.
An upload may prepare one future arrival ahead of CPU selection. Preparing
unbounded future arrivals can evict a frame before CPU playout selects it.

Limits remain 64 registered sources, 16 active generations, 28 pending tokens
per source and 512 MiB shared production storage. A source may keep one charged
retiring generation. A third generation is refused until that lease releases.
CPU and ISO references do not pin GPU slots; submitted GPU reads do.
BGRA row stride and straight alpha are preserved. I420 range/matrix metadata
stays on the original CPU descriptor and is applied by the consumer shader.

Idle GPU retirement does not invalidate the CPU frame. Still and decoder
workers refresh stopped tokens for exact held descriptors without advancing
playout or changing observation time. Failed GPU owners publish attributable
failed tokens for subsequent valid arrivals; they cannot silently resurrect
an old prepared image. The GPU owner watches production consumer registration.
A changed production consumer set fences prepared tokens; producer arrivals
and held-frame refresh create new immutable views on the resource owner.
Old GPU reads remain charged until they finish, with one retiring generation
per source. Monitor-only registration does not fence production preparation.
Same-adapter consumer recreation has real BGRA/I420 pixel tests; preparation
device loss and adapter migration remain separate qualification.

WGC CPU fallback transfers OS frames to the bounded capture worker and offers
BGRA before publishing its descriptor. The descriptor snapshots its logical
epoch and calibrated WGC observation time with those exact pixels. Geometry
changes fence the generation on the capture owner. Its idle callback refreshes
stopped held tokens without new CPU bytes or a new observation time. CPU demand
preserves pending capture order; capacity refusals remain reported as drops.
Direct GPU capture and its separate CPU/ISO branch retain their own payloads.

Browser input uses one bounded reader thread with at most 64 sources when
preparation is enabled. OS mapping, seqlock BGRA copying and preparation run
outside the adapter's metadata lock. Program polls immutable descriptors.
Each actual host spawn receives a unique SHM name and reader generation;
retained old readers cannot collide with a restarted host or publish into a
removed/replaced source. Reader observations are stamped on exact copied
pixels; the browser SHM ABI does not carry host acquisition time. Held tokens
refresh on the reader. Original CPU/ISO bytes and legacy preparation-off
polling remain available. SHM BGRA uses its existing separate owner.
Installed output continuity, receiver/display identities,
latency, A/V and hardware qualification remain governed by #802/#517 and the
[render delivery specification](render-delivery-spec.md).
