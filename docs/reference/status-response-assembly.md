# Status response assembly

`JsonRpcServer::run` applies commands and captures `CoreStateCapture` under
`coreMutex`. The capture retains plain control/render metadata, one immutable
audio-owner observation, immutable JSON values, and source health counters.
It does not copy Program pixels, PCM, LUT payloads or GPU resource leases.
The live cold-start thumbnail is a lease of separately bounded encoded preview
data; its encoding runs on the optional frame pump outside the render lock.

`ResponseAssemblyLane` owns one executing job and one pending job. The command
loop waits for pending capacity before acquiring `coreMutex`. Projection,
serialization, transport admission, and captured-data retirement run outside
that lock. Direct single-owner `handle()` callers retain synchronous behavior.
Shutdown drains accepted assembly jobs before media owners are destroyed.

Owner endpoints borrowed by the capture remain alive for the server lifetime.
Only their cached, internally synchronized diagnostic accessors are queried by
the assembler. Plugin status uses its metadata mutex, not the audio DSP mutex.
Sender lifecycle evidence has one projection mutex; it is never acquired by a
live command capture. Audio bus sample counts and peak/RMS meters are published
by the audio owner; projection cannot scan PCM or run DSP.

JSON strings/containers are immutable shared values. Editing requires creating
a new container, so a captured tree cannot change under a queued response.
The shell rejects a response with an older applied revision from the same core
epoch, while allowing a replacement core to restart at revision zero.
On MSVC Windows, the audio observation names the QPC clock domain, allowing the
shell to include assembly, pipe and dispatch delay in its freshness calculation.

The response transport reserves capacity outside media locks: 32 messages,
16 MiB total, and 4 MiB per message. Capacity pressure backpressures producers;
accepted responses are not dropped. An oversized response returns an explicit
error identifying that accepted mutations remain applied. Disposable thumbnail
traffic and structural surface announcements have separate bounded lanes.
Zoom thumbnail base64/event packaging occurs before its generation-checked
publication phase, outside the Zoom runtime mutex.

Async encoder/sender controls enter their owners' command lanes directly.
Synchronous adapters retain their serialization lock. Audio-monitor device
activation and delivery use a separate bounded owner with revision-fenced
completion, rather than the render or DSP thread.

The capture is bounded by the executing/pending job count and admitted core
state. This does not yet implement the spec's aggregate pre-copy byte reservation
for all control metadata, nor its allocation/retirement attribution gate.
The separate Program display handoff repair is also required for installed
presentation validation. The two-minute development meeting probe is not an
installed pixel/audio qualification. See the
[approved specification](status-snapshot-isolation-spec.md) for acceptance.
