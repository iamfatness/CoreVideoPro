# Zoom format generations and CPU playout (#908)

Zoom's stream epoch changes when dimensions change. The preparation owner used
to stop the old epoch immediately when the first new-format arrival was offered.
CPU frame synchronization can still be selecting that old epoch. Its exact GPU
token then rejects acquisition as `preparation-stopped`, and Program admission
clears the held image. This explains why source-content acceptance can fail
while Program identities and camera publication continue advancing normally.

Keep format identity separate from guest lifetime. Zoom assigns a process-local
preparation lifetime when a stream is registered or its participant/source
generation changes. Dimensions change the format epoch but preserve that
lifetime. The actual arrival tap passes this lifetime to CPU preparation; other
source adapters retain their existing immediate replacement fencing.

A healthy format replacement in the same nonzero lifetime retains its
predecessor as a draining generation. It can finish preparing exact old CPU
selections until Program selects a token in the successor. That selection marks
the predecessor stopped; its existing GPU owner drains leases before retirement.
The existing generation, active-resource and memory caps still apply. No new
CPU playout buffer or future-image substitution is introduced. Capacity refusal
remains attributable and fails acceptance.

The first successor selection can precede that format's GPU resource readiness.
Program may temporarily keep the exact old image only for a verified equal
nonzero lifetime and a strictly newer requested epoch. The actual old epoch,
frame, dimensions and capture timestamp remain attached to those pixels, with
reason `same-lifetime-format-transition`; both lifetime IDs are exposed as
decimal strings in admission JSON. The capture-time difference is bounded to
100 ms so stalled preparation cannot masquerade as an indefinite transition.
Failed/stopped successors, future images, mismatched tokens and changed
lifetimes never authorize this hold. The QA consumer validates this explicit
transition rather than treating arbitrary cross-epoch images as available.

A reconnect, reassigned participant, restarted helper or failed preparation
owner does not qualify as a format replacement. A changed lifetime invalidates
the current and all draining older generations even if capacity prevents the
new allocation. Program's existing epoch/dimension and identity checks remain.
Explicit source removal is still governed by runtime roster/playout invalidation;
this change does not render a removed guest from an old image cache.

Validate exact old pixels after a new-format arrival, successor selection and
old-lease retirement, plus reconnect fencing. Retain all historical #908 trials
and repeat real-meeting resolution ramps with source admission evidence from
the first required frame. Clean publication/copy counters alone do not pass
source-content qualification; missing source evidence remains unverified.
