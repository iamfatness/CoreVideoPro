# ISO recorder throughput — 2026-09-14

PR #531 includes the Program startup allocation changes from #528 and addresses
#529's shared serial ISO writer. It targets main; neither draft is merged.

Each Windows ISO file now owns an ordered audio/video worker, its Media Foundation
writer, and its Program-anchored capture clock. The producer captures missing
source timestamps before enqueue. Pre-epoch ISO audio is trimmed, and accepted
media drains before finalization. Source resolution, rate, and bitrate are unchanged.

The current candidate uses a bounded 64-picture / 96-audio-packet queue per file.
Codec startup may retain 96 pictures until the first real frame is written and
that backlog drains, after which the 64-picture limit applies.
The outer dispatcher retains at most 256 ISO pictures across sources while a
synchronous Program write stalls. At 1080p I420, video payloads at both stages'
combined steady limits are approximately 2.25 GiB for eight tracks (approximately
3 GiB at startup limits), excluding encoder and
other app memory. Normal occupancy is much lower. File buffering does not change
the three-frame Program delivery buffer. Sustained overload still drops and reports
media; these bounds do not establish unlimited encoder capacity.

Inner and outer losses are combined in recording proof, and completed finalization
cannot erase a degraded result. Per-file queue occupancy, drops, and writer timing
are exposed. A previous take's progress cannot end the next take's startup window.
The startup video counter remains visible; zero steady-state drops does not mean
that all pictures between pressing Record and the chosen mux epoch were retained.

GPU conformance now replaces the scalar CPU box filter for live ISO size changes.
The compute shader uses the same integer footprints, rounding, chroma placement,
and letterbox values. Pixel comparison against the CPU oracle passes for resizing,
letterboxing, tiny buffers, and buffer reuse. Bulk upload/readback stays on the
independent writer thread; its D3D11 context is separate from the compositor.
Hardware GPU initialization failure is reported, with no silent CPU resize fallback.

## Validation

Code candidate: `99e09c64` (GPU conformance, bounded codec startup, protected capacity
cache, idle recording-profile configuration, staggered teardown, and VFR durations).
All 1,010 Release native tests, 2,225 managed tests, and 19 hosted checks passed.
Regressions cover independent writers,
bounded overflow, accepted-tail drain, exception finalization, eight real MF files,
pre-epoch audio trim, retained loss, repeated recording startup, and eight ISO
sources surviving a blocked Program writer. A macOS test race was corrected to
accept requested or preparing before the first written frame, rather than assume
that the asynchronous open had not yet completed.

Host: Intel i7-14700K, 28 logical processors, NVIDIA RTX 4090, Windows. Live workload:
real Zoom test meeting (initially nine participants, later thirteen), eight selected
ISO sources, 1920x1080/60 Program,
8 Mbps H.264 Program recording, 6 Mbps GPU-direct RTMP, AAC 192 kbps / 48 kHz stereo,
three-frame Program buffer. The process-local encoder probe cap of seven reproduces
six hardware-placement ISOs plus two explicitly software ISOs. Placement labels are
not independent proof of which physical encoder backend each file used. The cap is
not saved in the package. No builds or file decoding run during controlled soaks.

Earlier candidates and failures are retained:

- `74efd444`: a 228.8-second run with seven hardware-placement and one software ISO
  ended with 83 video drops, six audio drops, and five Program deadline misses.
  A local build ran during the latter portion. This tested heavy-load configuration
  failed; successful finalization retained degraded health.
- `d179c187`, first run: 300.6-second recording, eight scene Takes, zero steady video
  or audio queue drops, zero Program-buffer underruns/deadline misses, 31 reported
  startup video drops. RTMP restarted once near the beginning, then recovered (#532).
  Aggregate per-track video-write wall time was 3.34 seconds per elapsed second,
  supporting independent writers rather than a single serialized worker; this is
  blocking wall time, not CPU utilization.
- `d179c187`, repeat: 180-second recording, four Takes, 85 outer video drops and zero
  inner track/audio drops. Of these, 73 appeared during startup with a stale prior-take
  progress flag; 12 more appeared later. Program underruns rose from 10 in the first
  sample to 12; this configuration failed delivery acceptance. RTMP remained on the
  established generation with no additional restart. The final candidate addresses
  the startup evidence race and outer burst capacity; it also retains a 32-picture
  per-file allowance through steady state.
- All 18 MP4s from the two d179 runs have readable H.264 video and AAC audio according
  to ffprobe. Container readability does not establish frame continuity or lip sync.

- `95e42750`: all 19 hosted checks and installer validation passed, but the five-minute
  live run failed with 14,080 ISO drops and one Program underrun. All eight ISO files
  were 1920x1080. The 120-second follow-up diagnostic run (same code plus timing logs)
  showed Media Foundation writes/file-length reads consuming only milliseconds while
  per-track frame work consumed tens of seconds after size changes. Six video MFTs
  identified themselves as NVIDIA. This points to scalar CPU conformance, not a need
  to remove Media Foundation throttling. No throttling setting was changed.

The first GPU candidate (49879c1a) retained zero ISO video/audio loss over 300.4s
with eight Takes (two 1080p and six 720p ISO file dimensions). The all-1080p repeat
ran 179.8s with four Takes: 23 startup ISO drops, zero additional steady ISO loss,
and no audio loss. All 18 files contain readable H.264/AAC streams. Program buffer
failures remained: the first final snapshot delta was 8 underruns / 4 deadline misses;
the repeat delta was 18 underruns / 0 deadline misses. Both sender restart deltas
were zero. These are failed overall delivery configurations, despite improved ISO
throughput. The current startup allowance addresses the measured 1.07-second codec
open; it does not relabel those 23 lost frames as success.

The c6956f77 startup candidate recorded eight 1080p ISOs for three minutes with
zero video/audio queue loss. Program counters stayed zero during recording but
rose by 15 underruns and three deadline misses during finalization. The next take
recorded Program only: a capacity probe launched at Configure had finished during
the first recording, replacing capacity seven with the four slots then available.
Admission subsequently refused all eight ISOs. This candidate failed live validation
despite passing installer validation and all 19 hosted checks; it was not published.

The capacity cache now suppresses probes while recording or GPU output encoding is
active and discards results that overlap any live encoding, even if encoding stops
before the probe returns. The shell sends the selected recording profile while idle
so the correct workload can warm before streaming. Cold/changed workloads still
report an explicit assumed capacity when a probe cannot safely run.

The 05c35f87 capacity checks completed two three-minute takes, each with four scene
Takes and six hardware / two software ISOs. The first had four 1080p and four 720p
ISO files; the repeat had eight 1080p files. All eighteen files contain H.264/AAC.
The capacity cache stayed valid and both recordings created every ISO. Queue losses
were zero in the first take and one ISO video frame in the second (no audio loss).
Program deltas were 11 underruns / two deadline misses and 12 / three respectively;
underruns clustered at Stop. RTMP restart deltas were zero. These runs failed
overall delivery acceptance and the second also failed ISO loss acceptance.

The next candidate raises the steady track allowance from 32 to 64 pictures for
the measured 600–780 ms encoder stalls. Stop spaces ISO close
requests by 100 ms on the file-control thread. Each writer releases its GPU
conformance context during that staggered shutdown, rather than releasing all
contexts in a final batch. All tracks are closed before any joins, so one hung
finalizer cannot stop the other tracks from finalizing. Program file finalization
follows the ISO close requests.

Independent packet inspection found an additional defect (#533): ISO video
timelines shortened by 1.1–3.2 seconds in the second 180-second take, despite MF
reporting all samples received and processed through the expected end. A focused
test reproduced ten seconds becoming 9.85 seconds. Nominal video sample durations
lose the remaining gap at fragment boundaries. The fix holds one ISO sample until
the next source timestamp establishes its actual duration, then flushes the last
sample at Stop. This neither duplicates pictures nor changes capture timestamps.
The regression now passes with software and hardware encoders and uneven frame
spacing. The saved failing file is `iso-vfr-regression-before.mp4` in the evidence
directory.

The final 99e09c64 checks each ran for three minutes with four scene Takes, eight
ISOs (six hardware placement, two software), and the same 1080p60 Program/stream
settings. The first recorded seven 1080p and one 720p ISO file; the repeat recorded
eight 1080p ISO files. Both had zero ISO video/audio queue loss, zero Program-buffer
underruns/deadline misses including Stop, and zero RTMP restart delta. All eighteen
H.264/AAC files finalized. Every file's video/audio endpoint difference was at most
21.1 ms. All video packet DTS values increase, and 36 head/tail decode checks pass.
The initial null-output decode command rounded VFR timestamps to its inferred frame
rate and warned about duplicate DTS; retaining the input time base resolves that
test-harness error without modifying the files. Its original output is retained.

The Program startup counter was three after the first run and stayed three through
the repeat. That remains visible and is not a claim of recording every frame from
the button press. These two finite checks do not replace a 30–60 minute live-show
soak, destination completion verification, or manual lip-sync/laptop testing.

The package is assembled from clean checkout 99e09c64. Both its native executable
and managed shell DLL match the SHA256 hashes of the live-tested diagnostic app.
The real isolated install/runtime probe/uninstall validation passed, including
payload integrity and user-file preservation. Evidence is in the packaging
checkout's `artifacts/installer-validation/077c1ad093f349ebb62dbe49a4836c5a`.

Published testing beta:
https://github.com/iamfatness/CoreVideoPro/releases/tag/beta-2026-09-14-99e09c64
All six uploaded asset sizes and SHA256 hashes match local files. The public tag
resolves to 99e09c64 and the installer download returns HTTP 200 with the expected
206,254,981-byte size. Installer SHA256:
`fba6c180eaa7b335c78d48e7a77076049a94eebccb1b9f51e090edafa25163f7`.

The actual packaged app also passed a 30-second default-settings recording with
no process-local probe cap (seven hardware-placement ISOs, one software). All nine
files finalized; ISO video/audio losses and Program-buffer underruns/deadline misses
were zero. The packaged app remains in the authorized test meeting with streaming
on, recording off, the wall on Program, and the Program/Preview view selected.
This additional smoke check does not extend the two three-minute tests above.

Destination
completion and display presentation are unverified. Laptop capacity and manual
lip-sync checks remain unverified. The earlier `beta-2026-09-14-85ecaca0` does not
contain this ISO implementation; use the new testing beta above.

Local evidence: `artifacts/live-2026-09-14/iso-*-live.jsonl`, finalized snapshots,
`iso-verified-summary.json`, `iso-verified-decode.json`, and
`iso-first-candidates-files.json`; full tests: `iso-timeline-full-tests.log`.
