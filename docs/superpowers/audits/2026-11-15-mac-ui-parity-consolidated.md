# macOS ↔ Windows parity — consolidated gap list (2026-11-15)

Source audits (row-level evidence, file:line on both sides, every Mac claim verified in code):
- `2026-11-15-mac-ui-parity-sources.md` — 82 rows: 15 same · 18 partial · 8 different · 41 missing
- `2026-11-15-mac-ui-parity-routing-overlays-media.md` — 72 rows: 12 same · 11 partial · 9 different · 40 missing
- `2026-11-15-mac-ui-parity-audio.md` — 81 rows: 25 same · 18 partial · 7 different · 31 missing
- `2026-11-15-mac-ui-parity-settings-automation-health.md` — 99 rows: 33 same · 28 partial · 8 different · 25 missing · 5 n/a
- Earlier: `2026-11-14-mac-parity-{graphics,studio,social-comms,behaviour}.md` and the roadmap `../plans/2026-11-14-mac-parity-1.0-roadmap.md`.

**Totals: 334 operator-visible features compared; 85 same (25%), 75 partial, 32 different, 137 missing.**

Why: the Mac shell is a separate SwiftUI re-implementation; only the C++ media core is shared. The
Windows shell kept evolving after the August port and the 11-14 audits never compared screens
control-by-control. Most gaps are **shell-only** — the core already supports them on macOS.

## Tier 0 — breaks or silently degrades a live show (fix first)

| # | Gap | Area | Size | Lane | Status |
|---|---|---|---|---|---|
| 0.1 | Zoom audio never subscribed (no meeting-audio / participant-audio) → no channels, no monitor, silent outputs | Audio | M | shell | **in progress** (branch `mac-zoom-audio`) |
| 0.2 | Strips only from core echo (late guests + media silent by the fader law); no `"media"` strip | Audio | M | shell | in progress (same branch) |
| 0.3 | No default routing on a fresh console | Audio | S | shell | in progress |
| 0.4 | Audio state not re-sent on core restart; echoed mute overwrites the A1's mutes (#481) | Audio | S | shell | in progress |
| 0.5 | No pre-mute input meters (muted talker reads silent) | Audio | S | shell | in progress |
| 0.6 | Manual lower third likely never drawn (`mac:manual` source refused by the on-Program gate) while UI says "Key on air" | Overlays | S | shell | verify on screen, then fix |
| 0.7 | Production settings not persisted (canvas/fps, stream bitrate, recording format/prefix, multiview layout) | Settings | S | shell | |
| 0.8 | Slot assignments not persisted; ISO selection wiped on relaunch; ISO matrix drifts from the recording selection | Sources/Routing | M | shell | |
| 0.9 | Multiview solo Take follow-ups (rollback guard, solo captures source at cue) | Studio | S | shell | **in progress** (`mac-zoom-signin`) |
| 0.10 | Non-Studio tabs capped at 720 pt → Sources rows cramped/clipped | Layout | S | shell | |
| 0.11 | Recording folder fixed to ~/Movies/CoreVideoPro (no Browse / external drive) | Settings | S | shell | |
| 0.12 | Monitor output always the system default device | Audio | M | native+shell | |
| 0.13 | No "Join and end my other Zoom session" (Mac engine lacks `end_other_meeting`) | Zoom | M | native+shell | |
| 0.14 | Studio "Edit layout" does a different thing; scene edit only on the cued scene; no Add overlay / tile drag-swap | Studio | M | shell | |
| 0.15 | Zoom tab status ignores SDK readiness (bad creds only visible in Health) | Zoom | S | shell | |

Rough size: **~1.5–2 weeks** of shell work (one lane), with 0.12/0.13 needing a few days of native.

## Tier 1 — virtual camera (owner priority)

Roadmap M9.2 — **in progress**: Tasks 1–6 of 12 done and reviewed, Task 7 (e2e tools) running;
first owner milestone (Photo Booth + Zoom) after Task 7. ~1.5 weeks remaining.

## Tier 2 — large feature gaps (each its own plan)

| # | Gap | Size | Lane |
|---|---|---|---|
| 2.1 | Media clip playback (video/audio) — core decoder is Media Foundation only; needs an AVFoundation decoder | L | native |
| 2.2 | Graphics engine + Operate surface (On Air strip, Cue/Show/Hide playlist, F-keys, Shot Layouts, looks designer) — roadmap M3/M4 | XL | native+shell |
| 2.3 | CoreVideo Tiles + Edit Tiles (roadmap M5) | XL | native+shell |
| 2.4 | Sources rebuild: full-width table, inspector (display name, lower-third text, role, dropout), per-source signal/format/fps chips, missing source types (stills/media/screens properly labelled) | L | shell |
| 2.5 | Per-source colour grade (core `set-source-grade` works on Metal) + looks/LUT/curves; scopes/ROI/compare (native) | L / XL | shell / native |
| 2.6 | Per-source live thumbnails + tally on Sources | L | native |
| 2.7 | Remote control parity: ~100 actions vs 9 today, plus OSC (Companion/Stream Deck pages) — roadmap M2.3 | L | shell |
| 2.8 | Separate stream/record render sizes + codecs; recording bitrate/audio kbps | M | shell(+core) |
| 2.9 | Audio: Zoom audio mode toggle (per-guest ISO vs program mix); local mic/loopback source; mastering persisted; ISO 2–8 + aux buses; integrated LUFS/true-peak/stereo meters | M each | shell |
| 2.10 | Browser overlays (DSK) — needs a WKWebView browser host | L | native |
| 2.11 | SRT / RTMP / NDI ingest + SRT/HLS/NDI output on Mac | L | native |
| 2.12 | VST3 hosting on macOS | XL | native |
| 2.13 | Zoom quality caps (max res/fps), ±20 ms capture audio-sync nudge | S–M | shell |
| 2.14 | Comms/talkback (roadmap M7), YouTube/Twitch chat (M6) | L each | native+shell |

## Windows defects found along the way (separate issues)
- RTMP ingest sources never appear in the Windows source picker (`ShowInputRosterService.cs:208` vs `:290`).
- Windows Routing-page ISO cells don't change what records (only the Sources-tab toggle does).
- Windows master-rail monitor slider is 0–100 feeding a 0–1 value (unconfirmed at runtime).

## Mac-only features worth keeping
Per-slot chroma key; Keychain stream key; Zoom passcode field; live main multiview while popped
out; pop-out clock + native full screen; Health extras (binary paths, perf scan, Reveal, crash list);
monitor feedback-risk warning; hardened control API (no CORS, DNS-rebinding guard).
