# RTMP packet amplification and connectivity investigation (#615)

On 2026-09-24 the operator reported that CoreVideo streaming disrupted Battle.net
and an AVD session on the same PC. The configured video rate was 10 Mbps at
1920x1080, 60 fps; a subsequent upload test measured 50 Mbps. The original
FFmpeg log stopped advancing for approximately 20 seconds while its accumulated
muxed bitrate remained about 10 Mbps. Neither that average nor the sender's
estimated `bytesSent` counter measures instantaneous network traffic.

## Confirmed defect and change

The GPU-direct RTMP path forced `tcp_nodelay=1`. FFmpeg writes RTMP headers,
payload chunks and continuation markers separately. Disabling TCP coalescing
turns small writes into excessive packet traffic on the tested Windows path.
FFmpeg explicitly documents the efficiency limitation of this setting:
[protocol documentation](https://ffmpeg.org/ffmpeg-protocols.html#rtmp),
[packet writer](https://github.com/FFmpeg/FFmpeg/blob/master/libavformat/rtmppkt.c).

Set `tcp_nodelay=0` for RTMP/RTMPS bitstream forwarding. Keep the selected encoder
bitrate, dimensions, frame rate, audio pacing and bounded asynchronous bitstream
writer. SRT arguments are unchanged. The old justification for bypassing Nagle
predated the asynchronous writer that separates transport stalls from encoder
event handling. Sustained delivery must still be checked for each receiver.

This fixes measured packet amplification. It does **not** establish that this
was the sole cause of the original PC-wide outage, which has not been reproduced.

## Measurements

Rig: Windows, Intel I226-V Ethernet at 1 Gbps, NVIDIA RTX 4090, Release native
core with installed beta `58d4fab` WinUI shell and real Zoom engine. Destination:
operator-configured external YouTube RTMP ingest. No stream keys are included
in this report or committed evidence.

Initial run with eight Zoom video sources: about 15 Mbps total adapter upload
while FFmpeg reported about 10 Mbps. A separate 100 ms sampling interval measured
12,198 outgoing packets/s on average, with maxima of 39,028 packets/s and
36.94 Mbps. This was not a 59 Mbps encoded-video reproduction.

After restarting, Zoom rejoin using the saved meeting number failed. The matched
comparison therefore used the same custom scene's animated background without
Zoom participants, and the same 10 Mbps / 1080p60 encoder settings. This removes
Zoom receive load from both sides of the comparison. The first five seconds of
each streaming interval were excluded:

| Transport | Measured interval | Mean adapter upload | Mean outgoing packets/s | TCP retransmissions/s |
|---|---:|---:|---:|---:|
| Original, `tcp_nodelay=1` | 58.8 s | 14.248 Mbps | 9,250 | 133.74 |
| Candidate, `tcp_nodelay=0` | 170.7 s | 10.912 Mbps | 1,847 | 2.21 |
| Candidate, repeated longer run | 599.6 s | 10.895 Mbps | 1,814 | 3.08 |

These are whole-adapter byte/packet counters and whole-host TCP retransmission
counters, **not** per-socket packet capture or receiver-decoded frame evidence.
An idle baseline was collected before each stream; other PC traffic can affect
the values. The matched reversal supports attribution of the packet amplification
to this option. It does not identify which router, driver or upstream condition
caused the original wider disruption.

Both conditions had occasional isolated ICMP timeouts. Do not equate those with
the original outage. No link-reset events were found in Windows System or
NetworkProfile logs during the original failure window, and adapter packet-error
counters were zero. Packet capture through PktMon was unavailable without driver
access. The operator's router and other applications were not reconfigured.

## Validation and reproduction

Release checks: 13 FFmpeg argument tests, 5 asynchronous sender tests, 9 bitstream
queue tests, and 20 destination-supervisor tests passed. The rebuilt stub suite
passed all 1,193 tests. The candidate's three-minute run held approximately
10 Mbps muxed output at real-time speed without sender restarts or encoder input
shedding. The repeated ten-minute run ended with FFmpeg reporting 36,340 frames,
605.82 s media time over 605.36 s elapsed, 9.988 Mbps muxed output and 1.0x speed.
The supervisor recorded zero restarts; encoder export remained at divisor 1 with
zero shed frames. Twelve HTTPS HEAD probes all returned HTTP 200. Gateway RTT
remained at most 6 ms. Nineteen isolated internet ICMP timeouts occurred in the
settled interval; the watchdog never saw three consecutive degraded samples.

Average real-time speed does not prove every individual frame deadline. Render
deadline misses increased from 7 at baseline to 8, with zero skipped slots;
this is **not** a pass of the product's strict per-frame performance acceptance.
Decoded external-receiver presentation was not captured. The stream was stopped
after the soak. The reusable probe was also checked in read-only mode and with
an owned ten-second start/stop run.

The reusable Windows probe is read-only by default:

```powershell
powershell -NoProfile -File scripts/qa/measure-stream-network.ps1 -Seconds 120
```

To run a bounded stream against the **currently configured destination**, explicitly
add `-StartStream`. It refuses to take ownership of an already-running stream,
stops its own stream in `finally`, and ends on three consecutive degraded gateway
or internet ping samples. Use `-Adapter`, `-Gateway`, and `-InternetProbe` for the
rig. Outputs are timestamped JSONL under `artifacts/network-qa` by default. Keep
the configured destination and source workload identical for an A/B comparison;
retain FFmpeg progress logs to distinguish network overhead from encoded bitrate.

Local raw evidence remains in `artifacts/live-601`: `network-repro.jsonl`,
`network-soak.jsonl`, `network-100ms.jsonl`, `tcp-original-matched.jsonl`,
`tcp-coalescing-soak.jsonl`, their snapshots, and FFmpeg logs. The first extended
baseline's stream was manually stopped early; its later rows retain a `stream`
label, so use the matched runs above for quantitative comparisons.
The repeated run is `tcp-candidate-matched.jsonl`; its completed FFmpeg log is
`candidate-external-ffmpeg-complete.log`. The staged candidate native SHA-256 was
`18E809215E20ADB8BD94C5D54CC9E5CC5C6F3DEE36CA7C15E87052C92BB9FFAB`.

## Remaining acceptance

Repeat with all eight live Zoom inputs using the recovered test link; verify
audio and decoded receiver delivery. The original Battle.net/AVD disruption is
still an open acceptance item. RTMPS, other network adapters, other ISPs and
other receivers have not been measured. No fleet-wide or unlimited-duration
stability claim follows from these finite runs.

## Original incident timeline and diagnostic limits

All times below are local EDT on 2026-09-24, from the preserved original app,
native and performance logs, not the later matched network experiments.

- 08:19:48: GPU encoder drain reached 3,060 samples. FFmpeg output subsequently
  stopped advancing around frame 3,033 / media time 50.75 seconds.
- 08:19:50: the sender entered backpressure at 742 ms buffered; by 08:19:51 it
  reached divisor 4. Queue-overflow discards continued until streaming stopped.
- 08:19:55.256 to 08:20:12.306: the shell's every-30-thumbnail-event diagnostic
  had a 17.05-second interval, versus approximately two seconds immediately
  before and after. These are thumbnail events, not roster snapshots. The
  sample counter is shell-local; this cannot identify whether the missing
  delivery originated in Zoom, core ingest or event transport.
- 08:20:09.678: the shell recorded an operator stream-stop request. The pipe
  failure at 08:20:09.692 followed that request and is not evidence that a pipe
  error initiated the incident.
- 08:20:25: several Zoom inputs changed from 1280x720 to 640x360; some later
  recovered. This is consistent with degraded video delivery, but does not
  identify its cause.

The Tiles membership gate removes a feed after 1,500 ms without a new frameId.
This explains how a video stall can remove a tile while the participant remains
in the meeting roster. It does not establish that the participant left Zoom.

The Windows RDP client log has no events between 08:17 and 08:24. It records a
disconnection at 08:25:43 and subsequent receive-thread watchdog warnings.
Battle.net's available Chromium log records window closure at 08:25:46, without
network-failure diagnostics for the earlier interval. These later events do not
establish the onset or cause of the operator-observed loss of connectivity.

Zoom SDK logs from the incident were preserved locally, but their contents are
encrypted. The engine currently leaves onMeetingStatisticsWarningNotification
and onUserNetworkStatusChanged empty, so those SDK callbacks provide no saved
network-quality evidence. The asynchronous RTMP writer releases its queue mutex
before blocking in WriteFile; no shared Zoom lock was identified in that write
path. Neither observation proves the absence of another blocking path.

The confirmed transport defect remains excessive small-packet traffic with
TCP_NODELAY enabled. Whether that traffic triggered the original PC-wide outage
remains unproven. Do not label the incident resolved from the lower-packet-rate
background-only runs. The saved meeting number alone timed out. The full link was subsequently recovered
from prior tasks and successfully rejoined; there is no missing-user-input blocker.


## Full meeting recovered and exercised (2026-09-24, 10:19-10:23 EDT)

The owner had already supplied the full test-meeting URL in previous tasks.
It was retrieved from those tasks and joined successfully; requesting it again
was unnecessary. The native runtime verified joined=true, synthetic=false and
10 participants. All eight routed video feeds reached 1920x1080. The link is
stored locally with CurrentUser DPAPI and is not included in this report.

Candidate, 120-second bounded run, 128 approximately one-second observations:

- Eight drawn Tiles members in every snapshot.
- Mean whole-adapter upload 11.141 Mbps and 1,967 outgoing packets/s, with the
  operator encoder target unchanged at 10 Mbps / 1080p60.
- Zero increases in render deadline misses, skipped slots, encoder shed frames
  or audio lost samples. Zero sender restarts or backpressure entries.
- Sender audio counters advanced by 5,748,480 sample frames; this is transport
  evidence, not a listening test or decoded receiver verification.
- Six isolated internet ICMP timeouts; no three-consecutive watchdog trigger.

A bounded reversal to the original TCP_NODELAY setting rejoined the same real
meeting and routed the same eight tiles. It was stopped by the watchdog after
about 15 seconds, rather than completing its planned 120 seconds. Outgoing
packets reached 16,464/s at 10:22:45, followed by three consecutive internet
ICMP timeouts at 10:22:48-50. Whole-adapter upload fell from 14.49 Mbps to
4.96, 4.98 and 2.41 Mbps. The local gateway answered throughout at 0 ms. The
last captured sender buffer reached 363 ms; eight tiles were still present in
all captured snapshots, with no recorded backpressure-policy entry. Streaming
was verified off after the watchdog.

This strengthens the evidence that the original transport setting contributes
to network degradation under the real meeting workload. It is not reproduction
of every original symptom: no Battle.net/AVD session failure was measured, the
watchdog ended before tile loss, and a run of ICMP failures alone does not prove
all internet traffic stopped. Longer candidate testing and external receiver
validation remain required. Raw evidence is local under artifacts/live-601/
full-meeting and full-meeting-original; both sampled native snapshots each second.

## Ten-minute full-meeting candidate result: acceptance FAILED

On 2026-09-24 from 15:14:16 to 15:24:17 UTC, the restored candidate ran the
same eight real Zoom video inputs at operator-selected 10 Mbps / 1080p60.
The runner completed normally, and streaming was verified off afterward.
There were 637 approximately one-second native/network samples.

- All eight Tiles members remained present throughout; all eight video-source
  frame counters and ISO audio sample counters advanced. Sources ended at
  1920x1080. This is not receiver-decoded or listening verification.
- Mean whole-adapter upload was 10.699 Mbps, mean outgoing packet rate 1,808/s,
  and maximum sampled upload 16.010 Mbps. Host TCP retransmissions rose by 1,926.
- Fifteen isolated internet ICMP timeouts; zero gateway failures, gateway maximum
  1 ms, and no sustained-connectivity watchdog trigger.
- Seven additional render deadline misses; zero skipped render slots, zero
  recorded audio lost samples, zero sender restarts. Sender audio advanced by
  28,763,520 sample frames between first and last snapshots.
- **One backpressure entry and 1,254 encoder frames shed.** FFmpeg's final
  progress was 34,716 frames, 599.85 seconds media time, 599.37 seconds elapsed,
  9.756 Mbps muxed bitrate and approximately 58 fps average. This fails the
  requested uninterrupted 1080p60 behavior.

The captured failure began at 15:22:12 UTC with 577 ms buffered. The policy
stepped through divisors 2, 3 and 4 by 15:22:13. It recovered in ten-second steps:
divisor 3 at 15:22:24.543, divisor 2 at 15:22:34.543, and full-rate divisor 1 at
15:22:44.543. The logged buffer was already zero at all three recovery steps.
Thus the existing backpressure policy prolonged frame shedding after the buffer
cleared. The queue's initiating stall still needs attribution.

Around onset, upload fell to 2.91 Mbps at 15:22:11 and fluctuated afterward,
while gateway and internet probes continued to succeed and all tiles remained
present. This is not the original PC-wide outage, and coalescing alone is not a
complete fix for output continuity. The later pipe error at 15:24:17 coincides
with the scheduled stream stop, not the start of this stall.

Evidence is preserved locally under artifacts/live-601/full-meeting-candidate-10min:
network/snapshot JSONL, summary.json, backpressure-events.json, final-snapshot.json
and ffmpeg-complete.log. The candidate executable is restored and the app remains
joined; no further stream was automatically started. Full incident acceptance
remains open, with neither release nor merge authorized by this test result.

## Follow-up pipe diagnostics

Code inspection found that queue telemetry excludes the chunk already removed
by the writer before its blocking WriteFile call, as well as FFmpeg/TCP buffers.
An empty application queue therefore does not establish end-to-end recovery.
The measured 600-tick recovery threshold explains each ten-second divisor step;
it has not been shortened without evidence about the initiating stall.

The GPU-bitstream video writer and audio pipe writer now log writes lasting at
least 100 ms, including duration, requested/written bytes and the captured Win32
error; the video entry also marks shutdown. Win32 errors are saved immediately
after WriteFile so diagnostic work cannot overwrite the failure code. These
bounded asynchronous log entries appear only after the write returns, including
cancellation; they are not a detector for a write that never returns. They do not
change queue limits, pacing, bitrate, frame rate or recovery thresholds.

Release native and test targets compiled. Focused existing suites passed:
13 FFmpeg argument, 5 asynchronous sender, 9 bitstream queue and 26 backpressure
policy tests (53 total). The first test command selected zero cases and was
replaced with verified per-suite filters; it is not counted as validation.
No new live stream was started. The diagnostic executable is saved locally as
artifacts/live-601/corevideo-native-pipe-diagnostics.exe, SHA256
D16DD4673CC6CE5FF6B6FA3CB944DA688BFFF6A09982264B9BFDBA6D085A9219.
The running app remains the earlier coalescing candidate.

## Diagnostic build: first ten-minute run did not reproduce the stall

The instrumented candidate ran from 15:39:54 to 15:49:55 UTC on 2026-09-24.
All eight tiles remained present in 639 samples. No backpressure entry, encoder
frame shedding, skipped render slot or audio-loss counter increase occurred.
FFmpeg finished at 36,036 frames / 600.70 seconds media time / 600.28 seconds
elapsed, 10.110 Mbps muxed bitrate and 60 fps average. Adapter means were
11.097 Mbps and 1,848 packets/s. No slow-pipe diagnostic event was captured.
Six render deadline misses still occurred, so this is not strict performance
acceptance and does not invalidate the earlier reproduced stall.

Evidence: artifacts/live-601/pipe-diagnostic-run. Streaming stopped normally.
The owner directed continued investigation. A separately bounded 30-minute run
started at 15:51:04 UTC with the same diagnostic binary and settings, independent
local watchdog, and first-backpressure stop in addition to connectivity/tile
checks. That run is diagnostic capture, not a claim that the defect is fixed.

## Instrumented stall captured; guarded run stopped at first backpressure

The planned 30-minute run stopped automatically after about 6m43s at
15:57:46 UTC, 2026-09-24. All eight tiles stayed present in 426 saved snapshots.
Before shutdown, the video writer logged two successful but slow WriteFile calls:
727 ms for 334,595 bytes at 15:57:45.638, then 836 ms for 10,815 bytes at
15:57:46.474. Both logged error=0 and stopping=0. No audio slow-write event was
captured. These establish actual encoder-to-FFmpeg pipe blocking; they do not
alone identify what prevented FFmpeg consuming video sooner.

Backpressure entered at 15:57:45.669 with 674 ms buffered. The next captured
state recorded one GOP-tail discard of 55 chunks. Sixteen encoder frames were
shed through the last saved streaming snapshot, increasing to 23 in the live
post-stop snapshot. The guard prevented another extended throttled recovery.
There were three render deadline misses, zero skipped render slots and zero
recorded audio loss through the saved samples.

Adapter upload fell before the pipe-write logs: 10.69 Mbps at 15:57:41.7,
9.52 at 42.7, 7.36 at 43.6, 6.73 at 44.6 and 5.96 at 45.5. Host TCP retransmissions
rose by 63 between 41.7 and 45.5. There was one internet ICMP timeout at 42.7;
subsequent internet probes succeeded, and the gateway remained responsive.
These are host/adapter counters, not proof of loss on the RTMP socket itself.

FFmpeg reported its PCM input resuming with a 1.050 catch-up rate after an
850 ms lag. The earlier failing ten-minute run also logged a 492 ms lag; the
clean instrumented ten-minute run logged none. Audio read pacing remains a
hypothesis: this message can follow blocked input OR output and does not prove
that pacing initiated the network/pipe slowdown. Do not remove it on correlation
alone. A controlled comparison or more direct transport evidence is required.

Raw evidence is preserved under artifacts/live-601/pipe-diagnostic-30min,
including the completed FFmpeg log, native/performance logs, summary and per-second
snapshots/network samples. Streaming is off. CI and CodeQL both passed for the
instrumentation commit b1fb1f6b. Neither the original PC-wide outage nor sustained
1080p60 acceptance is declared fixed.

## Controlled live-input pacing comparison (September 24, 16:10–16:14 UTC)

A local two-input FFmpeg fixture now isolates a recovery defect from internet
variability. H.264 access units arrive at 60 Hz through a 1 MiB anonymous pipe;
stereo 48 kHz float PCM arrives in 20 ms blocks through a 1 MiB named pipe.
The input/mux arguments match the GPU H.264 path, including both 512-packet
input queues. A local FLV reader pauses for 12 seconds at approximately second
6, then resumes. This deliberately blocks downstream output without a network.
The only argument varied is the real PCM input's `-re`. Each variant ran twice,
with the order reversed on the second comparison.

With audio `-re`, video writes remained blocked for approximately another
21.7 seconds after the downstream reader resumed. Thirty seconds of source
material took 39.87 seconds to finish. Last video PTS was 39.78–39.79 seconds,
against audio PTS 30.016 seconds. Without `-re`, the additional video write
block was 0.175 seconds, the run finished in 29.99 seconds, and last video PTS
was 29.967–29.973 seconds. All four outputs retained 1,800 video packets and
1,408 AAC packets, with nondecreasing DTS in each stream.

The fixture demonstrates that the second pacing clock can prolong recovery
and distort arrival-derived video timing. It does not establish the cause of
the original external network slowdown. Its producers catch up after blocked
writes; the native app's bounded queue/drop policy is not simulated. Burst
arrival timestamps also produce repeated DTS after rescaling to the null
decoder output's frame timebase, so this is not proof of smooth per-frame
delivery. A codec-only decode with regenerated frame timestamps is clean.

The candidate removes `-re` only for real PCM in the GPU bitstream branch.
Synthetic unlimited `anullsrc` remains paced. Bitrate, resolution, frame rate,
input queue sizes and backpressure hysteresis are unchanged. Release build and
14 FFmpeg argument tests pass, including live PCM versus synthetic silence for
H.264, HEVC and AV1. Candidate native SHA256:
`07FE60895D28BDFA92348DE1FBD7CC5C3AFF595BCA5441F7E549474580E48411`.

A guarded 30-minute real-meeting comparison started around 16:18 UTC with eight
Tiles feeds and saved operator stream settings of 10 Mbps / 1080p60. Results
are pending. The runner stops on first backpressure, missing expected feed,
three degraded connectivity probes or unhealthy independent watchdog, and
stops streaming in `finally`. Raw local comparison evidence and the live run
remain under ignored `artifacts/live-601`; no meeting or destination secrets
are included here. Neither the PC-wide outage nor production acceptance is
declared resolved.

### Guarded candidate run interrupted by harness fault

The first audio-pacing candidate run ended at 16:20:35 UTC after approximately
169 seconds. The runner read an empty watchdog status file during its
truncate/write publication and failed closed on a null process ID. The
independent watchdog remained alive and healthy; streaming was verified off.
This is an invalidated soak, not a detected media/network failure or a pass.

Its 178 saved samples retained all eight Tiles feeds with zero backpressure,
encoder shedding, recorded audio loss or sender restarts. One render deadline
miss occurred, with zero skipped render slots. Adapter mean upload was
11.097 Mbps at 1,825 packets/s. Final FFmpeg progress was 10,141 frames,
169.23 seconds of media and 60 fps average. Logs and summary are preserved in
`artifacts/live-601/audio-pacing-candidate-30min`.

The local runner now retries incomplete status reads three times, 50 ms apart;
stale, expired, latched or dead guards still stop the test. Valid acceptance,
empty/latched rejection and 100 live status reads were checked. A new bounded
30-minute run started around 16:23 UTC with the same native executable and
settings, under `artifacts/live-601/audio-pacing-candidate-repeat30min`.

### Corrected candidate still encounters the initial stall

The repeat stopped on its first backpressure event at 16:29:43 UTC, approximately
seven minutes after starting. Streaming was verified off and the watchdog was
healthy. All eight expected Tiles feeds remained present in all 449 samples.
There were 26 encoder frames shed through the final saved sample, one render
deadline miss, zero skipped slots and zero recorded audio loss. Backpressure
entered with 743 ms buffered; no queued chunks were discarded before stopping.

Pre-stop successful video pipe writes took 193, 450, 169, 691 and 613 ms near
onset; no audio slow-write or read-rate catch-up message was recorded. Thus
removing real-audio pacing does not prevent the initiating stall. Its controlled
local recovery benefit must not be presented as an explanation of that trigger.

Upload declined from 11.59 Mbps at 16:29:35 to 7.81 Mbps at 16:29:42, while
internet probe latency rose into the 41–73 ms range, with one timeout at :39.
The gateway remained responsive. Host retransmissions increased by only 16
between :35 and :43, versus 63 around the previous instrumented failure; these
are not RTMP-socket counters. Run means were 11.024 Mbps and 1,841 packets/s,
with 19 internet probe timeouts overall and no sustained guard trigger.

Final FFmpeg progress was 25,449 frames / 424.85 seconds of media / 424.56
seconds elapsed, about 60 fps average, which does not negate frame shedding.
Two non-monotonic DTS warnings at approximately media second 191 corrected
191680 to 191700, well before final onset. Arrival-derived H.264 timestamps
remain an independent continuity concern. Native, performance and FFmpeg logs,
network samples, snapshots and summary are preserved in the repeat directory.
No further external stream was automatically started after this failure.

### Short-pause local comparison

An 850 ms downstream pause was absorbed by fixture buffering in both variants:
neither recorded a pipe write above 50 ms, and both retained all packets and
normal endpoint timestamps. It does not reproduce the observed input stall.

A 1.8-second downstream pause did produce an initial video pipe block of
834 ms with audio `-re` and 859 ms without it. With `-re`, there were 49 video
writes above 50 ms and the last such write completed 9.115 seconds after the
downstream reader resumed. Without it, only the initial write crossed 50 ms;
it completed approximately 1 ms after reader resume. Neither variant logged
an audio write above 50 ms. Consequently, absence of an audio slow-write log
does not exclude the extra pacing clock's effect on video recovery.

Both retained 1,800 video / 1,408 AAC packets, nondecreasing DTS and approximately
30-second completion; the old variant caught up before the fixture ended.
These single short-pause comparisons supplement the repeated long-pause test;
they do not establish the live stall's initiating cause. Evidence is in
`local-pacing-short-summary.json` and `local-pacing-short1800-summary.json`
under the ignored artifacts directory.

A separate 30-second passive baseline with streaming off and the meeting still
joined averaged 0.078 Mbps upload, with one internet probe timeout, up to 61 ms
probe latency and 51 host TCP retransmissions. Occasional host-level probe/loss
signals therefore also occur without the stream and cannot identify its socket.
The initial code commit's macOS stub job failed a background-media reopen test;
the latest document-only commit's same job passed. No unrelated code was changed;
the overall CI workflow is still pending.

### Local RTMP receiver comparison

The same paced H.264/PCM fixture was sent through FFmpeg RTMP over loopback
with TCP coalescing enabled and real-audio `-re` removed. A second FFmpeg
process listened on loopback and copied received packets to FLV. The baseline
and a variant pausing the receiver's output drain for 1.8 seconds both completed
in 30 seconds with 1,800 video / 1,408 AAC packets, nondecreasing DTS and zero
process errors. Baseline had no input writes above 50 ms. The paused receiver
caused one 711 ms video pipe write, completing approximately 2.5 ms after drain
resumed, with no subsequent slow input writes or slow audio writes.

This confirms that downstream receiver blocking can propagate to the video
pipe through the actual RTMP path, and that the corrected fixture recovers
promptly. It does not prove the external receiver blocked, or reproduce the
full app's encoder queue policy. Receiver mux/socket buffers are additional
to the direct-pipe fixture, so pause durations are not interchangeable. No
external destination or meeting configuration was changed. Both owned local
processes exited, and no loopback listener remains. Evidence:
`artifacts/live-601/local-rtmp-paced.py` and `local-rtmp-paced-summary.json`.

### Ten-minute loopback baseline

The paced local RTMP fixture completed 600 seconds at 16:56:40 UTC with no
input write reaching the 50 ms logging threshold. Sender and receiver exited
zero and producer threads finished. The receiver retained all 36,000 video
packets and 28,126 AAC packets; video last PTS was 599.975 seconds and audio
600.000. Neither stream had backward DTS, but video had three equal DTS pairs
and a maximum 47 ms interval. Thus packet retention is established, not perfect
per-frame timing. Receiver stderr reported I/O termination at sender disconnect
and inability to update a non-seekable FLV header; packet counts were complete.

This baseline uses repeated encoded fixture access units, not the native live
encoder, queue/drop policy or full app. It narrows the remaining investigation
but cannot attribute the external stall to the network or receiver. A full-app
test against a local receiver would distinguish those remaining paths more
directly. External streaming remained off; all owned fixture processes ended.
Evidence: `loopback-rtmp-10min-summary.json`, FLV and logs under local artifacts.

Latest fa5a2619 CodeQL passed; its macOS stub test again failed the background
media reopen assertion at RenderedSceneAttributionTest.cpp:603, while that job
passed at 132f373e with the same relevant code. Windows CI was still pending.
No unrelated test was weakened or production behavior changed for this failure.

### Source/render progress through captured output onset

The final ten saved snapshots of the corrected candidate span served times
16:29:34.322–16:29:43.098 UTC. Every expected Zoom source advanced its ingest
counter by 269–399 frames, with zero increases in source dropped-frame counters.
The three sources with advancing audio counters each added 432,000 samples;
the other five audio counters did not advance, so this is not a claim of audio
delivery from all eight sources. Render completed-slot counters added 540 with
no new deadline miss, while audio worker ticks added 450. Snapshot publication
and caching mean these are counter deltas, not exact wall-clock delivery rates.

These observations argue against stopped Zoom ingestion or a whole-render-loop
stall as the cause of this captured output episode. They do not distinguish
the encoder/FFmpeg path from downstream transport, nor explain the earlier
PC-wide outage. Sender `framesSent` increments on local input acceptance and
must not be interpreted as receiver-delivered frames. The local evidence file
`audio-pacing-candidate-repeat30min/onset-source-progress.json` retains details.

### Fixture burst-size limitation

Inspection of the 1,800 access units in `pacing-fixture-packets.json` found a
20,861-byte mean and 45,102-byte maximum. In contrast, slow live pipe writes
included complete encoded chunks around 335,000 bytes. Matching average bitrate
and input cadence therefore did not match the live stream's packet-size bursts.
The ten-minute fixture's clean result does not rule out burst-sensitive pipe,
FFmpeg or downstream behavior under the app's actual encoded output. Large
chunks alone do not prove link saturation or identify the original outage cause;
they define a missing dimension in that comparison. No new stream was started.

### Full staged app to local RTMP receiver, September 24, 20:37–20:47 UTC

Following renewed owner authorization, the full app sent eight live Tiles feeds
to a loopback RTMP receiver for ten minutes at 10 Mbps, 1920×1080/60. The guarded
runner stopped normally and restored the original output preferences verbatim;
streaming and engine were verified off afterward. Evidence remains locally in
`artifacts/live-601/full-app-local-10min`.

Across 638 snapshots, all eight expected Tiles remained present, every source's
video and audio counters advanced, and there were zero backpressure entries,
encoder shed frames, new render deadline misses, skipped slots or lost audio
samples. No session-scoped slow-pipe log occurred. The native sender supervisor
reported zero restarts and remained healthy. The shared app log nevertheless
contains output-supervisor restart/give-up messages during this same session;
their relationship to the healthy sender telemetry needs reconciliation before
calling the entire supervision path clean. Follow-up code inspection reconciles
this with existing issue #602: `CompositeOutputSender::sync` fans the complete
destination list to every protocol supervisor, and `noteDesired` creates records
for every name. The SRT and NDI supervisors therefore also supervise `rtmp` even
though their adapters ignore RTMP recovery/interrupt requests. The two paired
warning sequences are consistent with these non-owning supervisors; the actual
RTMP sender remains generation 1 with zero restarts and continuous recorded
output. This is misleading supervision logging, not evidence of an RTMP process
restart or the initiating network stall. No duplicate issue or unrelated fix was
introduced; #602 already defines the ownership fix and regression coverage.

The receiver exited successfully and retained 36,032 video packets and 28,149 AAC
packets, spanning approximately 600.5 seconds. Video packets averaged 20,827 bytes,
peaked at 342,903 bytes, and included 601 packets over 100 KB. This closes the
earlier fixture's small-packet limitation: actual native bursts of the observed
live size traversed the local path without captured backpressure. It does not
establish that an external server or network caused the earlier stall.

Five equal video DTS values and a maximum 34 ms video DTS gap remain; no backward
DTS occurred. Thus 60 fps average and clean pressure counters are not proof of
perfect per-frame delivery. Source dimensions also varied among 320×180,
640×360 and 1120×630, versus 1920×1080 sources in the earlier failing external
run. Output resolution/rate were unchanged, but this is not a matched source
workload. Whole-adapter upload averaged 0.254 Mbps with RTMP confined to loopback;
15 isolated internet probe failures still occurred. Original PC-wide outage
causality remains unresolved.

Packet-neighborhood analysis of this recording localizes the five equal DTS
pairs: two occur during startup (21 and 54 ms), and three at 282.055, 407.054
and 570.055 seconds. Each later pair follows a 33–34 ms interval and consists
of small non-key packets (roughly 1–8 KB), not the large keyframe bursts. All
601 packets above 100 KB are keyframes, with exactly 60 video packets between
successive keyframes. The equal timestamps therefore also occur without the
captured output-backpressure symptom or a coincident large packet. This is
consistent with the known arrival-clock timestamp weakness but does not locate
whether encoder scheduling, pipe delivery or FFmpeg parsing created each pair;
the recording does not retain original encoder PTS. Local details are saved in
`full-app-local-10min/packet-timing-detail.json`. Do not infer a network outage
cause from these DTS pairs.

### Instrumented full-app local comparison, September 24, 21:16–21:26 UTC

Native commit `8303a640` retains the previous 16 encoded packets on the writer
thread and emits their encoder PTS/DTS, keyframe flag, size, queue age and write
timing only when a pipe write takes at least 100 ms (rate limited to once per
second). Normal frames produce no trace output and no media bytes are retained.
The separate Release native build succeeded, as did 14 FFmpeg argument, 9 queue
overflow and 26 backpressure-policy tests. Its staged SHA256 was
`50139949546779EB98FD5EDE4E8E99C07F1E73E9A8657F438BF8B7981DAFF93D`.

The guarded ten-minute eight-Tiles run to the local RTMP receiver completed
without a slow write, so the new history was not triggered. All expected feeds
remained present through 639 snapshots; each source advanced video and audio
ingest with no source drops. Sender restarts, backpressure entries, encoder
shedding, skipped render slots and recorded audio loss were zero. There were
**two new render deadline misses**, which fail strict frame-delivery acceptance.
The receiver exited successfully with 36,070 video and 28,179 AAC packets over
approximately 601 seconds. Video peaked at 340,004 bytes per packet, with 602
packets above 100 KB, one equal DTS and no backward DTS.

Incoming source dimensions varied among 320×180, 640×360, 1280×720 and
1920×1080 during the run. All eight were simultaneously 1920×1080 in 524 of
639 snapshots (82%), so most of this local comparison did match the earlier
failing external source geometry. Whole
adapter upload averaged 0.205 Mbps while sending to loopback, with 14 isolated
internet ICMP timeouts. The local test cannot establish what initiates the
external transport stall or the original PC-wide outage. Its owned independent
guard was retired; original output preferences were verified byte-for-byte
restored, and streaming and the engine were off after the run. Raw evidence
remains under `artifacts/live-601/full-app-local-trace-verified10min`.
