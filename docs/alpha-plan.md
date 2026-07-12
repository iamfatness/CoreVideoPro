# CoreVideo Pro - Alpha Plan

_Status: rewritten 2026-07-12 against main after PR #277. The previous plan (late June)
predated the July arc: the audio war, the virtual camera epic, screen/window capture,
the console UI, mastering, and the operator-perf root cause. Most of its Track B-E
content is now shipped product; what remains for alpha is **verification and
stability**, not feature building. Beta scope lives in `docs/beta-plan.md`._

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
| Recording / RTMP | MF mux + RTMP with real program audio on the shared-epoch PTS clock |
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
- [ ] Flip `NativeUvcCapturePolicy.IsEnabled` to default-ON (bridge remains automatic
      per-device fallback)
- [ ] Re-run PresentMon under real show load to confirm the 400-875ms UI freezes are
      gone with the UI thread unstarved (closes the P4 diff-update question - only
      build P4 if stutter survives this measurement)

### G2 - A/V sync proof (north star: sync is paramount)
- [ ] Clap test on a real recording: measure audio-video offset at head and tail
- [ ] 5-minute recording head/tail sync check (owed since #163)
- [ ] Verify recording from a **packaged** run has an audio track (2026-07-02 alpha
      evidence found video-only MP4 in packaged runs; the mux was rebuilt since - verify,
      don't assume)

### G3 - The show drill (one structured rehearsal, owner + assistant)
A single end-to-end rehearsal that doubles as the backlog of owed rig verifications:
- [ ] Join real meeting -> assign sources (Zoom, UVC, screen, media) -> roles -> scenes
- [ ] Console walkthrough: fader/pan/mute/solo live, insert rack edits audible
      (gate-threshold drag while someone talks), meters + LUFS moving
- [ ] Mastering: enabled on master, target holds, program L/R inherit
- [ ] Record 1080p MP4 + one RTMP push + virtual camera in Zoom **simultaneously**
- [ ] 30+ minute soak in that state: no audio artifacts, no UI degradation, working
      sets flat, frame drops 0
- [ ] Engine off / leave meeting / rejoin mid-show behaves (no deadlock, no orphan state)
- [ ] Support bundle exports after the run

### G4 - Stability debt (fix or explicitly accept before alpha)
- [ ] **Engine-off teardown audit** against the five ZoomISO deadlock rules (stop off
      the UI thread, stop-then-destroy order, no locks across SDK calls, watchdog on
      async stop confirm, no STA marshaling) - this is the reference product's
      production failure; same architecture, same risk
- [ ] **Stale Zoom OAuth token** hard-fails join with no fallback (known since 07-02) -
      add refresh/re-auth path
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
