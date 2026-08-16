# CoreVideo Pro ↔ CoreVideo OBS plugin — parity charter

Date: 2026-08-15
Status: active — this document fixes the bar and the inventory. Design specs hang off it.

## Why this exists

CoreVideo Pro cannot launch as a product that does *less* than the OBS plugin it
descends from. An operator running a show on the plugin today must not find a
workflow missing when they move to Pro. This charter states the bar, records a
verified inventory of where the two products actually differ, and groups the gaps
into units small enough to spec and implement.

It supersedes nothing. `obs-real-meeting-parity-audit-2026-08-13.md` audits
*behavioural lessons* learned from plugin releases v0.1.33–v0.1.39 — how the
product must behave in a real meeting. This document audits the *feature surface* —
what the product can do at all. Both gates apply.

## The bar (owner decision, 2026-08-15)

**Plugin parity is the floor, not the target.** Every documented plugin feature
must exist in Pro, and where Pro's architecture allows a better implementation, it
takes it. Pro is intended to be a strict superset, such that no operator has a
reason to prefer the plugin except the OBS ecosystem itself.

This is a deliberate hardening of the earlier framing in the real-meeting audit
("a focused Zoom production studio, not a broad OBS clone"). That sentence
described the *shape* of the product and remains true — Pro is not growing generic
OBS features. It does not license a smaller feature surface than the plugin.

## Method

The inventory below was derived by diffing the plugin's documented feature surface
(`docs/CORE_PLUGIN_FUNCTIONALITY.md` at `iamfatness/CoreVideo` `origin/main`,
release v0.1.39) against the CoreVideo Pro implementation at `ca8778d5`, reading
source rather than documentation on the Pro side. Every "gap" below carries the
evidence that established it. Where a claim rests on absence, the search that found
nothing is named, so it can be re-run.

## Inventory

### Pro is at parity or ahead

| Capability | Note |
|---|---|
| ISO recording | Pro is **ahead**: per-source A+V MP4s with pre-DSP raw stems, silence-fill alignment to a shared epoch, fragmented MP4 for crash recovery. The plugin writes MP4 video plus a separate PCM WAV and requires `ffmpeg` on `PATH`. |
| Per-participant audio | Pro is **ahead**: per-guest strips, ISO stems, and THE FADER LAW (no source reaches a bus without a strip). The plugin creates audio sources into an operator-managed scene or group and documents three failure modes for running two walls at once. |
| Active-speaker director | Timing model at parity (challenger stability, incumbent hold, candidate eligibility, roster-loss grace). Exclusions present (`ZoomActiveSpeakerDirector.h`). |
| Screen share | Route mode present and resolved in the core render plan. |
| Audio routing modes | Mixed / isolated / audience all present (`ZoomEngineClient`, `RoutingMatrixModels`, Audio page). |
| Control APIs | TCP and OSC control servers both present (`OscControlServer`, `OscAddressMap`, `ControlActionRegistry`). Command *vocabulary* parity is a gap — see group O. |
| Output profiles | Present (30 hits across shell services). |
| Support bundle / diagnostics | Present and redacted; Pro adds crash-dump pipeline and opt-in telemetry. |
| Update check, onboarding | Present. |
| Shared-memory robustness | Pro is **ahead**: fixed-capacity, instance-scoped SHM regions rather than the plugin's resize-generation scheme. |

### Gaps — group T: CoreVideo Tiles

The largest gap and the plugin's differentiator. Pro shipped a Tiles *layout*
in #401; it did not ship the plugin's Tiles *renderer*.

| Plugin capability | Pro today | Evidence |
|---|---|---|
| Glow (size, colour, intensity, softness) | Settings persist; nothing draws them | `grep -i glow native/src` → 0 matches |
| Rounded corners / corner radius | Settings persist; nothing draws them | same |
| Per-tile crop (left/right %) | Absent | no crop fields on the gallery model or route override |
| Animated layout reflow | Setting persists; no animator | `AnimateLayout` written and stored, never read by a renderer |
| Manual per-tile assignment | Absent — auto-fill only | `ReconcileDynamicGalleryRoutes` fills from roster order only |
| "Never show" exclusion list | Absent | `grep NeverShow` → 0 matches |
| Background colour / background source | Absent on the gallery model | `DynamicGallerySettings` has no background fields |
| Fill-never-letterbox crop semantics | Route `FitMode = "fill"` is set, exact crop math unverified against the plugin | — |

Borders themselves **do** work: scene borders composite into program as an explicit
operator choice (`MediaCore.cpp:4284`). The earlier "borders never composite into
program" rule was reverted.

Design spec: `superpowers/specs/2026-08-15-corevideo-tiles-parity-design.md`.

### Gaps — group Z: Zoom production surface

| Plugin capability | Pro today | Evidence |
|---|---|---|
| Spotlight slot 1–8 assignment | Route mode exists but **cannot resolve** — the SDK callback is a no-op stub | `onSpotlightedUserListChangeNotification(...) override {}` at `native/zoom-engine/engine/main.cpp:548`, `native/src/modules/ZoomMeetingSdkAdapter.cpp:214`, and `main-macos.mm:660` |
| Director manual take / release (operator supersede) | Absent entirely | `grep -E 'manualSpeaker|ManualSupersede|SpeakerTake|ForceSpeaker'` across the repo → 0 matches |
| ISO grace window on temporary participant loss | Absent (already flagged P1 in the real-meeting audit) | audit row "Temporary participant loss does not destroy an ISO immediately" |
| Interpretation audio channel | Absent — Zoom SDK headers present, no implementation | `grep -i interpretation` hits only vendored SDK headers |

### Gaps — group O: operability and control surface

| Plugin capability | Pro today | Evidence |
|---|---|---|
| `recover_stale_outputs` | Absent | `grep -E 'RecoverStale|recover_stale'` → 0 matches |
| `upgrade_low_quality_outputs` | Absent | `grep UpgradeLowQuality` → 0 matches |
| Cancel-recovery stop path | Absent | same search |
| Per-output health vocabulary (waiting-for-speaker vs stale vs missing) | Partial | Pro has health states; the plugin's specific discrimination is unverified |
| TCP/OSC command vocabulary parity | Partial | both servers exist; command-by-command diff not yet done |

## Explicitly out of scope

These are OBS-shaped, not product-shaped, and parity does not mean importing them:

- Running inside OBS at all — docks, `Tools >` menu placement, OBS themes.
- "Also start/stop OBS program recording" — Pro owns its own transport.
- `ffmpeg` on `PATH` as a recording prerequisite — Pro records through Media
  Foundation and treats an external ffmpeg dependency as a regression, not parity.
- The plugin's CPU-I420-through-shared-memory transport limits at 8+ feeds. Pro's
  GPU compositor path exists precisely to not have them.

## Decisions locked (2026-08-15)

1. **The bar is superset** — plugin parity is the floor; exceed where architecture allows.
2. **Tiles lives in the core.** The shell owns membership policy and styling; the
   core owns the layout solve, the animation clock, and the draw. Driven by a hard
   constraint: animated reflow needs a 60Hz clock, and giving the WinUI shell a
   frame-rate command path is the documented `CoreMessagingXP 0xc000027b` churn class.
3. **Per-tile overrides preserve canvas editing.** A tile the operator drags carries
   an override rect; un-overridden tiles solve into the remaining space and animate
   around it. A pinned host tile beside a reflowing gallery is a layout the plugin
   cannot express.
4. **Tiles styling is scoped to the wall.** The paused source-level border work
   (four decisions locked 2026-08-11, including "glow stays Tiles-only") stays
   paused and will reuse the shaders this ships.

## Sequencing

1. **T — Tiles** (specced now). Largest gap, the differentiator, and the only group
   requiring new compositor work.
2. **Z — Zoom production surface.** Spotlight is engine work; manual take is
   director plus a UI surface; the ISO grace window is already an audit P1.
3. **O — operability.** Recovery commands and a command-by-command control-API diff.

Windows leads in every group. `MetalCompositorAdapter` must reach the same place or
the Mac port ships without Tiles; that is tracked inside the T spec, not deferred
silently.

## Open items

- The exact glow falloff and the at-rest pixel-snapping rule are being extracted
  from the plugin's shipped shader into
  `obs-plugin-tiles-behavioral-contract.md`. The T spec depends on that document
  for those two formulas and states so where it does.
- The TCP/OSC command-by-command diff (group O) has not been done. The inventory
  above records that both servers exist, not that their vocabularies match.
