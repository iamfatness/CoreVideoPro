# Edge feeds in CoreVideo Pro Sources

Tracking: [#1227](https://github.com/iamfatness/CoreVideoPro/issues/1227). See the [implementation plan](../plans/2026-11-22-edge-operator-view.md).

## Operator workflow

A producer running Pro on Windows can see which Edge NDI feeds are available, which guest is on each attached slot, and whether Pro is receiving video. Sources groups the Edge slots by node and orders them numerically. The producer uses the existing NDI source picker and Show Input assignment to route a feed. A different guest on a slot changes the guest label while the Show Input, scene bindings and audio route retain their existing source identity.

A discoverable feed appears as **Available** before attachment. Pro shows the advertised node and slot as a LAN discovery hint. It learns the guest and sender-side evidence once the operator attaches the feed. A node with no advertised or attached NDI feed has no row. Availability of an NDI advertisement gives no evidence about the node's Zoom session or recording state.

The first pairing is Apple Silicon Edge to Windows Pro, with Windows Edge supported by the same wire contract. Ordinary NDI and SRT sources keep their established workflow. Remote assignment, automated failover, unattended Zoom join, idle-node discovery and Edge-specific SRT integration require separate designs. NDI receive on macOS Pro is covered by the [Mac ingest design](2026-11-15-mac-outputs-ingest-browser-design.md).

## Current evidence

- Edge's [feed design](https://github.com/iamfatness/CoreVideoEdge/blob/6b4c2b90b2bf45171e0dc363d42d484cf3b85d30/docs/superpowers/specs/2026-11-09-edge-to-pro-ndi-srt-design.md) defines stable NDI names and the existing operator handoff. Its [feed worker](https://github.com/iamfatness/CoreVideoEdge/blob/6b4c2b90b2bf45171e0dc363d42d484cf3b85d30/native/src/edge/FeedSender.cpp#L78-L99) adds guest metadata to video frames. The [Edge control state](https://github.com/iamfatness/CoreVideoEdge/blob/6b4c2b90b2bf45171e0dc363d42d484cf3b85d30/docs/PROTOCOL.md#feeds-state) already exposes sender-side sequence skips locally.
- Pro's [NDI adapter](../../../native/src/modules/NdiReceiveCaptureAdapter.cpp) owns discovery and receiver lifetimes. It hashes the exact NDI name supplied by the finder, exposes per-device receive evidence, and has access to the video metadata while the SDK owns the frame. [CoreStateCapture](../../../native/src/core/CoreStateCapture.cpp) publishes capture observations to the shell. The [NDI receive drill](../../qa/ndi-receive-review.md) covers normal input assignment and signal-loss recovery.

## Identity and metadata

Use the existing NDI device ID and its `capture:<id>` source identity everywhere Pro already stores a binding. Keep the announced Edge name `CoreVideo Edge <node> S<n>`, with slot 1 through 8. Parse its advertised component using the SDK host wrapper when present. When the finder supplies a host prefix, include it in the grouping key so that identical Edge display names on different advertised hosts remain distinct. An ambiguous advertisement must stay on the ordinary NDI path. Operators use unique Edge names within a discovery domain. A renamed advertised source follows the existing NDI rebind behavior.

Read metadata in the existing receive worker before releasing the SDK frame. The payload produces display observations only. Use the existing `corevideo_edge` element and a backward-compatible addition on the same frame side channel:

```xml
<corevideo_edge guest="Avery &amp; Alex" zoom_id="42"/>
<corevideo_edge version="1" guest="Avery &amp; Alex" zoom_id="42" frames_skipped="7"/>
```

The attributes on the first line are the deployed format. Existing Pro builds can keep ignoring both formats. `guest` is plain display text. `frames_skipped` is an optional nonnegative signed-64-bit-range count within the current Edge feed worker lifetime. It counts complete Zoom ring frames superseded before that worker sent them. It can reset on reassignment, engine recovery or worker restart. Pro exposes the sampled sender count with its origin and never adds it to Pro receive drops. The legacy payload supports guest labels with sender skips unavailable. The sender publishes the latest sampled count on its existing periodic metadata frames; the first accepted video frame of a new worker carries metadata. Allow a missing sample until its worker has taken a measurement.

Restrict parsing to a bounded UTF-8 payload of at most 1 KiB, including the enclosing markup, with one self-closing element, quoted attributes and predefined XML escaping. Reject duplicate attributes, unknown versions, truncated data and malformed values. Support additive attributes within a recognized version. Cap guest text to 256 UTF-8 bytes at a scalar boundary on the sender and reject invalid or oversized input on the receiver. The guest field is optional for a valid observation. Avoid raw payload logging, and leave the legacy `zoom_id` off the Pro observation and support-export surfaces. The protocol can carry guest names across the production LAN; metadata is unverified display data and grants no control privileges.

## State in Pro

| What the operator sees | Source of evidence |
|---|---|
| Available | The NDI finder advertised a matching name and Pro has no active receiver for it. |
| Connecting | Pro started a receiver and has no accepted video frame yet. |
| Receiving video | The established NDI receive policy reports a recent accepted video frame. |
| Video stalled | The receiver previously accepted video, and frame age has passed the current 1,500 ms receive threshold. Audio may still be arriving. |
| Receiver error | The existing NDI connection marks a receiver failed. A stream warning on an active receiver is shown alongside the receive state. |
| Guest and Edge skips | A supported metadata sample arrived on accepted video in the active receive session and is at most five seconds old on Pro's monotonic clock. Once video stalls, hide these current values until a new accepted metadata sample arrives. |
| Pro drops and delivered size | Existing NDI receive counters and accepted frame geometry. A displayed frame rate must come from measured frame arrivals; keep the sender's declared frame rate separate. |

A generic or malformed metadata packet leaves valid video and audio flowing. An accepted replacement sample replaces the earlier optional values. Clear metadata on detach, receiver recreation, component restart or stall. Evaluate its age on Pro's monotonic clock, with absent values across unsupported or older contracts. A sender worker reset may lower the displayed cumulative count. A lower count has no implication for earlier observations.

Publish optional structured Edge fields through the capture-device observation and typed shell projection. Keep receiver health in the existing observation. Key UI rows by the established device ID, and coalesce changes through the existing snapshot cadence. Limit parsing and metadata copying to the receive path. Sources stays a consumer of ordinary snapshots, with no direct SDK, socket, discovery or render-thread work.

## Release evidence

Show real video and audio from an already-routed Edge slot in Pro and prove the binding survives guest reassignment and a sender restart. Pro's dropped-frame and Edge skip readouts must come from separate observations. Qualify the Mac Edge to Windows Pro workflow on a physical Apple Silicon machine, with four live guests, four reviewable ISOs, four routed NDI inputs, a 60-minute steady-state run, and an explicit breakout and sender-recovery drill. Keep fault intervals separate from normal cadence. Use the [implementation plan](../plans/2026-11-22-edge-operator-view.md) for the fixtures and evidence to retain.
