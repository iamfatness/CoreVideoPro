# Architecture branch validation

Local validation on Windows, September 5–6, 2026. These are working-branch
results, not evidence for a signed release candidate.

| Check | Result |
| --- | --- |
| Native portable suite | 619 passed, including 18 asynchronous encoder lifecycle cases |
| WinUI suite | 750 passed; TRX retained under `artifacts/test-results/winui` |
| MediaCore suite | 487 passed, including real child-process handshake/restart cases |
| Control suite | 53 passed |
| Renderer unit / integration | 1,739 / 111 passed |
| Node protocol runtime | 88 passed |
| TypeScript show engine | 417 passed |
| Release evidence validator | 22 passed |
| Shared lifecycle fixtures | 56 wire cases in C++, C#, both TypeScript consumers and Swift |
| TypeScript checks / contract regeneration | Passed |
| Real Windows native Release build | Passed with D3D11, Media Foundation and Zoom SDK adapters |
| WinUI Release publish | Passed; existing analyzer warnings remain |
| Stub process bridge smoke | Passed with an explicitly selected stub binary |
| macOS shell CI | Build passed; 173 checks passed |
| macOS native CI | Stub and Metal/AVFoundation/CoreAudio/capture configurations passed their suites |

## Process responsiveness

The fake engine injects separate 30-second authentication and join stalls. Each
stage measures 40 Ping and Stop round trips and sends 128 MiB of input. Stop p95
was 14.58 ms during authentication and 17.33 ms during join; maximums were 19.75
and 18.29 ms. Peak private memory stayed below 4.3 MB in these runs. Cancellation
worked and an incompatible Leave request did not cancel the current operation.

This proves synthetic process responsiveness for the exercised workload. It does
not establish real Zoom render cadence, Take effect latency, or slow-client
outbound backpressure. Mailbox overload limits are covered separately by native
tests. The reproducible harness is [validate-command-responsiveness.mjs](../scripts/validate-command-responsiveness.mjs).

## Real recording

A generated 1920×1080 color-bar scene and silent stereo audio were recorded with
Media Foundation and D3D11. The harness observed the matching session become live,
accepted Stop, waited for completed/finalized, then required ffprobe metadata,
full ffmpeg decoding, and varied decoded image content. Both runs passed these
checks and produced H.264 video plus 48 kHz stereo AAC.

The isolated run delivered about 51.13 effective frames/second with 65 encoder
queue drops under a configured 60fps profile. An equivalent build of the original
`660f626` commit, with matching adapter flags/compiler/SDK and no source changes,
delivered 50.72 fps with 62 drops. The throughput ceiling exists in the baseline;
these short runs do not indicate a branch regression or establish statistical
equivalence. Successful finalization is not a 60fps acceptance result. A/V duration
agreement in silent generated content is not a lip-sync or intelligibility test.
See the [recording proof procedure](recording-finalization-validation.md).

## Remaining evidence

The macOS media drill still misses its 60fps recording requirement. Branch CI runs
reported 19.3 and 24.1 fps; historical main runs reported 29.4 and 25.4 fps. This
establishes an existing failing gate but shared-runner variation does not exclude
a regression statistically. The frame-rate requirement remains unchanged.

Real Zoom
multi-participant ingest, simultaneous recording/streaming, per-destination faults,
physical camera disconnection, disk exhaustion, the two-hour soak and clean-machine
installation remain unverified for this candidate. The release workflow requires
the controlled-rig evidence described in [release-evidence.md](release-evidence.md).

Full generated legacy-protocol coverage, durable participant identity/native Take,
and outbound backpressure remain implementation work in the
[remediation plan](architecture-remediation-plan.md). They are not hidden behind
the automated test totals.
