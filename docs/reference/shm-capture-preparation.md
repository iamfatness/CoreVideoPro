# Shared-memory capture preparation

The WinUI BGRA capture bridge uses `ShmCapturePreparation` behind
`WinUiCaptureDeviceAdapter` (#797, parent #517). Program consumes completed
immutable frame descriptors through the existing capture/source-bus seam.
Mapping open, pool allocation, full-frame copying and normal resource retirement
run on the preparation owner. The render consumer does not wait for that copy
or acquire its mapping lock. This does not change monitor isolation or GPU
capture ingress defaults.

Each source has four reusable CPU buffers. An externally held buffer cannot be
overwritten; exhaustion holds the latest good image and reports
`capture-pool-busy`. One preparation thread polls all registered mappings every
two milliseconds. A stalled preparation can delay freshness of these sources,
but does not stall unrelated Program consumption. It is not a promise of
independent preparation latency for every source.

The bridge admits at most 16 desired sources and 512 MiB of logical mapping plus
pool payload. Active and retiring generations share this budget. There is at
most one retiring generation per source; further reconfiguration waits with
`capture-retirement-pending`. A frozen last-good frame survives reconfiguration
until the replacement is ready. Late completion of the old generation cannot
replace that frozen image. Unregister removes the source immediately from
consumption and reclaims its mapping when outstanding payload leases end.
Shutdown joins the preparation owner after rendering has stopped.

The legacy registration command acknowledges a queued request. Admission,
mapping-open, allocation and dimension failures are observable separately in
`realtimeEvidence.capturePreparation`, not a new synchronous command result.
`residentBytes` is the logical charge of each mapped payload, its 16-byte header
and four CPU buffers; it is not process working-set or total GPU memory.
`memoryAccounting` names this accounting explicitly. Reasons are fixed values;
no mapping names or exception text enter diagnostics.

An even, changed sequence and a stable header around the copy are required.
Sequence zero before the first arrival and odd/in-progress writes cannot create
a fresh frame. A torn copy is rejected. Each accepted copy has a monotonic
prepared identity and registration generation (`sourceEpoch`). Its
`captureTimestamp100ns` records the steady-clock read-observation boundary,
not upstream camera acquisition. `senderAcquisitionTimeVerified` remains false.
These identities do not prove that every publisher frame was delivered.

Native `ShmCapturePreparation.*` tests exercise real Windows mapping pixels,
immutable pool leases, exhaustion, empty/in-progress/torn writes, resize,
reconnect, generation fencing, retirement and Program consumption/composition
while copying is blocked for at least 25 ms. The maintained
[`monitor-isolation-ab.py`](../../scripts/qa/monitor-isolation-ab.md) retains
capture preparation counters alongside delivery evidence. Run Release builds
with no competing build or QA workload; retain every failed trial.

This slice covers the shell BGRA fallback bridge. CPU ISO arrival ordering,
consumer-demand pruning, other CPU preparation paths, actual WGC/UVC capture,
GPU-ready/display/receiver frame identities and installed/fleet qualification
remain under the [render specification](render-delivery-spec.md). Passing this
bridge's tests cannot close #517 or justify enabling isolation by default.
