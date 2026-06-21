# CoreVideo Pro - Alpha Build Plan

_Status: after pulling `origin/main` at `f10493aa` (`alpha #4: finish Scenes IA cleanup`). Owner: production._

This is the working plan for turning the current codebase into a usable Alpha
build. The latest `main` moved the project from "build the native foundations"
to "prove the dev-gated native paths on a Windows rig, tighten the operator
workflow, and package a build with evidence."

## 1. Current Baseline

CoreVideo Pro is now a native Windows production app with a typed WinUI/C# shell
to C++ media-core boundary:

```text
WinUI shell
  -> MediaCore bridge (C#, JSON-RPC)
  -> native media core (C++)
      -> Zoom SDK ingest path
      -> D3D11 compositor
      -> Media Foundation recorder
      -> audio routing / DSP / monitor taps
      -> RTMP / NDI / SRT output adapters
      -> diagnostics and support bundles
```

The repo has a strong stub-default contract surface and a much more complete
dev-gated native runtime than the previous Alpha plan assumed.

### What Landed On Latest `main`

| Area | Current state | Alpha meaning |
|---|---|---|
| Sources / Inputs / Routing / Scenes IA | Sources owns Input 1-10 mapping; Scenes audio routing cleanup landed | UX structure is no longer just a proposal; smoke it and polish states |
| F2 audio bus | Real PCM routing matrix, program/ISO taps, BS.1770 meter, bus inserts, monitor tap | Audio foundation is mostly built; remaining risk is device/runtime proof |
| F3 compositor | Framing, borders, overlay/caption raster math in D3D11 and CPU stub | Program/preview parity needs Windows visual proof |
| Recording | MF recording mux path for real program frame + audio + ISO work landed | Needs on-disk validation with real duration, A/V sync, and file bytes |
| RTMP | Real program audio over RTMP landed; codec compatibility matrix exists | Needs live push validation with FFmpeg runtime staged |
| NDI | Dev-gated NDI sender landed and reconciled with program-audio sync | Optional Alpha proof unless we include NDI in the Alpha promise |
| Automation | Local on-device director provider and native recommend-auto-production command landed | Keep as assisted Alpha feature, not a blocker for live media proof |
| Diagnostics | Support bundle builder/export and native crash event surface landed | Needs failed-run bundle validation |
| Packaging | Packaging and record/stream validation hardening landed | Needs clean-machine install/launch proof |

## 2. Alpha Exit Bar

Alpha is "working" when, on a Windows dev machine with the required SDK/runtime
dependencies staged, an operator can:

1. Join a real Zoom meeting and see participant feeds plus metadata.
2. Assign Zoom participants, media, and at least one local/video input to Inputs 1-10.
3. Route video and audio through the Sources, Routing, and Scenes workflow without
   duplicate route state or hidden per-layer audio state.
4. Use Magic Scene / Set & Forget to produce a stable show layout.
5. Record a playable 1080p MP4 with real program video and real program audio.
6. Stream one RTMP destination with real program audio in sync.
7. Export a support bundle after a normal run and after a simulated failure.
8. Install or launch the native app from the packaged Alpha artifact on a clean box.

NDI, SRT, DeckLink/AJA, VST hosting, and full AI/cloud direction remain
post-Alpha unless they are already staged on the validation machine and pass as
non-blocking proofs.

## 3. Replanned Workstreams

### Track A - Alpha Evidence And Build Hygiene

- [ ] Run `npm run alpha:preflight` on latest `main`.
- [ ] Attach the generated `artifacts/alpha-preflight/<timestamp>/alpha-preflight.md`
      to the Alpha decision.
- [ ] Fix any failed offline gates before spending time on live Zoom validation.
- [ ] Keep warnings for missing local SDK/runtime dependencies visible rather than
      hiding them.

### Track B - Live Zoom And Source Proof

- [ ] Stage Zoom SDK/runtime and build dev native artifacts.
- [ ] Join a real meeting, enable capture, and verify roster, active speaker,
      mute/unmute, screen share, leave/rejoin, and participant churn.
- [ ] Validate Inputs 1-10 persistence across restart and recent meeting restore.
- [ ] Validate at least one non-Zoom input path available on the machine
      (UVC/test-pattern/media is acceptable for Alpha if real UVC is not staged).

### Track C - Operator Workflow Polish

- [ ] Smoke Sources tab as the canonical Inputs 1-10 mapping.
- [ ] Smoke Routing video and audio matrix behavior, including ISO isolation and
      gain changes.
- [ ] Confirm Scenes no longer has conflicting audio/route controls.
- [ ] Tighten empty/loading/error states for first-frame wait, missing source,
      no route, encoder unavailable, and Zoom not joined.
- [ ] Verify the full 16:9 canvas remains visible at common laptop and desktop
      window sizes.

### Track D - Record And Stream Proof

- [ ] Run `npm run validate:record-stream -- --destinations recording --timeout-ms 30000`.
- [ ] Record a short manual MP4 and confirm playable file, non-zero bytes, real
      audio track, expected duration, and acceptable A/V sync.
- [ ] Record a longer 30-minute soak when the short pass is clean.
- [ ] Stage FFmpeg runtime and push one RTMP destination with real program audio.
- [ ] Treat NDI as optional Alpha evidence unless explicitly promoted into the
      release promise.

### Track E - Diagnostics And Recovery

- [ ] Export a support bundle after a successful run.
- [ ] Force or simulate native-core crash/output failure and verify crash event,
      recovery affordance, warning state, and support bundle contents.
- [ ] Confirm output/recording failures remain visible until the operator recovers
      them.
- [ ] Confirm secrets in destinations and meeting config are redacted.

### Track F - Packaging

- [ ] Run `npm run pack:native` and, if signing is available, `npm run pack:native:msix`.
- [ ] Launch the packaged build on a clean Windows machine or clean user profile.
- [ ] Verify Zoom runtime discovery, native-core process launch, recording folder
      access, and support bundle export paths.
- [ ] Produce a short Alpha release note with commit, preflight report, known
      warnings, and manual validation result.

## 4. Recommended Alpha Sequence

1. **Freeze the baseline:** stay on `main` at `f10493aa` or a named Alpha branch
   cut from it.
2. **Run offline gates:** `npm run alpha:preflight`; fix hard failures only.
3. **Stage runtime dependencies:** Zoom SDK/runtime, FFmpeg, dev native build,
   optional NDI SDK.
4. **Run live media proof:** Zoom join, source assignment, routing, scene output,
   record, stream.
5. **Run failure proof:** crash/recover/support bundle.
6. **Package and clean-launch:** create the Alpha artifact and validate startup on
   a clean machine/profile.
7. **Decide Alpha:** ship only if the exit bar passes with documented known gaps.

## 5. Known Risks

- Real Zoom raw-media behavior and entitlements can only be validated in live calls.
- Windows-only paths cover D3D11, Media Foundation, WASAPI, WinUI, FFmpeg process
  plumbing, and packaging; Linux/container CI is not enough.
- Real hardware capture beyond UVC/test-pattern is still vendor-SDK dependent and
  should not block Alpha unless promised.
- RTMP H.265/AV1 compatibility is ingest-dependent; Alpha should default to
  H.264/AAC unless enhanced RTMP is explicitly validated.
- The local director should be treated as an operator assist; live media reliability
  is the release gate.

## 6. Post-Alpha

- Real UVC native capture if Alpha uses only test-pattern or WinUI-local preview.
- DeckLink/AJA frame capture and embedded audio.
- SRT ingest/output with decode/encode.
- NDI hardening beyond the dev-gated sender.
- VST3 host and ASIO capture.
- Longer multi-hour soak runs and update/installer hardening.
