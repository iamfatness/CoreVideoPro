# Explicit Program delivery cessation

Issue: [#941](https://github.com/iamfatness/CoreVideoPro/issues/941). Ordering lives in BACKLOG.

The retained shutdown trace charges empty Program slots after intentional core closure. D3D11Compositor currently retires grade/monitor workers before destroying its Program buffer; the scheduled delivery worker continues while its render producer has stopped. This is a lifecycle defect, not permission to ignore genuine missed output deadlines.

## Contract

At explicit compositor destruction, stop and join scheduled Program buffer workers before retiring unrelated monitor/grade owners. Keep buffer textures and leases alive until their normal destruction order releases them. Stop is idempotent and rejects further admission; it must wait for any in-flight delivery callback before returning. Preserve every pre-stop miss and diagnostic counter. Do not classify ordinary starvation, a slow producer, a stalled source, or an active-output gap as shutdown. Do not filter trace events retrospectively or change any delivery deadline.

## Execution and acceptance

Separate stopping worker activity from resource destruction in the existing buffer owner, with the compositor destructor as its first real consumer. Handle both current and pending-composition buffer owners without holding a lock needed by their callbacks. Constructor failure and normal destruction remain safe.

Use actual worker tests to distinguish a genuine source-unavailable miss while active from explicit stop; after stop, admission and callbacks cease, counters remain stable, repeated stop succeeds and retained resources remain valid until normal release. Cover an in-flight callback barrier. Retain an old-order negative control and a normal-close trace with no active outputs. No GPU/app/device workload will overlap the owner's beta tests; any unavailable local qualification stays explicit. This follow-on is separate from the frozen testing beta and must not broaden its release contents by default.
