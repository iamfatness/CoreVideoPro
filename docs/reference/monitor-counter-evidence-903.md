# Monitor device evidence — #903

Muted/disabled dispatches currently publish a default zero instead of reading the device's cumulative dry-event count. Reenable then makes the old event appear new. The asynchronous device owner can also leave observations stale indefinitely when no packets arrive. The original recorded dry event remains unexplained; this repair must not relabel it retrospectively.

## Contract

The existing `monitorUnderruns` readout remains the measured raw device-dry total in every monitor state. Desired mute, zero volume and disable do not reset it. A true backend replacement or endpoint lifecycle change has an explicit epoch and endpoint context; raw history is not erased to make a counter look healthy. Unsupported classification remains unknown.

Capture event classification where WASAPI observes the dry ring, using the suppression intent actually applied at that time. Distinguish intentional global monitor suppression from unexpected starvation. Expose priming/suppression silence separately without converting an earlier unexpected event into harmless priming. Retain raw event totals, missing sample frames, last-event time and lifecycle, and cumulative category totals. Initial cushion fill is not retroactively counted as a new raw underrun. Do not change ring size, priming depth, rate correction, PCM gain or Program buffering.

The asynchronous owner refreshes measured evidence at a bounded 100 ms cadence while otherwise idle, and wakes promptly for changed suppression intent. It must not call devices from the render/control caller or introduce unbounded real-time waits. A blocked device operation remains visibly stale; never manufacture fresh observations. Configuration failure preserves the last valid measurement and its time. The existing native monitor snapshot consumes the evidence; a bounded rehearsal consumes and checks that snapshot.

## Implementation and validation

Use a focused allocation-free device evidence recorder with coherent bounded reads. WASAPI owns its lifecycle/event updates; suppression is an explicit atomic intent, and event classification is latched at observation. Publish evidence through the existing asynchronous observation owner and native audio snapshot rather than adding an unused diagnostic island.

Regress the observed 1 → 0 → 1 sequence across zero volume, disable and reenable; raw count must stay 1. Exercise idle freshness without PCM, true endpoint lifecycle changes, stale-operation results, and preservation of prior evidence after device failure. Deterministic event tests must prove raw/category accounting and timestamps, with suppression changes never reclassifying earlier events. Keep the existing blocked activation/render/stop and bounded PCM ordering checks.

Run the changed native checks and an authorized local WASAPI rehearsal after measured video workloads finish. Capture mute/unmute and disable/reenable, actual endpoint/epoch identity, raw/category counts and observation age. Preserve failures and original evidence. A local device/control pass does not prove physical headphone audio continuity or explain the original event. No claims based solely on an average, a silent PCM buffer or a desired-state flag.
