# Recording write-queue specification

Implementation specification, October 7, 2026. Work ranking stays in [BACKLOG](../BACKLOG.md). This document defines the recording-path queue split. It is not a second work queue.

The live Program buffer stays 2 or 3 frames. Recording does not. A disk stall must become latency in the file while a bounded write queue has room, and an explicit drop only after that queue is full. One slow ISO file must not evict Program or another guest.

Pre-roll is out of scope. vMix does not arm a circular buffer on normal program or MultiCorder recording either. Seconds of history belong on a replay path, not on this recorder.

## Why this is a gap

vMix keeps a Recording Memory Buffer (recommended value 10) between the encoder and the disk so a slow write does not become a dropped frame. Instant Replay sizes that cushion in seconds. CoreVideo Pro does not have that split.

Current recording path, from [iso-recording.md](iso-recording.md):

- The approved Program presentation buffer is 2 or 3 frames, default 3, applied after restart. At 60 fps that is 33.333 or 50 ms, with matching Program audio delay. A render overrun absorbed there is diagnostic, not an output failure.
- ISO video is arrival-driven. The render gather appends each newly seen `(sourceId, frameId)` to `pendingIsoVideoQueue_`, capped at `kMaxPendingIsoFramesPerSource = 4`. `renderIsoVideoTick` drains the queue. A late tick adds latency, not lost frames, up to that cap.
- `AsyncEncoderSink` is one dispatcher. Each ISO file has its own `RecordingTrackWorker`, so encode is parallel. Under disk pressure the sink drops ISO video to latest inside its budget. Program is protected by priority, not by a deep write queue.
- The 2026-09-18 eight-ISO 1080p60 run shed 8,482 ISO video items over about 100 seconds and did not recover inside the session. Dispatcher drops and writer drops are already split: `encoderEvidence.isoVideoBySource.<id>.dropped` versus `recording.streams[].droppedFrames`.

That is live-path plumbing with a short cushion. It is not a recorder.

## Outcome

Deliver playable Program and per-source ISO files that keep every gathered frame across a disk hiccup no longer than the write queue. Presentation cadence, the 2–3 frame Program buffer, and Program audio delay do not change. Overflow stays loud and charged to the writer that lost the picture.

Do not add steady-state latency to Program, Preview, multiview, virtual camera, or stream senders. Do not unbounded-queue frames into RAM. Do not implement pre-roll or instant replay in this change.

## Ownership

| Component | Owns | Must not |
|---|---|---|
| Program presentation buffer | 2 or 3 composed frames for on-air delivery | Wait on disk, encoder open, or an ISO writer |
| Gather (`pendingIsoVideoQueue_`) | Arrival dedup and a short handoff into the recorder | Become the disk cushion. Cap stays a gather bound, not the write bound |
| Program recording queue | Bounded FIFO of Program video and Program audio for the program file | Drop while it has room. Evict an ISO frame to save itself by stealing another writer's slot |
| Per-ISO write queue | Bounded FIFO of that source's video frames and audio stem packets, owned by its `RecordingTrackWorker` | Evict Program or another source. Share one global ISO budget that bills the arriving guest for a frame another guest lost |
| Dispatcher | Route a gathered frame onto the owning writer's queue and return | Encode, mux, or block the render thread on disk |

Hand off frame references, not pixel copies, until a writer must convert or encode. I420 to NV12 stays off `coreMutex` and off the audio worker, as it does now.

## Queue contract

Default depth is 10 frames of video per writer, Program included. That matches the vMix recording-memory recommendation and is a capacity limit, not intentional latency. At 60 fps a full queue is about 167 ms of file delay, not 167 ms of on-air delay.

Startup retains the existing bounded burst allowance (at most 96 video items,
also subject to the file's byte reservation) until the first committed sample's
backlog drains into the steady-state limit. Codec open may exceed the ten-frame
window. Removing this allowance reproduced a clipped ISO head in the existing
eight-writer test. This is startup admission, not pre-roll; startup loss remains
separate and visible. Steady-state capacity is the configured 4–30 frames.

Audio uses the same time window, sample-counted, on that writer's audio queue. A video queue at 10 frames and an empty audio queue is a bug. Silence-fill on the shared `RecordingPtsClock` epoch stays. A gap is filled, not slid.

Rules:

1. Enqueue at gather time. PTS is the gather stamp, not the write time. A drain must not collapse a burst onto one instant.
2. The queue is FIFO. Do not drop-to-latest while space remains. Drop-to-latest is the overflow policy only.
3. Program never drops while its queue has room. ISO overflow drops that source's oldest queued picture, charges that source, and leaves every other writer alone.
4. A writer blocked on disk stops accepting only after its own queue is full. The dispatcher does not sit in `WriteSample`.
5. Startup drops stay in `startupDroppedVideo` / `recordingStartupDroppedAudioPackets` until the writer's first committed sample. They must not poison the steady-state loss counter.
6. Stop drains each writer's queue, then finalizes that file. A stop does not discard queued frames to make finalize look fast. Finalize timeout is explicit and counted.

Depth is a setting on the recording session, default 10, range 4–30. It is not the presentation buffer setting. Changing it does not require an app restart unless the encoder session is already open; a change applies on the next Record start.

Memory is bounded. 10 frames of 1080p60 NV12 is about 30 MB per writer before encode. Eight ISOs plus Program is about 270 MB of uncompressed references if every queue is full and frames have not been encoded. Prefer holding compressed access units once the worker has encoded them, and keep at most one uncompressed frame in flight per worker. Report high-water bytes on the recording snapshot. Refuse to raise depth if the projected cap exceeds the existing frame-allocation budget; say so, do not silently shrink.

The Windows implementation uses raw frame references: Media Foundation's sink
writer owns encoding and muxing together. The ten-frame FIFO absorbs a blocked
writer; it is not a separate encoded-packet queue. A recording retention ceiling
of 512 MiB is separate from the GPU preparation pool. Projection includes the
short dispatcher handoff, one in-flight frame per writer, thumbnails and audio;
per-file byte reservations also enforce the ceiling during startup or a source
size change. A byte refusal must remain distinguishable from a full video FIFO.

## Failure behavior

| Condition | Recording result | Live result |
|---|---|---|
| Disk slower than realtime for less than the queue | Frames land late in the file. No drop counter increment | Unchanged |
| Queue full | That writer drops, charges itself, surfaces `recording.warning` with the source name | Unchanged |
| Program queue full | Program recording warns. ISO queues are not raided to save Program | Unchanged |
| Writer open fails | That file is refused and loud. Program still records | Unchanged |
| Core restart mid-record | Existing resume-in-a-new-folder behavior. Queues do not survive the process | Unchanged |

Unknown delivery stays unknown. A queue depth reading is not proof the file is playable. Finalization evidence remains the playable-file check.

## Evidence

Extend the existing recording and encoder evidence. Do not add a parallel status path.

Per writer, Program and each ISO:

- `queueDepth` configured, `queueHighWater`, `queued`, `written`, `dropped`, `startupDropped`
- `oldestQueuedAgeMs` while recording
- `videoWorkUs` and `completedVideo` stay, so a mean over 16,667 µs at 60 fps still means that track cannot hold rate

`session.encoderQueueDroppedVideoFrames` may keep summing dispatcher and writer drops, but the spec requires the split counters above. A single summed number cannot diagnose this change.

Operator surface: if any recording writer is dropping, Health says which file and whether the queue is full. Do not invent a new panel in this slice. A support-bundle field is enough until the owner ranks a UI row.

## Slices

1. Program write queue only. Inject a stalled program writer. Frames that fit are written after the stall. Presentation underruns do not increase. Existing program A/V tests stay green.
2. Per-ISO write queue, replacing global drop-to-latest. An eviction test must charge the source that lost the picture. A fast guest must not be able to empty a slow guest's queue while the slow guest still has room.
3. Evidence and the depth setting. Headless `validate-iso-record.mjs` grows a stall case: eight sources, injected write block shorter than the queue, zero steady-state drops, non-zero high-water.
4. Installed acceptance only after the owner ranks it. A synthetic stall is not a show-night proof.

Slice 1 lands with its test. Do not merge the queue type with no consumer.

## Tests

- Program queue absorbs a blocked write and commits the held frames in order, PTS unchanged.
- ISO queue full drops only that source. Program `dropped` stays 0.
- Gather cap and write-queue cap are different constants. Filling the write queue does not change `kMaxPendingIsoFramesPerSource`.
- Stop drains, then finalizes. A moov is present. No 0-byte tail.
- Presentation buffer setting still reads 2 or 3 after the recording depth is set to 10.
- The seven `EncoderRecordingSession.*` Windows tests still see exact counts on the wrapped sink. Async snapshot lag stays on `AsyncEncoderSink::session()`, not on the Media Foundation sink.

## Non-goals

- Pre-roll, arm-to-record, or a replay circular buffer.
- Changing Program presentation depth, stream sender queues, or monitor isolation.
- Sharing one hardware encoder across ISO files.
- Raising ISO fidelity above the render gather rate. A source faster than the gather is still capped at about 60 distinct frames per second until `ZoomEngineRuntime` stops publishing latest-only.
