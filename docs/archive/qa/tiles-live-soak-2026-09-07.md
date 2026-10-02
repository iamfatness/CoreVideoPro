# Live Tiles rehearsal results

The d97eeaa Windows candidate completed a 60-minute API rehearsal from
18:14:21 to 19:14:21 UTC with eight live Tiles video bindings and a 3-frame
Program buffer. All 60 alternating Tiles/speaker-slides selections and Takes
completed. Maximum HTTP duration was 214 ms. Program and Preview were restored
to speaker-slides, view to ProgramPreview, and auto-Take to true; the API
confirmed restoration. The earlier UI/API deadlock did not recur in this run.

This is **not release acceptance**. The operator reported flashing in both
Program and Multiview, absent from the Zoom client. Subsequent encoded-frame analysis below identifies brightness/contrast pulses. API success cannot override that visual failure.

Buffer deltas during the measured hour:

| Counter | Delta |
| --- | ---: |
| Underruns | 29,129 |
| Overflows, including expired packets | 16,764 |
| Delivery deadline misses | 1,364 |
| GPU-not-ready events | 0 |
| Output sequence gaps | 0 |
| Display-unconsumed events | 191,942 |
| Display-busy events | 84 |

Native frame-number progression averaged 56.56/s; actual buffer production and
delivery averaged approximately 52.77/s and 51.90/s. These are distinct internal
metrics, not measured display cadence. Delivered sequence IDs are assigned to
accepted deliveries, so zero sequence gaps does not negate missed time slots.
The hidden presentation path explains the separately retained unconsumed count.

Ten-minute frame-number rates fell from 59.88 to 58.94, 57.94, 55.28, 53.38 and
53.96/s. The admitted Tiles count remained eight. Retained stage logs identify
Program and Multiview as dominant CPU wall time; participant texture export
dominated most sampled slow Program calls. These timings include driver and
scheduler waits and do not prove a GPU or thermal cause. Log rollover limits
early-versus-late stage comparison. Further profiling should split participant
export and Multiview into mutex, upload, draw and copy stages.

OBS plugin history exposed a separate reachable Pro race: a copied frame could
publish after its same-UUID mapping was replaced or resized. The guard passed
a Release build and 34 focused runtime/shared-memory/IPC tests, including
deterministic replacement, retirement and resolution-change cases. Accepted
frames and rejected obsolete copies now have separate diagnostics. That race is not yet proven to
explain the operator's flashing. Relevant plugin commits: fccaaf9, 5b65dae and
7c0f1b5 in iamfatness/CoreVideo.

After the soak, a short Program/ISO recording was captured using the existing
recording configuration to inspect encoded pixels. Recording was stopped and
both buses restored. Its added encoder load excludes it from the soak's
performance measurement. Pixel analysis is reported below.

Private evidence: artifacts/tiles-live-soak-d97eeaa/soak/,
operator-visual-failure.json, post-soak/ and the test copy's Recordings directory.
The heartbeat is paused. The public release has not been replaced.

## Recorded flashing investigation

Original decoded Program frames (without FFmpeg frame duplication) confirm
brief brightness/contrast pulses inside individual Tiles. The same pixel detector
found 11 events over 40.133 seconds on d97eeaa and 16 over 60.633 seconds on
f848a28: 16.45 and 15.83 events/minute. The obsolete-frame publication guard
therefore did not materially reduce this recorded symptom.

For one low-motion pulse, stable-pixel affine fits had RGB slopes 0.879, 0.852,
and 0.862 and intercepts 15.92, 17.55, and 13.88. This supports a temporary
full-to-limited range compression, rather than a source identity swap. ISO
sampling was too sparse to establish which pipeline stage introduced it.

CoreVideo OBS commit 444cfea8da6d2aa78bdca7d4faba91b2c0d2db7e
(2026-08-22) documents the same SDK behavior: even with BT709_F requested,
individual callbacks can be limited range. Its fix checks IsLimitedI420 on each
frame and normalizes pixels before shared-memory publication. Pro was missing
this treatment. The port covers participant video and screen share, reuses
lock-protected scratch storage only for limited-range frames, and rejects
oversized frames before allocation. Full-range SDK buffers remain borrowed and
unmodified. Live validation below did not pass.

Optional COREVIDEO_D3D_STAGE_PROFILE=1 instrumentation splits participant export
and Multiview CPU wall time into stages for the separate cadence investigation.
These durations include driver/scheduler waits and do not measure GPU completion.

### Range-port live validation

Local diagnostic core 3371f00 (shell d97eeaa, range fix c9c34a7) joined the test
meeting. The running helper hash matched the built artifact. Camera correction
logs were frequent: about 100 limited frames per source in four seconds, unlike
the rare-frame pattern recorded in the original OBS investigation.

The post-port Program clip contains 8,386 original decoded frames over 142.233
seconds. The identical detector found 26 pulses (10.97/min), with maximum RGB
mean jump 13.18. The strongest stable-pixel fit remains compression-directed.
This is a failed visual acceptance result; the short samples do not establish a
statistically reliable improvement. No full-black/full-white tile failure was
found by this detector. Ten selection/Take cycles passed (20 selections, maximum
HTTP 81 ms), and both buses, recording state and auto-Take were restored.

The final Release build passed four range tests, four actual-GPU Tiles decoration
tests and two actual-GPU buffer tests. Independent review found and resolved an
oversized-frame allocation issue. Earlier shared-memory tests also passed.

The new diagnostic copy initially had ffmpeg.exe without its shared DLLs; analysis
used the previously verified d97 decoder. Automatic approval review rejected the
attempt to restart and install the complete pinned media runtime, without a
specific reason. No public package was changed. The local copy's media-runtime
setup remains incomplete; this is separate from the encoded pulse result.

Next evidence needed: correlate SDK flag/timestamp and pre/post-normalization
luma with a published frame and an encoded pulse. Audits found no callback bypass
or inverted flag interpretation, and Zoom remains I420 downstream. BGRA export
metrics include other input types. Avoid inferring pixel range solely from a
histogram or adding a heuristic color correction without this correlation.

Private evidence: artifacts/tiles-range-fix/ (build hashes, focused test logs,
selection results, original recording and visual-review). The stage timing report
in post-validation identifies participant texture upload/conversion/copy as the
next performance target; this recording does not establish 60 fps acceptance.

A bounded comparison of the matching tile-1 ISO also found a compression pulse:
ISO PTS 45.259500 versus neighbors 45.213867/45.282367 had stable-background RGB
slopes 0.869/0.850/0.865 and offsets 16.48/17.88/14.27. Exact same-frame alignment
to Program remains ambiguous. This proves the symptom is not exclusive to
Program composition, but does not distinguish shared ingest from encoder
conversion. The strongest Program tile-7 pulse could not be compared because
none of the sampled ISO images matched that source; one ISO was blank at the
sampled times. Keep this as a separate ISO-mapping investigation.
