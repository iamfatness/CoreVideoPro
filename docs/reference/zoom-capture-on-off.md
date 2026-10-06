# Zoom capture on/off (engine raw-media stop — 2026-07-19)

_Moved verbatim from `CLAUDE.md` (#737). Paths are relative to the repo root._

Capture-off must stop raw media IN THE ENGINE, not just our spine payloads:
the shell's Capture toggle OFF sends `zoom-stop-capture` (in addition to
`ConfigureZoomSpineSync(null)`) → core `MediaCore::stopZoomCapture` →
`ZoomEngineRuntime::stopCapture` enqueues `stop_media` on the sender thread
(never a direct pipe write) → the engine's command loop runs
`stop_raw_media`: `StopRawRecording()` (this is what clears Zoom's
participant-facing recording indicator) + `unsubscribe_all` across
video/share/audio. Without it the recording banner stayed up and frames kept
flowing after the button went red. `stopCapture` also clears the
`sentSubscriptions_` dedup + `mediaStarted_` so Capture ON re-arms through the
EXISTING path (spine `startCapture:true` → `ensureMediaStartedLocked` →
`start_media` → engine `start_raw_media` re-request + resubscribe). The engine
emits a first-class `raw_media_status {active}` event on every start/stop; the
core mirrors it as `rawMediaActive` in the zoom snapshots and the shell's
Capture status line reflects that engine-reported truth ("Capture stopped —
Zoom recording indicator cleared"), polling briefly until confirmed — never
claiming stopped on hope. (This section belongs with the engine-teardown rules
from PR #302 once that lands.)

**THE POLL CAN DIE SILENTLY IF START/STOP RACE — they are atomic now (2026-09-10, live on
beta-2026-09-10-5a24225).** Owner: "Audio meters aren't showing live data… If I slide a channel it
updates." `/snapshot` aged to 130 s+ while the core rendered at 58 fps. Evidence chain, all from
the running shell with no input touched: `dotnet-trace` showed no poll work at all (only the spine
sync and command replies), a 10 s exception trace showed ZERO exceptions (so polls were not failing,
they were not happening), and a `dotnet-dump` + `dumpasync` showed NO pending poll. Reading the
bridge object out of the dump settled it: the one live poll timer was armed with generation **17**
while `SingleFlightTimerWork` was at **20**, so every tick returned at the generation check. Cause:
`EnsureMediaCoreRunningAsync` is built to be called by several startup edits at once, each reaches
`StartAsync` → `StartPolling`, and start/stop were three unsynchronised steps (reset, dispose,
assign) — a stop reset the generation and disposed the newest timer, then an older start still in
flight installed ITS timer. Only operator commands (a fader, a source pick) carried fresh state
after that, for the whole session. T1.5's launch retries made the interleaving far more likely.
`StartPolling`/`StopPolling` now run under one leaf lock (`_pollTimerGate`), so the installed timer
always carries the current generation. Test: `MediaCoreBridgePollTimerRaceTests` — 8 threads x
1000 rounds of concurrent start/stop; it fails 3/3 runs with the lock removed. **Diagnostic
lesson:** when a periodic loop "stops", check its generation guard against its timer in a dump
before theorising — a no-op tick leaves no trace in logs, CPU samples, or exceptions.

**Engine off does NOT stop the shell polling the core (T1.5, #432).** The
bridge's 250 ms poll (`MediaCoreBridgeService.PollLoopAsync`) requests the core
snapshot on EVERY tick, with Engine on or off. While capture is off in a meeting
it ALSO refreshes the Zoom roster, because the spine sync that normally carries
the roster is not running. It used to do only the roster refresh there, and
`ZoomCaptureSnapshotMerger` carried the old core fields forward. So after a join
with Engine off, `/snapshot` aged and `nativeProgramFrameCount` froze, and so did
everything bound to `LastSnapshot` (meters, output health, program buffer). In one
session that lasted 67 minutes. Operators read it as a core wedge, but Program
had rendered at 60 Hz the whole time. The decision is `MediaCorePollPolicy`: core
first, then roster, each best-effort on its own. An empty `media-core-sync` runs
no tick, but it is not free: the core takes `coreMutex` and builds
`sessionState()`, and the shell's single sync slot is held for the round trip.
This is the same per-poll cost as with Engine on. Test: `MediaCoreBridgePollTests`
(a node fake core; it asserts the poll cadence and a fresh `RawReceivedUtc` with
Engine off).
**Consequence, and a rule: A SKIPPED SINGLE SEND MUST RE-ARM ITSELF.** With the
poll now running while Engine is off, a single-send sync can collide with it and
be refused with `MediaCoreSyncInFlightException`, meaning it was NOT delivered.
Three paths used to swallow that because "the periodic sync reapplies". With
Engine off nothing does: there is no spine sync and the poll is empty. A
Preview-scene pick could be lost, and the operator could then Take a scene the
core never composited in Preview. The three paths now re-arm: the Preview-scene
sync (`QueueProductionSyncRetry`), the multiview layout (its own debounce), and
the Engine-On production sync. The spine carries only the Preview scene, so the
Program sync is not repeated either. They use `SingleSendBackpressure.RunAsync`
(`SingleSendBackpressureTests`, plus
`TransportCoordinatorTests.ToggleEngine_ASyncSkippedForBackpressureIsReArmedNotAssumed`).
Never swallow `MediaCoreSyncInFlightException` on the assumption that someone
else will resend. Same report, second half:
a launch sync that collided with that poll used to leave EngineStatus reading
"Media core unavailable - media-core sync in flight; skipped for backpressure"
until Engine On. `MediaCoreLaunchStatusPolicy` now treats a skipped launch sync
as backpressure: the core is reported ready and the retry worker delivers the
sync. Only a real failure reads "unavailable" (`MediaCoreLaunchStatusPolicyTests`).

## SDK callback reentrancy during share transitions

Zoom renderer operations can synchronously invoke raw status, frame and destruction callbacks, or wait for a callback on another thread. Never hold the share-state mutex across SDK renderer/controller calls. `RendererCallbackGate` drains admitted frame access, suspends new renderer callbacks, and releases the mutex for SDK calls. Lifecycle commands wait for a transition to finish; share-controller events during a transition request reconciliation after renderer ownership is committed. Targets and shared memory remain protected by the state mutex. This prevents the share-end reentrant mutex exception captured in #790.

A helper exit or lost IPC connection is a fatal `engine_disconnected` event, unlike recoverable mid-meeting media warnings. It clears connected/raw-media state, retires stale media work and requires a fresh SDK process on the next Join. Zoom's own crash handler may write dumps under `%APPDATA%/ZoomSDK/logs/zoomcrash_*` without a Windows Application crash event; preserve those artifacts privately.
