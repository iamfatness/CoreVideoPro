# Control command lifecycle

The operator HTTP/OSC action `settings.programBuffer.set` accepts positional
arguments `[2]` or `[3]` only. It saves the setting for the next app launch; the
current native process and its recovered children retain their startup depth.
`/state` exposes `programBufferRequestedFrames`,
`programBufferSessionRequestedFrames`, and `programBufferRestartRequired`.
`nativeProgramBuffer` separately reports the native process's observed state.
A failed durable save rejects the action and retains the previous selection;
the same request can be retried. Successful saving does not claim that the
running buffer changed or that frame performance was verified.

The native handshake includes `protocolVersion: {major: 1, minor: 0}` and a
`processEpoch`. Existing clients may omit version fields. Requests specifying an
unsupported major receive `incompatible-protocol` before execution.

Real Zoom join/authentication runs on one dedicated lifecycle worker. The normal
command executor remains available for Take, recording/streaming commands, Leave,
and snapshots. Legacy `zoom-join` callers still receive a final reply with their
original request ID and the Zoom snapshot; moving the work off-thread does not
turn a pending join into a successful meeting.

Clients that send `asyncOperation: true` receive an accepted `operation` containing
`processEpoch`, `operationId`, and `state`. A later `operation-completed` event
contains the final operation and `result`. Match both identity fields. Accepted
does not mean joined. `zoom-leave` and `zoom-cancel` invalidate the current join;
late worker success becomes `operation-cancelled`. Repeated join requests while
the worker is occupied receive `operation-in-progress`, without starting another
SDK operation. Retry only after cancellation/completion; never replay Take because
a reply was lost.

The input mailbox holds at most 128 requests and 8 MiB of wire data. Eight slots
and one eighth of the byte capacity are reserved for Leave/cancel/stop commands.
FIFO order is retained. Overload returns `control-overloaded` with the request ID
and executes no action. Individual lines are limited to 4 MiB (including room for
base64-encoded VST state); larger lines are drained and rejected as
`request-too-large` with an unknown ID because their JSON was not parsed.

Coalescing requires both `replaceableFullState: true` and a non-empty
`coalescingKey`. Only adjacent matching sync envelopes containing exclusively
allowlisted state commands can replace each other. Recording, output start/stop,
Take, VST actions, and unknown commands cannot be coalesced. A replaced request
receives `superseded: true`, without an applied-state snapshot. Current legacy
sync producers do not opt in, so their commands preserve order and effects.

Deterministic tests cover a delayed join, cancellation during a 30-second auth
wait, discarded late success, mailbox byte/entry capacity, stop reservation, and
embedded-action preservation. Physical-rig p95/maximum command latency and render
frame-time evidence remain required. Leave suppresses late Joined callbacks while left. The next join retires the old
SDK process before spawning a fresh one; reader events carry an internal process
generation so old-process callbacks cannot revive the next meeting. Outbound
response backpressure and shell overload reconciliation remain follow-up work.

Run the process-level regression with:

```powershell
cmake --build native/build --config Release --target corevideo-native corevideo-zoom-engine-fake
node scripts/validate-command-responsiveness.mjs --build-dir native/build
```

The script requires a stub build with capture adapters disabled. It injects separate
30-second auth and join stalls through the fake engine, measures 40 Ping and Stop
round trips in each stage, sends 128 MiB of input per stage while sampling core
memory, and verifies cancellation plus rejection without side effects. It writes
`command-responsiveness-evidence.json` beside the binaries with their hashes.
These are synthetic IPC measurements, not live media or GPU acceptance evidence.
