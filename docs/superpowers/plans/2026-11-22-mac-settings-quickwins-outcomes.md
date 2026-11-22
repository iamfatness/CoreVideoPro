# Outcomes: macOS settings quick wins (A1, A5, Z1, Z2, O2, O1, C1)

**Plan:** `2026-11-22-mac-settings-quickwins.md`. **Branch:** `mac-settings-quickwins`, stacked on
`mac-control-audio-phase2` at `2d766924`. **Not pushed.**

## Commits

| Commit | Item | What |
|---|---|---|
| `c2834ee0` | plan | The plan |
| `a6051d2d` | prep | Prefs fields (all items) + `RecordingProfile` / `StreamEncoderSettings` values |
| `f6b0508e` | A1 | Zoom audio mode: per-guest ISO vs Zoom program mix, doubling guard, METER ONLY, `audio.zoomMode.set`, `zoomAudioMode` state |
| `14a825b1` | A5 | Local audio source card, `local-machine-audio` entry, strip and default sends, core-read status |
| `b0688706` | Z1 | Zoom camera max resolution / guest fps pickers, sent on `zoom-join`, latched by the join |
| `2d246c16` | Z2 | Capture audio-sync nudge, both core copies, session state |
| `95fb26a3` | O2 | AVF recording: scale not crop, HEVC (`hvc1`), AV1 refused, real container/codec reported |
| `3211ce5b` | O1 | Recording profile + stream audio/keyframe/rate control/H.264 profile/B-frames (recording fps picker later replaced by "follows the canvas", review #4) |
| `483c739a` | C1 | `automation.autoAssignInputs.set` + `autoAssignInputs` on Mac (Companion autoassign preset) |
| `24143fe7` | review | Z2 nudge on camera rows only, compact buttons; HEVC ISO path proven |

## Evidence

| Gate | Before | After |
|---|---|---|
| `COREVIDEO_SHELL_TESTS=1` | 5994 passed | **6133 passed, 0 failed** (19 new `quickwins/*` cases plus updated counts) |
| `COREVIDEO_SHELL_SELFCHECK=1` | PASS | PASS (round trip covers every new prefs field) |
| `AvfEncoderRecording.*` (Metal build) | 14 | **20 passed** |
| Full Metal ctest `-LE realtime` | — | passed at O2 (`corevideo-native-tests`, 224 s) |
| `npm run contract:check` | — | clean |
| Companion module vitest + lint (scratch copy) | 20 | **20 passed**, lint clean |
| Mutations (rule 7) | — | 23 named mutations, every one red; restored files re-verified green |

One mutation first **survived**: swapping the A5 status branches stayed green because the table
covered only 3 of the 4 `signalPresent`/`captureStreaming` combinations. The fourth case was added
(plan-authoring rule 3) and it now turns red.

Pixels reviewed via `COREVIDEO_SHELL_TAB_SNAPSHOT`: Audio SETUP (local audio card, mode toggle),
Zoom (quality block), Sources (capture rows), Settings (O1 controls). The review caught the nudge on
screen rows (fixed in `24143fe7`).

## Decisions taken (in the plan, restated)

- **D1** The Mac keeps its "operator removal of a default wins" rule; PerGuestIso applies the Windows
  doubling guard *after* edits; ProgramMix seeds `zoom-mix` only while the meeting mix is subscribed.
- **D2** The Z2 offset is session state (Windows), not persisted. The outputs spec said persisted and
  the audio spec said not; Windows decided it.
- **D3** The stream keeps canvas size and the destination's 30 fps, stated as text.
- **D4** Stream codec stays H.264. Recording offers H.264/H.265; AV1 is listed unavailable. No
  recording-quality picker.
- **D5** Hardware encoder shown as "VideoToolbox" text.
- **D6** `StreamEncoderSettings.apply(to:)` is the one merge point for the streaming lane.
- **D7** `viewMode` (STU) and `autoLowerThirds` (GFX) stay blocked.
- Recording defaults = Windows `DefaultRecordingOutputProfile` (1080p60, 8.2 Mbps, 192 kbps), which
  equals the core's defaults. The plan's first draft said "follows the canvas"; corrected in `3211ce5b`.

## Found along the way

- **Every Mac recording opened at 1920x1080@60** (the shell never sent a profile), and AVF cropped a
  larger Program top-left: on a 1440p or 4K canvas the file held the top-left corner. O2 fixes the
  crop; O1 lets the operator pick the size.
- `f6b0508e` (A1) left the Companion module's `mac.test.ts` pinned at 40 actions while the fixture
  served 41. Fixed in `483c739a`. Do not cherry-pick A1 alone.

## Unverified (owner checks)

1. **A1** In a real meeting with two guests, flip AUDIO > Per-guest ISO off: program carries Zoom's
   mix, guest strips read METER ONLY and their faders do not change program; flip back, no doubled voices.
2. **A5** SETUP > Local audio source: pick the Mac mic, Source on, speak: "Audio detected", the
   Local machine audio strip moves, and a recording carries it.
3. **Z1** Set Camera max 720p, join: Format shows ≤720p delivery (whether the Mac engine honours a
   sub-1080p subscription is unknown; if not, file an engine task).
4. **Z2** Clap test on a capture card with a paired mic: nudge until lip sync; kill the core and
   confirm the offset comes back.
5. **O1/O2** Record a 1440p canvas at 1080p H.265 MOV: it opens in QuickTime, the full frame is
   present (not a corner), `ffprobe` shows hevc 1920x1080. Stream with VBR / 4 s keyframes /
   main profile and confirm the receiver sees it.
6. **C1** Companion on a Stream Deck: the autoassign button toggles Sources > Fill free inputs and its
   feedback lights.

No test-launched app, no request to `127.0.0.1:8011`, and no Zoom meeting were used.

## Owner decisions needed

- **O-1** Allow a stream fps picker (60 fps streaming) before GPU-direct encode (O4) lands? Today the
  stream is pinned at 30 fps because 1080p60 BGRA would go through the CPU ffmpeg pipe.
- Confirm D2 (the nudge offset is not persisted, matching Windows), or rule that the Mac persists it.

## Merge notes

- Stacked on the frozen `mac-control-audio-phase2` (`2d766924`); merge after it.
- **Conflicts expected with `mac-streaming-phase2`:** `AppModel.streamDestinationSettings()`
  (that lane moves rows into `StreamDestinationPolicy.destinationSettings`; apply
  `streamEncoder.apply(to:)` to each row it builds), `StudioFooter.swift` recipe line,
  `SettingsPane.swift` Outputs card, `ShellPrefs.swift` (additive fields).
  `CoreLaunchEnvironment` is untouched.
- Native change is one file plus a CMake link line (`-framework Accelerate` on the AVF encoder).
- Build note (2026-11-22): the overnight Command Line Tools update made `MacOSX27.2.sdk` default and
  its linker cannot read it. This lane built native with Xcode 26.6's own toolchain and SDK:
  `DEVELOPER_DIR=/Applications/Xcode.app/Contents/Developer`, `CMAKE_{C,CXX,OBJCXX}_COMPILER=<Xcode
  toolchain>/clang(++)`, `CMAKE_OSX_SYSROOT=.../MacOSX26.5.sdk`. The coordinator's alternative
  (`DEVELOPER_DIR=/Library/Developer/CommandLineTools` for native) was not tried here.

## Review round (2026-11-22, independent review via the coordinator)

| # | Finding | Verdict | Fix | Proof |
|---|---|---|---|---|
| 1 | O2 tests appended after the AVF test file's `#endif`: Windows / stub / AVF-off builds fail to compile | **Real** | `ef5ef942`: `#endif` moved to EOF | Stub configure reproduced `unknown type name 'ProgramFrame'` before; after, the stub build links and its full suite passes |
| 2 | Saved local audio not applied on the first connect: `onConnected` reasserted the console before listing audio inputs; inputs never refreshed; `reassertCaptureAudio()` dead | **Real** | `887456f6`: inputs listed before the reassert; refreshed on AVCaptureDevice connect/disconnect with a re-push when the local source's sendability changes; dead function removed | `a5-first-connect-sends-the-saved-source` drives the real `onConnected` (red before); `a5-hot-plugged-mic-goes-live`; both mutations red |
| 3 | `session_.codec` hard-coded `h264`, so HEVC reports h264 | **Real** | `2de88dec`: the writer's codec once open | HEVC test red before, green after |
| 4 | Recording FPS picker changes nothing; 0.5-step bitrate truncated by the core | **Real, both platforms** | `8723a016`: fps follows the canvas (text, payload carries the canvas fps, so `recordingFps` is true); whole-Mbps bitrate, default 8 | Windows checked first: MF sets `MF_MT_FRAME_RATE` as metadata and stamps real time (no decimation); the Mac AVF writer never used fps. The shared int `targetBitrateMbps` (Interfaces.h) is left alone; fractional bitrate in the core is a follow-up for both shells |
| 5 | Z2 device copy re-sent at `onConnected` dropped if the AVF camera is not enumerated yet | **Real** | `8723a016`: reconcile on every snapshot against `captureDevices[].audioSyncOffsetMs`, once per device while in flight; the `onConnected` re-send removed | `AvfCaptureDeviceAdapter` fills its map on a background queue and `setAudioSyncOffset` ignores unknown ids; `z2-device-copy-reconciles-from-the-snapshot` (red without the reconcile) |
| 6 | `submit()` falls back to the 320x180 preview, now upscaled by O2 | **Real (reachable)** | `2de88dec`: a tick without full-resolution Program is skipped | The Metal tap publishes one frame late on its own thread: the first tick after arming always lacks it. `NeverRecordsThePreviewThumbnail` (replaces the test that pinned the fallback; red before). `JsonRpcServer.PreservesIdAndAcksStartProgramOutput` had been counting that thumbnail as its one encoded frame; proven by restoring only the fallback (green) — it now drives display + output ticks until a real frame is encoded (see the commit below) |
| low | Companion README still said Auto-assign does nothing; AV1 row selectable | Real | `8723a016` | — |

None of the medium items turned out not real.

**Gates after the round:** shell 6145 passed, 0 failed; self-check PASS; contract check clean;
AVF + program-tail 27/27; full Metal test binary **2107 passed, 0 failed**; full stub test binary
**2014 passed, 0 failed** (Metal suite run after the JsonRpcServer test change, stub after review #1).
