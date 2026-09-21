# Zoom PCM delivery through the source bus

Scope: #535 audio delivery slice. Priority changed by the owner on September 21:
#535 and #555 are active; #513 idle validation is deferred after an overnight run
without recurrence. Order lives in BACKLOG, not this design.

Zoom engine/fallback polling stays outside coreMutex. The existing runtime owns
SDK packet draining and the 10ms-to-20ms priming flag. Under coreMutex the gather
moves that batch into the same per-participant sources used for video, then
drains PCM through SourceBus before the unchanged coalescer, steady feed, mixer,
Program delay and participant ISO routing. Packet timestamps, channel count,
sample rate, sample count, attribution and priming are preserved unchanged.

ISource has a separate audio poll because rendering and audio run at different
cadences. Render polls never consume pending audio. The staging batch is drained
in the same gather: no new worker, lock, extra buffering interval or DSP path.
Multiple SDK packets from one source stay ordered until the existing coalescer
concatenates them. All bus access remains in the coreMutex ownership domain.

An audio-only participant uses the same identity as its later video. Video
roster removal clears video while audio is present; an audio batch absence clears
audio capability, and a source with neither facet is retired. The established
empty-video-roster hold rule is preserved. Video health continues to depend only
on video: fresh audio must never conceal stalled camera content. Audio-only
health counts actual PCM, not metadata-only placeholders. Snapshot counters
report PCM packets and per-channel sample frames separately from video frames.

Validation covers consume-once PCM, packet order, unchanged timestamps/priming,
independent video/audio cadence, audio-only-to-camera transitions, facet removal,
and metadata-only counter truthfulness. Existing mixer/ISO attribution tests,
stub gate, real Windows native suite and recorded A/V clap gate remain required.

This slice does not retire the underlying legacy producer interfaces, migrate
media/capture PCM, or establish the entire #535 completion criteria. Those
require their own lifecycle/producer migrations; naming a wrapper differently
would not retire the old paths.

## Recorded sync dependency (#579)

The recorded clap gate exposed an existing mismatch between Zoom input clocks:
its PCM feed primes three 20 ms blocks, while the video queue previously drained
one frame on every render fetch. At a 30 fps camera / 60 fps renderer, the video
reserve emptied despite continuous input. The source-bus move did not add this
queue or change PCM timing.

Zoom video now becomes eligible 60 ms after core ingest observation. This is the
nominal age of the first sample in the primed audio block: the two retained
20 ms blocks plus the current capture block. Eligibility uses monotonic time,
not frame IDs or render calls, so resolution/FPS changes cannot drain the
reserve early. The queue is capped at twelve frames; a late renderer selects
the newest eligible frame and counts older eligible frames as overwritten.
Unsubscribe/rejoin still retires the entire source queue.

This intentionally adds source-video latency compared with the old drained
queue. It does not reduce the audio reserve, add audio delay, change recording
PTS, modify image quality, or change the selected 2/3-frame Program buffer.
The existing FRAME_SYNC=0 diagnostic bypass remains a control, not an acceptance
configuration. Source callback jitter means this is a nominal alignment policy,
not a claim of sample-exact camera capture timestamps. Both recorded paired-event
validation and real-source acceptance are required before closing #579.
