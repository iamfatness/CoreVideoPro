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
