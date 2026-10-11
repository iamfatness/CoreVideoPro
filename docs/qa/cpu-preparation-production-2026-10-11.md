# CPU preparation production integration (#802)

The owner authorized Windows production defaults and test-meeting qualification
on October 11. Normal module composition now enables the bounded preparation
owner and completed-image admission, with optional monitor/participant exports
on an isolated compositor. Explicit environment overrides remain available for
comparison; GPU capture ingress and the configured two-frame Program buffer are
unchanged.

The installed incident showed cold 1920×1080 background texture creation plus
BGRA upload work exceeding the 16.67 ms Program deadline. An expired-ready queue
head then amplified the original loss. The queue now retires that head before
waiting for the same following slot. It still records the original missed slot.

## Local validation

- Release native core and test executable built successfully.
- Eight hardware Program-buffer tests passed, including actual NV12 identities
  following an injected late preparation. Reverting only the queue repair makes
  that regression fail; restoring it passes.
- Ten I420/source-preparation tests passed, including normal factory defaults,
  actual pixels, faults, and lease behavior.
- Full native suite: 1,566 passed, two webcam-framer failures. Both failures also
  reproduced in the unchanged `idle-recording-state-859` baseline executable;
  they are not waived or counted as passing.
- Candidate native SHA-256:
  `f7ddf77cebe818973344d4f65b362bdafdad8adb20cc4632f755121b6b4a9399`.

## Live qualification

Reference machine: Windows 11 Home build 26200, Core i7-14700K, approximately
32 GiB RAM, NVIDIA RTX 4090 driver 32.0.16.1742. The test uses eight real SDK
sources, a newly opened 1080p media background, isolated Preview/multiview,
1920x1080 60/1 Program and webcam, and the unchanged two-frame Program buffer.
Recording, streaming, physical camera/screen reconnect and A/V sync are not
qualified by this workload.

`scripts/qa/media-background-admission-soak.mjs` uses the real SDK helper,
requires frames from every selected camera, repeatedly opens the supplied
1920×1080 background under new asset identities, and checks delivery counters
without changing buffer depth. An independent registered OS camera receiver
reads pixel identities; its verdict is separate from publication counters.
Credentials are supplied through the child environment and excluded from
committed evidence.

The earlier recording-privilege block cleared on requalification: all eight
requested real SDK camera feeds received raw video. The installed-layout
three-minute smoke test admitted all three newly opened 1920x1080 backgrounds
and delivered 10,786 Program frames without buffer underruns, overflows,
scheduled deadline misses, GPU-not-ready events or output sequence gaps. One
compositor render deadline miss was absorbed without Program delivery loss.
The registered OS camera receiver passed its independent pixel judge: 11,101
measured samples over 184.999830 seconds after the fixed 30-second warm-up,
zero repeated/missing/reordered/invalid identities, maximum interval 20.584 ms.

The subsequent run completed 1800.034 measured seconds, 3,510 sampled snapshots,
and all 20 cold background admissions. It fails zero-miss acceptance at two
boundaries. Before any Program loss, the OS receiver recorded
37 repeated and 37 missing identities after warm-up. Windows Frame Server
reported 41 held frames total: 40 contended shared-memory reads and one unchanged
read, including four warm-up holds. This independently locates the downstream
reader-contention boundary previously identified under #517. The matched
race/retry qualification in the render-delivery completion plan remains necessary.

Program then began losing delivery slots at measured second 1007.264, while the
SDK remained joined. The first logged misses had no queue head (`front=-1`),
followed by additional preparing/ready deadline cases. Frames arriving after
their slots expired also increased the overflow count. Source texture upload,
creation and scratch counters on Program remain zero: the enabled preparation
boundary is exercised, but that alone does not qualify the whole producer.
Sampled slow-tick windows show long ingest and monitor handoff intervals; these
are not per-frame causal traces. The remaining producer scheduling/ingress and
monitor handoff costs need attribution before a further repair. A passing short
trial does not erase the longer failure or establish an SDK crash.

Final qualification:

| Boundary | Measured result |
|---|---|
| SDK process/meeting | No crash or disconnect; eight feeds fresh at the end, all negotiated 1920x1080, measured incoming rates 29–43 fps. This does not certify loss-free SDK source ingestion. |
| Program, 30 minutes | 201 underruns, 132 overflows, 371 compositor render deadline misses. GPU-not-ready, scheduled delivery-thread deadline-miss and output-sequence-gap counters remained zero; those zeros do not override underruns. |
| Registered OS webcam | 242 repeated, 173 missing, zero torn/invalid or reordered identities across 108,302 measured samples in 1805.009485 seconds; maximum arrival interval 49.632 ms. Its window includes five seconds after the Program workload. |
| Publisher | 107,770 published frames, zero pending replacements or publication exceptions during the workload. Publication is not receiver continuity. |
| Preparation | All 20 backgrounds reached admitted completed GPU images; sampled Program source uploads/creations/scratch remained zero. |
| Monitor | 181 pending updates superseded after the first measured sample, no worker failure, 190,771,200 retained input bytes. Physical display presentation remains unverified. |
| Resources | Minimum sampled system commit headroom 80,820 MiB, limit 130,755 MiB. No recurrence of the earlier commit exhaustion. |

Sampled slow-tick maxima were 35.40 ms ingest, 18.96 ms monitor handoff, and
11.45 ms Program submission. Rate-limited buffer logs included empty, Preparing
and Ready heads at missed slots; no aggregate identifies the specific cause of
every miss. The end SDK snapshot also reports rejected shared-memory snapshots
in its `malformedFrameCount` fields; these are not proof of malformed Zoom
network packets. Their attribution remains open.

The next diagnostic run should enable the existing
`COREVIDEO_QA_ZOOM_HANDOFF_TIMING=1` scopes and a fresh bounded
`COREVIDEO_DELIVERY_TRACE_PATH`, retaining the same quality, workload and buffer.
Separate lock wait/body time, render wakeups, exact Program submission/readiness
and monitor handoff before repairing the demonstrated producer bottleneck. The
downstream reader race needs its independently controlled off/on qualification.
Do not treat either the short pass or passing CI as sustained delivery acceptance.

The native candidate was backed up and substituted into the existing installed
application directory to match registered camera ownership. The shell and
protected camera DLL remain unchanged; this is not a newly published beta.
Candidate artifacts launched outside that owner directory failed camera startup
and were rejected as combined-delivery evidence. The protected registered DLL
SHA-256 is `2406fe1603486cfa97d4481e3ff2ec85876f3b8a83b643149e65c297586ff08a`.
Service module enumeration did not return its loaded DLL path, so loaded-path
pinning remains unverified and must precede a reader replacement comparison.

The maintained harness now drains and judges receiver pixels before final
acceptance; receiver process exit zero alone cannot certify continuity.
Seven receiver-judge tests pass, including intentional repeats, gaps, torn
identities, wrong cadence and incomplete capture. Physical display presentation,
CPU ISO continuity, unsupported adapters, audio synchronization, and fleet
behavior remain separate qualification boundaries. PR #862 remains draft.

Private ignored logs and before/after test evidence are in
`artifacts/qa-802/` on the reference machine.
The completed per-boundary report is
`artifacts/qa-802/live-installed-soak/qualification.json`; failed raw receiver,
camera-reader and core logs are preserved alongside it.
