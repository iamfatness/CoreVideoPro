# Camera reader publication contention (#517)

The registered camera consumer reads one NV12 shared-memory slot at its own
60 Hz cadence. The writer marks the header odd while copying, then publishes
an even sequence. Two immediate reads can both land in the same write window;
without yielding, neither observes the subsequent complete frame. The sample
then holds its prior pixels or, after 30 missed samples, emits the standby slate.
Those are delivery failures, even when native publication counters advance.

The retained `prewarm-grid-camera-depth3` trial on the reference RTX 4090
recorded 2,977 contended reads, 2,949 held emissions and 223 slate emissions in
the registered reader. Independent OS receiver pixels recorded 2,948 repeats,
2,651 missing identities and 223 invalid identities in 3,600 samples. Program
copy counters were clean. The old DLL did not distinguish an odd header from
a sequence changed during copy; that initiating subcase remains unproven for
this historical trial. An adjacent depth-two trial was clean and does not
invalidate the failure.

## Recovery contract

Capture an absolute read deadline before pacing or sample allocation: the
original scheduled sample time plus 2 ms. A late request gets no renewed wait.
Retry only unchanged/contended publication, yielding through a high-resolution
timer for at most 250 microseconds per attempt, eight attempts, and two payload
copies shared across all attempts. Missing or invalid publishers do not retry.
Never count held/slate pixels as fresh. Never reset failure totals for acceptance.
The existing sample pacing, SHM layout, output resolution and buffer depth stay
unchanged. An explicit retry-off environment setting is a diagnostic control.

The serving log exposes writer-active observations, changes during copy, payload
budget exhaustion, recovery attempts/successes and reads finishing beyond their
original deadline. These distinguish the next reproduction's causal branch.
Deadline misses remain failures even if usable pixels are eventually obtained.

## Evidence and boundaries

Release tests use the actual mapped reader at 1920x1080. Holding its sequence
odd makes the immediate negative control fail without consuming payload copies;
completing the write during the bounded wait returns the exact payload. Deadline
tests reject retries after late entry or an overslept wait. Actual DLL tests
retain fresh/held/slate sample counts, pixels and timestamps across recovery.

The API probe can load an explicit DLL in its diagnostic receiver process.
Its manifest records that DLL hash; receiver evidence labels this `direct-dll`.
This does not qualify the registered Windows camera, Frame Server scheduling,
physical display, audio synchronization or an unlimited zero-miss guarantee.
Run the real meeting grid at both buffer selections, retain all failed trials,
then qualify the packaged registered provider before closing camera acceptance.
Source-generation failures remain separately ranked under #908.
