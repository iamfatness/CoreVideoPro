# CoreVideo Pro - Alpha Plan

_Status: rewritten 2026-07-12 against main after PR #277. The previous plan (late June)
predated the July arc: the audio war, the virtual camera epic, screen/window capture,
the console UI, mastering, and the operator-perf root cause. Most of its Track B-E
content is now shipped product; what remains for alpha is **verification and
stability**, not feature building. Beta scope lives in `docs/beta-plan.md`._

_Addendum 2026-07-18: gates re-scored after PRs #280 and #286–#291 and the owner's
30-minute large soak (completed 2026-07-18). Closed since the rewrite: G1's
default-ON flip (#280, `NativeUvcCapturePolicy` is DEFAULT-ON, opt out with
`COREVIDEO_NATIVE_UVC=0`), the G2 zero-audio-recording root cause (#286 + headless
proof), the G3 30-minute soak (owner run 2026-07-18), the G3 sustained-RTMP leg
(#288), and G4's stale-OAuth-token hard-fail (#290 — `ZoomOAuthService` now
refreshes via the broker `/oauth/refresh`). Alpha is now down to a short list of
owed rig verifications; see the scoreboard in `docs/beta-plan.md` §2._

## 1. What alpha means

**Alpha = the owner runs real production shows on this rig, repeatedly, without the
app embarrassing itself.** One operator, one machine, real Zoom meetings, real
cameras, real audience-facing outputs (record + RTMP + virtual camera). Distribution,
onboarding, licensing, and other-machine support are explicitly **beta** concerns.

## 2. Current baseline (what is real and verified)

| Area | State |
|---|---|
| Zoom ingest (video + audio) | Real and stable: ISO video via SHM + zoom-mix/ISO audio via 128-slot rings; join/rejoin/churn proven in owner rig sessions |
| Compositor / multiview | GPU core-composited single-texture multiview, 4K canvas, 60fps; scenes with layers/opacity/z-order/draft-editing |
| Audio engine | Owner ear-verified + machine-soak-verified clean (clicks:0). Pull-model monitor, stateful DSP chain, 8-band EQ, comp/gate with GR metering, mastering M1+M2 rack on master bus |
| Audio UI | LV1-style console (SHOW) + three-column SETUP, insert rack + always-visible workspace with real response curves, pop-out hosts the same surface |
| Capture sources | Native MF UVC capture end-to-end (opt-in, validated on this rig 2026-07-10); WGC screen + window capture owner-verified; managed bridge fallback hardened (buffer ring, backoff policy) |
| Virtual camera | End-to-end in Zoom: core -> file-backed cross-session SHM -> COM DLL -> Frame Server; 1080p publishing ~50fps, GPU NV12, mirror + custom name |
| Overlays | Lower thirds, captions, wall clock at 60fps (DirectWrite/WIC raster) |
| Recording / RTMP | MF mux + RTMP with real program audio on the shared-epoch PTS clock. 2026-07-13: the zero-audio-recording bug (exit bar #5 blocker) root-caused and fixed — Mp4Writer reuse across the start-program-output/start-recording-session double-start left the gen-2 sink writer without an AAC stream (every audio WriteSample failed 0xC00D36B3, silently). Headless proof with real tone audio: `node scripts/validate-record-audio.mjs` |
| Stability tooling | Release PDBs, WER full dumps, crash-dump setup script, control API :8011, fake-engine soak harness, audio tap/scan toolkit, PresentMon/dotnet-trace recipes |
| Perf | Operator stutter root-caused (managed capture bridge); native UVC eliminates it (267MB flat vs multi-GB churn) |

## 3. Alpha gates (in order)

### G0 - System-audio citizenship (IN FLIGHT)
With the CVP virtual camera consumed by Zoom, other apps' audio must stay clean.
Fixes shipped (pacer timer, tap thread priority, MMCSS, GPU NV12 tap, reader
hardening); rig re-test staged 2026-07-12.
- [ ] Owner repro: CVP camera live in Zoom + browser audio -> no glitching
- [ ] If still glitching: LatencyMon during a glitch to name the driver
- [ ] Zoom self-view color/mirror unchanged (GPU BT.601 path is new)

### G1 - Native UVC default-ON
Currently opt-in via `COREVIDEO_NATIVE_UVC=1` (persisted on this rig). It is strictly
better than the bridge (CPU, memory, stability) and validated end-to-end here.
- [ ] Owner visual confirm on all his real cameras (tiles, multiview, program)
- [x] Flip `NativeUvcCapturePolicy.IsEnabled` to default-ON (DONE 2026-07-12, #280 —
      default-ON, opt out with `COREVIDEO_NATIVE_UVC=0`; bridge remains automatic
      per-device fallback)
- [ ] Re-run PresentMon under real show load to confirm the 400-875ms UI freezes are
      gone with the UI thread unstarved (closes the P4 diff-update question - only
      build P4 if stutter survives this measurement)

### G2 - A/V sync proof (north star: sync is paramount)

_2026-07-13 progress: the zero-audio-recording bug is fixed (stale `Mp4Writer`
state across the encoder double-start; every audio WriteSample failed
0xC00D36B3 silently) — this is almost certainly the same failure the 2026-07-02
"packaged run video-only MP4" evidence saw (any second `start()` on one sink
lifetime triggered it). Headless container proof on this rig (fake tone engine,
60s 1080p60 recording): video h264 + audio aac both present, |start delta|
1.8ms (< 50ms), |duration delta| 123ms (< 200ms). The same run also fixed the
audio worker pacer (bounded catch-up instead of re-anchor: a blown 20ms
deadline used to permanently shed 20ms of real-time audio at the feed FIFO —
measured 3.1% audio-shorter-than-video drift before, 50.0 ticks/s and 0 sheds
after). Repeatable proof: `node scripts/validate-record-audio.mjs`._

- [ ] Clap test on a real recording: measure audio-video offset at head and tail
- [ ] 5-minute recording head/tail sync check (owed since #163) — headless 60s
      container check now green (above); the owed rig check is the real-meeting run
- [ ] Verify recording from a **packaged** run has an audio track (2026-07-02 alpha
      evidence found video-only MP4 in packaged runs; the likely root cause is fixed
      2026-07-13 - verify on a packaged run, don't assume)

### G3 - The show drill (one structured rehearsal, owner + assistant)
A single end-to-end rehearsal that doubles as the backlog of owed rig verifications:
- [ ] Join real meeting -> assign sources (Zoom, UVC, screen, media) -> roles -> scenes
- [ ] Console walkthrough: fader/pan/mute/solo live, insert rack edits audible
      (gate-threshold drag while someone talks), meters + LUFS moving
- [ ] Mastering: enabled on master, target holds, program L/R inherit
- [ ] Record 1080p MP4 + one RTMP push + virtual camera in Zoom **simultaneously**
      (sustained RTMP itself was broken until #288 — fixed 2026-07-15 with platform
      profiles + hardened output; confirm the three-output combo specifically)
- [x] 30+ minute soak in that state: no audio artifacts, no UI degradation, working
      sets flat, frame drops 0 (owner large soak completed 2026-07-18; capture the
      run's evidence — working-set curve, drop counters, any warnings — in an
      alpha-evidence note so it's citable)
- [ ] Engine off / leave meeting / rejoin mid-show behaves (no deadlock, no orphan state)
- [ ] Support bundle exports after the run

### G4 - Stability debt (fix or explicitly accept before alpha)
- [x] **Engine-off teardown audit** against the five ZoomISO deadlock rules (stop off
      the UI thread, stop-then-destroy order, no locks across SDK calls, watchdog on
      async stop confirm, no STA marshaling) - this is the reference product's
      production failure; same architecture, same risk. DONE 2026-07-18 (PR #302):
      five confirmed findings fixed - engine exit now stops raw video + drains the
      pump before CleanUPSDK, ParticipantSubscription teardown serializes with the
      raw-frame callback (stopping flag + drain), EngineVideo maps got a mutex with
      SDK calls kept outside it, leave-meeting bridge Stop moved off the UI thread,
      and vcam stop() no longer deletes the SHM slot file. Owner rig verification
      (leave/rejoin cycles, exit-in-meeting, vcam stop/restart) listed in the PR
- [x] **Stale Zoom OAuth token** hard-fails join with no fallback (known since 07-02) -
      refresh/re-auth path shipped 2026-07-15 (#290): `ZoomOAuthService` validates the
      access token and refreshes via the broker `/oauth/refresh` before join; verify
      once on the rig with a deliberately expired token
- [ ] 0xc000027b window-resize trigger: mitigation is by design but not soak-verified -
      resize soak while under load
- [ ] One elevated run of `scripts/setup-crash-dumps.ps1` on the rig (if not yet done)

### G5 - Packaging-lite
Alpha ships to the owner's machine(s), not the public. Unsigned MSIX / `pack:native`
is acceptable.
- [ ] Packaged build launches on a clean profile with Zoom runtime discovery, core
      launch, recording folder access, vcam registration
- [ ] Alpha release note: commit, preflight report, known gaps

### Stretch (does not gate alpha)
- Virtual camera true 60fps (GPU NV12 readback in `exportVcamSharedTexture`; ~50fps
  today is competitive for a webcam consumer)
- Preview-screen direct editing round 2 (grips on the live preview picture - POS-2)
- Docs hygiene: spec status tables lag shipped reality (VST P1-P2b, screens, M2, B5
  are merged but marked pending in places); refresh during alpha close-out

## 4. Explicitly post-alpha (see beta-plan.md)

Production signing + installer + auto-update; onboarding/first-run; licensing and
access control; crash-report pipeline; hardware compatibility matrix; VST P2c real
plugin processing; browser sources; NDI hardening / SRT / DeckLink/AJA; V5 virtual
mic; per-source sync offsets; mastering reference presets.
