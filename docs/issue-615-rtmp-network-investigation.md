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

Repeat with a valid full test-meeting link and all eight live Zoom inputs; verify
audio and decoded receiver delivery. The original Battle.net/AVD disruption is
still an open acceptance item. RTMPS, other network adapters, other ISPs and
other receivers have not been measured. No fleet-wide or unlimited-duration
stability claim follows from these finite runs.
