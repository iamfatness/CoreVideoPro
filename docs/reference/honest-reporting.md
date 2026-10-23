# Honest reporting: measured or removed

Owner direction, 2026-10-22, before the closed beta: every panel, control and
value the operator sees is wired to something real or it is removed. A fake
metric is worse than none, because it sends diagnosis down the wrong path
(#519 did exactly that during the 2026-09-13 YouTube incident). No feature is
left half-wired to look complete.

## The rule when you add or touch a value

- A number shown as a measurement is measured at the place the thing happens
  (bytes at the pipe, network quality from the SDK callback, meters from DSP).
  If you cannot measure it, do not publish the key. Absent is honest; a
  placeholder is not.
- A control changes something the core applies. If the core only stores and
  echoes a field, the shell must not offer it. Echo-only fields are removed from
  both ends, not left for "later".
- A diagnostic claims only what its source proves. A roster is not video
  evidence; a frame counter advancing is not a picture; "idle" is not a render
  plan. Sections that are always empty are dropped from the bundle.
- Hiding is acceptable when the real implementation is a product decision
  (licensing, captions/STT); fabricating is never acceptable.

## What the October 22 pass changed (PRs #1020, #1021, #1023)

Removed from the shipping path: the License & Plan panel and the entitlement
gate on Set & Forget; caption style controls, the fabricated caption
latency/confidence and the `set-caption-enabled` command (caption cues via
`push-caption-cue` stay real); the no-op OHG per-input lower-third title; the
stub `services/licensing-api` and `services/caption-broker` workers; roster
`audioLevel` (both the core's `talking ? 78 : 12` and the shell's `? 78 : 22`);
RTMP/NDI sender `latencyMs`; brand-kit `logoText`/`lowerThirdStyle`/
`captionStyle`/`defaultOverlayBehavior`; route `audioRole` and strip
`inputLevel` on the wire; the always-"idle" render plan, `DeliveredFrameCount`,
`OperatorActions` and `EventLog` bundle sections.

Made real: `autoProduction.slotBindings` bound and mapped so Set & Forget and
Magic Scene act on the core's bindings; `networkQuality` from the SDK callback,
absent until reported; `bytesSent` measured at the FFmpeg stdin pipe (NDI: bytes
handed to libNDI); `ndiGroup` reaches `p_groups` and a change recreates the
sender; `rosterEpoch`/`rosterRevision` bound with the revision-0 case decided
(`ZoomRosterSnapshotPolicy.HasRosterFact`); first-frame evidence only from frame
events or spine counters; telemetry `CaptureSourceCount` counts connected
devices; the media-mix "no native PCM" warning only when true.

Verified real and kept: the built-in look presets (`ColorGradeParams.h` feeds
the GPU grade shaders); per-participant rms/peak on the synced-console path;
the SRT configured latency (a setting, not a measurement).

## Traps

- The shell marks some snapshot members `required`; removing a key from the
  core before the shell stops requiring it throws on every snapshot. Merge the
  shell side first (#1021 before #1023) or together.
- An audit written by reading code goes stale fast. Verify each claim at the
  current line before removing anything; the LUT claim in #764 was stale by the
  time it was acted on.
- Not live-verified in this pass: NDI group on a real receiver with a group
  filter; the autoProduction path end to end (unit-tested to the mapper and
  coordinator only).
