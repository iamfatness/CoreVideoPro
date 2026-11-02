# NDI output, independent of Streaming — design

**Status:** proposed 2026-11-01. **Owner ask (2026-11-01, after the NDI graphics key shipped in
beta-2026-11-01-765b7ce):** "NDI really shouldn't be tied to Streaming right? It isn't the same
thing. Just something that should be able to be enabled."

## Problem

NDI is a local, always-available video output, like the virtual camera. Today it is wired as one
of the **stream destinations** (RTMP / SRT / HLS / NDI):

- The shell adds `ndi` (and `ndi-graphics`) to the program-output destination list only while
  `StreamingRequested` is true (`MediaCoreCommandBuilder.BuildOutputCommand`, ~693-714). Feeding a
  switcher, vMix or OBS over NDI therefore needs the **Stream** button, and the UI says
  "Streaming" with no platform involved.
- Stopping a YouTube stream stops NDI. A stream validation or profile error stops NDI with it
  (`OnStreamOutputOptionChanged` ~10067-10120, `SyncOutputProfileChangeAsync` ~10035).
- Stream start proof requires every requested destination live, NDI and the key included
  (`TransportStatusFormatter.IsStreamingStartProven` :23-39), so an NDI hiccup fails a stream start.
- NDI frames are counted under the footer STREAM readout (`StudioOutputTelemetry.Streaming`), in
  `LiveProductionSync` stream summaries, and as "Streaming to N destinations" in the close guard.

The **core does not care**: NDI senders take Program frames directly, run on the destination list
of `start-program-output`, and already work in a session with no RTMP (Stream with only NDI ticked
is valid today). The coupling is entirely in the shell.

## Decision

**NDI out is its own output with its own on/off, modelled on the virtual camera.**

| | Today | After |
|---|---|---|
| Turn NDI on | Tick NDI in the Stream destinations, press **Stream** | Press **NDI** in the Studio footer (or Settings › NDI › NDI output) |
| Stop a YouTube stream | NDI stops | NDI keeps running |
| Stream validation / profile error | Stops stream and NDI | Stops the stream only |
| NDI name/group invalid, runtime missing | Blocks or stops the stream | Turns NDI off only, with the reason on the NDI button |
| Footer | NDI frames counted under STREAM | Own **NDI** tile with status; STREAM counts platforms only |
| Persisted | Checkbox persisted, but only meaningful while streaming | NDI on/off persists across launches, like the virtual camera |
| Graphics key | Rides with NDI while streaming | Rides with NDI output (unchanged rule, new parent) |

### Behaviour

1. **On/off.** `NdiOutputEnabled` (shell, persisted). While on, the shell's production sync
   includes `ndi` (and `ndi-graphics` when the key is on and can be sent) in the
   `start-program-output` destinations, with or without Stream or Record. With nothing on (no
   record, no stream, no NDI) the sync sends `stop-encoder-session` as today.
2. **Independence.** Starting, stopping, failing or reconfiguring the stream never adds, removes
   or restarts the NDI senders; toggling NDI never touches stream or recording senders. Both are
   destinations in the one desired-state list the core already diff-edges per destination id.
3. **Format.** NDI keeps today's format: the **stream output profile** (resolution and fps from
   Settings › Stream). That profile is the core's output render profile (`outputFps_`), sent with
   every `start-program-output`, so a NDI-only session renders exactly like a stream session.
   Changing the stream profile while NDI is on resyncs NDI (recreates the senders at the new
   format); it never stops NDI. *(A separate NDI format is out of scope; see Open questions.)*
4. **Validation.** NDI has its own validation: program name serialisable and unique against the
   key name, `ndi-output` capability present. A failure refuses the toggle (UI and API) and, if it
   appears while NDI is running (e.g. the name is edited to an invalid value), turns NDI off with
   the reason; it never touches the stream. Stream validation no longer considers NDI ("Select at
   least one stream destination" means RTMP / SRT / HLS).
5. **Proof and status.** Turning NDI on is proven when the `ndi` sender reports `live` or
   `starting` (no frames are required: a sender with no receiver is healthy). The NDI tile shows
   `Off` / `Starting` / `Live · <name>` / `Warning · <reason>` / `Unavailable · <reason>` from the
   `ndi` sender's lifecycle, plus `+ key` when the graphics key sender is live.
6. **Stream telemetry excludes NDI.** STREAM tile frames, "Partial", stream health, stream start
   proof, no-sender and health failures, `IsStreamingLive`, `SummarizeStreamStat`,
   `SummarizeOutputSession` and the close guard's stream count ignore `ndi` and `ndi-graphics`.
7. **Close guard.** NDI on does not prompt on close (same as the virtual camera).
8. **Migration.** Preferences v16 adds `NdiOutputEnabled` (default false). The v15 `StreamNdiEnabled`
   flag is read once and dropped; it does **not** turn NDI on at launch (it meant "include NDI when
   streaming"). Program name, group, key and key name keep their stored values and keys.
9. **Control API.** `transport.ndi.set {on: bool}` and `transport.ndi.toggle` (refusals return the
   NDI validation reason and change nothing). `transport.stream.ndi.graphicsKey.set` is renamed
   `transport.ndi.graphicsKey.set` (shipped one day earlier in a beta; no alias). Control state
   gains `ndiOutputOn` and `ndiOutputStatus`; OSC feedback gains `/cvp/state/ndi` (0/1).
10. **UI.** Studio footer: an **NDI** toggle tile beside **Virtual Cam**, with a flyout for program
    name, group, graphics key and key name. The NDI checkbox is removed from the Stream flyout's
    destination row and from Settings › Stream destinations; Settings › NDI gains an "NDI output"
    switch above the existing fields.

### Not changing

- Core C++: no behaviour change. It already runs NDI-only destination lists, keeps the recording
  generation across destination edits (#466), and treats every destination independently. One
  native test pins that removing `rtmp` from the list does not restart `ndi`.
- The NDI sender, the graphics key, the program buffer matching and the stale-key rule.
- NDI ingest (receiving NDI as a source) is a different feature.

## Open questions (owner)

- **Separate NDI format.** NDI follows the stream profile. A show that streams 720p30 but wants
  NDI 1080p60 needs a separate output render profile; the core renders one output profile today.
  Defer until asked.
- **Close prompt.** NDI on does not prompt on close, mirroring the virtual camera. If a house
  switcher depends on it, a prompt may be wanted.

## Acceptance (owner at the keyboard, installed build)

1. With Stream and Record off, press **NDI**: the program source appears on a receiver within 2 s;
   the footer shows `Live · <name>`; STREAM still says idle.
2. Start and stop a stream to a platform (or a local SRT sink) while NDI is on: the receiver's
   picture never drops; STREAM counts only the platform.
3. Enter an invalid NDI name while NDI is on: NDI turns off with the reason; a running stream is
   unaffected.
4. Graphics key on: the `Graphics` source appears with NDI only; turning NDI off removes both.
5. Relaunch: NDI comes back in the state it was left.
