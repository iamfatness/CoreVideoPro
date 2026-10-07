# Isolated monitor input admission

The isolated monitor worker accepts each source independently (#801, parent
#517). Program detaches its production GPU leases before enqueueing; only an
explicitly private monitor image or valid CPU representation can enter the
optional path. A production alias or mismatched private-image dimensions is
rejected for that source. Production and monitor pool generation counters are
independent, so equality of those counters is not an identity check. Capture
publishes the two representations from the same captured sequence.

`MonitorInputCache` runs entirely on the monitor owner. It retains one immutable
last-good frame per demanded source, at most 64 sources and 256 MiB of logical
payload references. This charge includes CPU buffers and one BGRA image payload;
it is not total GPU residency, process working set, or driver accounting. The
capture pools retain their independent allocation budgets and GPU read leases.
Refusing cache retention never refuses rendering an otherwise ready source.

When an optional copy is unavailable, that source holds its last good content.
Healthy sources still compose in Preview, multiview and requested exports. With
no prior content the source remains unavailable and follows the compositor's
existing slate behavior. All missing inputs are explicit; the worker can still
complete a composition. Recovery replaces the held image on the monitor owner.
Undemanded entries are removed there, including resize/reconnect replacements.
Demand here is the typed request's plans and exports, not a new UI visibility
signal; wider consumer visibility and GPU pool retirement remain parent scope.

Per-source result observations distinguish ready, held and unavailable inputs.
Held images retain their actual source epoch, frame ID and capture timestamp;
the requested epoch is separate (zero when no current arrival exists). Ready
means usable content, not proof of a fresh arrival or displayed frame. No hold
is stamped with the next job's arrival identity. Source-level degradation is
separate from worker initialization/render failure and Program delivery health.

Snapshots and support bundles expose ready/held/unavailable counts, retained
logical bytes and cumulative retention refusals. Older, disabled or uncompleted
workers remain unknown in the support view. These counters do not establish
physical display presentation. Isolation and GPU capture defaults stay disabled
until the parent qualification gates pass.

The native snapshot also exposes a bounded `monitorWorker.inputs` observation
with its own completed-job sequence and up to 64 source identities/states. An
explicit omitted count distinguishes truncation from absence. This sequence is
independent of the diagnostic counters' sampling instant. It carries no backend
handles or payloads; an uncompleted/unsupported worker reports observed false.
