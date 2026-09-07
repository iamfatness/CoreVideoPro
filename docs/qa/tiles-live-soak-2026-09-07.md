# Live Tiles rehearsal results

The d97eeaa Windows candidate completed a 60-minute API rehearsal from
18:14:21 to 19:14:21 UTC with eight live Tiles video bindings and a 3-frame
Program buffer. All 60 alternating Tiles/speaker-slides selections and Takes
completed. Maximum HTTP duration was 214 ms. Program and Preview were restored
to speaker-slides, view to ProgramPreview, and auto-Take to true; the API
confirmed restoration. The earlier UI/API deadlock did not recur in this run.

This is **not release acceptance**. The operator reported flashing in both
Program and Multiview, absent from the Zoom client. The exact flash appearance
is not yet specified. API success cannot override that visual failure.

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
performance measurement. Pixel analysis is pending.

Private evidence: artifacts/tiles-live-soak-d97eeaa/soak/,
operator-visual-failure.json, post-soak/ and the test copy's Recordings directory.
The heartbeat is paused. The public release has not been replaced.
