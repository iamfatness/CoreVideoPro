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
decimal strings in admission JSON. The transition is bounded to six Program
frames (100 ms at 60 Hz) from the first successor selection, also exposed in
admission JSON. This bounds replacement preparation without falsely treating
a slow incoming camera's already-held source picture as a new preparation delay.
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

October 14 retained trials: the first combined revision removed observed
`preparation-stopped` admissions but still had two first-successor readiness
gaps. The initial transition-age implementation then rejected a source whose
previous Zoom picture was already about 270 ms old. The six-frame bound now
starts at successor selection rather than the source picture's capture age.
These failed trials remain in the #904 artifact ledger.

The `ffec019` API-only real-meeting depth-two run (`source-908-age-depth2`)
passed the sampled source-content workload check throughout its 90-second
required interval. It completed 5,719 Program copies with zero refusals, skips,
repeats, late submissions or observed readiness misses. Independent direct-DLL
receiver evidence decoded 5,401 samples with zero invalid/repeated/missing/
reordered identities and zero camera read-deadline misses. Real Zoom sources
were mostly 640x360 plus an actual 2560x1440 screen; this is not eight-HD input,
registered Frame Server, physical presentation, audio or sustained qualification.
The aggregate judge in that retained report incorrectly treated unverified OS
qualification as a known failure; the correction preserves direct-DLL results
as unverified all-output qualification, never PASS. Raw counts are unchanged.


### Transient retiring-generation refusal

The `final-114-depth2` mixed cold-consumer stress retained one sampled source
admission failure: `preparation-retiring-generation-capacity`. Its refused
offer lost the caller's continuity lifetime (reported as zero), and its stopped
request was treated as a stopped producer. Refused offers now preserve verified
lifetime metadata without retaining CPU bytes. Only the specific retiring-
generation capacity case may use the existing same-guest format transition
hold, with the unchanged six-frame age bound and actual old epoch/dimensions.
A different guest lifetime, failed producer, unknown/malformed token or unrelated
capacity refusal still clears/refuses the old image. No capacity or hold interval
was increased. The source's eventual successor still needs real GPU completion.
