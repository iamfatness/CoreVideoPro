# OHG hardware-show parity assessment

**Date:** 2026-09-08
**Question:** the hardware rig (Isadora + ATEM + ZoomISO fleet + SPX + BMD router) ran the daily show. What could it do, and what can CoreVideo Pro do today?
**Method:** the Isadora patch is the informer of hardware capability. CoreVideo Pro status is read from code, with file:line citations.
**Explicitly out of scope:** remote-control interfaces (web panels for V1 / A1 / V2, Cloudflare-tunnelled surfaces). Owner deferred these to a future state. This document is about capability, not about who drives it.

---

## Verdict in one paragraph

The show *brain* is at or beyond parity. Everything the Isadora patch computed — PIN identity, the Mukana join, editorial roles with host/reader exclusivity, slot laws, utility participants pinned to the tail, the hands queue, active-speaker routing, smart-gallery scoring, look resolution — is built, tested and running in `show-engine/`. The show *plant* is not. Roughly a third of what the hardware could physically do has no expression in CoreVideo Pro: there are no transitions, no black/bars/FTB, no clock, no playout queue, the gallery wall cannot leave the operator's monitor, and the switcher's source-agnostic input bus has been narrowed to Zoom participants in a way that actively evicts non-Zoom sources. Six of these are cheap. Three are architectural.

---

## A. Sources and inputs

| Hardware could | CoreVideo Pro today | Status |
|---|---|---|
| 16 switcher inputs, source-agnostic — a camera, a playback machine, a Zoom feed and a graphics fill were all just "an input" | 10 Show Input slots with 8 kinds (`ShowInputModels.cs:3-14`), but the OHG engine models *panelists*, not inputs. `slotParticipantIds` emits an entry for every slot and on first tick emits `assignSlot(9, null)` / `assignSlot(10, null)` (`hostCommands.ts:62-70, :131-141`); the facade's clear path sets `InShow=false; ParticipantId=null` (`StudioViewModelOhgFacade.cs:50-72`) | **Broken.** The advertised "8 Zoom + 2 other sources" cannot be expressed. A camera in slot 9 leaves the show on the engine's first drive-mode tick. |
| ~20 isolated Zoom channels out of ZoomISO | 8 concurrent video subscriptions (`ZoomMediaSpinePayloadBuilder.cs:19`) | **Partial.** A 9th Zoom guest cannot be shown regardless of scene design. |
| Zoom screen share as a routable switcher source | Ingested as its own 1080p SHM stream (`ZoomEngineRuntime.cpp:364-382`), but `screen-share` route mode nulls the participant id (`SceneRoutingService.cs:386-397`) and the core falls back to positional binding (`RouteSourcePolicy.h:39-45`). Not a `ShowInputKind`. | **Missing in practice.** The stream exists; nothing can reliably point at it. |
| 4 remote contributor inputs (hardware encoders) | `ShowInputKind.SrtIngest` exists | **Have**, subject to the slot-eviction defect above. |
| Return video to the meeting | Virtual camera, proven in Zoom | **Have.** |

## B. Composition and switching

| Hardware could | CoreVideo Pro today | Status |
|---|---|---|
| Program and preview buses | Real independent preview composite on its own keyed-mutex texture, own scene graph (`MediaCore.cpp:5306-5332`, `D3D11CompositorAdapter::renderPreview`) | **Have.** |
| Cut, mix, dip, wipe with rate | Cut/fade/dip/wipe are picker labels only. `TakeTransitionMode` is never serialised; Take is a hard scene-id swap (`TransportCoordinator.cs:151-152`). No A/B mix, no dissolve buffer, no wipe shader anywhere in `native/src/` | **Missing.** Every take is a cut. |
| Black, colour bars, fade to black | No `transport.ftb`, `black` or `bars` in the control registry (`ControlActionRegistry.cs:45-174`). "Black" is an OHG preset name pointing at a scene the operator must have authored (`ShowConfig.cs:43`) | **Missing.** |
| SuperSource 1 plus cascaded SuperSource 2, up to 8 boxes with borders and a background plate | Scenes with unbounded layers (`MediaCore.cpp:4531-4570`), first-class `Scene.background` media asset at order −100 | **Have**, and richer. |
| DSK 1-4, independent, each with rate/clip/gain | One canonical downstream lower-third key plus N independently-toggled graphics — but every non-corner graphic collapses into the same lower-third rect (`MediaCore.cpp:4758-4762`), overlays are global rather than per-scene (`StudioViewModel.cs:835`), and they never reach the preview bus (`MediaCoreCommandBuilder.cs:114-129`). Browser overlays are true unbounded DSKs (`BrowserOverlayProgramService.cs:10-38`) | **Partial.** No numeric cap, but only two geometries, no per-keyer rate/clip/gain, and you cannot preview a key before you take it. |
| 16-cell gallery wall composed on the switcher | Tiles, 1-64 cells. But member order is roster order (`TilesLayerPayloadBuilder.cs:36-41`); the engine's smart-gallery ordering is computed and then discarded — there is no Tiles order API | **Partial.** The wall renders; the ranking the engine worked out never reaches it. |
| Wall on one output while program showed something else | One video program bus. `work.programFrame` is the single frame handed to every sender; output destinations carry no source selector (`MediaCore.cpp:88-120`). `aux-1`/`aux-2` are audio only | **Missing.** No video aux. |

## C. Identity, roster and show logic

| Hardware could | CoreVideo Pro today | Status |
|---|---|---|
| PIN-based identity, Mukana REST join, editorial roles, host/reader exclusivity, slot laws, utility tail, hands queue, look resolution | All of it, in `show-engine/`, 948 tests | **At parity.** |
| Active-speaker follow | `ProgramBus.setActiveSpeakerFollow` (`programBus.ts:72-92`) is real. The shell's own `SourceRouteMode.ActiveSpeaker` is label-only — it nulls the participant id and falls through to positional binding (`SceneRoutingService.cs:399-412`) | **Have in the engine; the shell's version is a decoy.** |
| Smart gallery scoring | Computed by the engine | **Have**, but unapplied (see Tiles above). |

## D. Graphics

| Hardware could | CoreVideo Pro today | Status |
|---|---|---|
| SPX lower third: name **and** location | Engine computes both; `SetInputLowerThirdTitle` is a deliberate no-op (`StudioViewModelOhgFacade.cs:86-98`). Shell renders name only | **Partial.** Location silently dropped. |
| Per-box nameplates inside the gallery | Nothing draws a plate per Tiles cell | **Missing.** |
| Start card, end card, background plate, full-frame video | All buildable as scenes with media routes and `Scene.background` — but none is mapped from the OHG show model, and each is a scene the operator must author by hand | **Partial.** The capability exists; the show does not know about it. |
| Show clock / countdown on air | Only a multiview wall clock, operator-monitor only (`MediaCore.h:938`). `preShowCountdown.ts` and `showClock.ts` exist solely in the dead Vite prototype | **Missing.** |
| Dual SPX for redundancy | Single graphics path | **Missing.** |

## E. Outputs and feedback

| Hardware could | CoreVideo Pro today | Status |
|---|---|---|
| oh.tally.video showing **preview and program** | `TallyState` carries program only — `showEngine.ts:1242-1248` passes `source: program.program`. The legacy `MixEffect_Info_v15` produced separate PREV and PROG PIN lists | **Half missing.** The reduction was silent; no doc records preview tally as deferred. |
| Tally posted over HTTP to the tally service | Derivation is done. Nothing posts it. Endpoint contract unknown | **Missing transport.** |
| MV16 gallery wall out through the BMD router to the gallery | Multiview is a single-consumer keyed-mutex texture; no sender consumes it; the pop-out window *unbinds* the main view | **Missing.** The operator wall cannot leave the operator's screen. |
| Program fanned to multiple destinations | Have, with per-destination protocol/url/key | **Have.** |

## F. Operations

| Hardware could | CoreVideo Pro today | Status |
|---|---|---|
| Rundown / playout queue with segment timing | Does not exist outside the dead Vite prototype (`src/engine/cueSheet.ts`, `showClock.ts`) | **Missing.** Owner has named this as wanted. |
| Show file portability — carry the patch to another machine | 6+ files, scenes live inside preferences, secrets are DPAPI CurrentUser-scoped and will not decrypt on another machine or account | **Partial.** The show is not a document you can move. |

---

## What this adds up to

**Nine gaps that would stop a hardware-parity show.** Ranked by how much show they cost, not by effort:

1. **Non-Zoom sources are evicted from OHG slots.** The product's core claim breaks the moment the engine drives. Engine needs an input vocabulary, not a panelist vocabulary.
2. **No transitions.** Every take is a cut. This is the single most visible difference between our output and the hardware show's.
3. **Preview tally missing and nothing posts.** The gallery lost half its information and all of its delivery.
4. **No playout queue.** Named by the owner as wanted; there is nothing to build on outside dead code.
5. **Gallery order discarded.** The engine's best work is thrown away at the shell boundary.
6. **No black / bars / FTB.** One-click safety net a switcher always has.
7. **No show clock or countdown.** Every live show has one.
8. **Nameplate location dropped**, and no per-box plates in the gallery.
9. **No video aux.** The wall cannot be produced alongside a different program.

**Cheap** (contained, no architecture): 3, 5, 6, 7, 8.
**Medium:** 2 (real compositor work, but bounded), 4.
**Architectural:** 1 (engine data model), 9 (a second render path through the compositor and senders).

**Not a gap:** the show brain, the preview bus, the composition ceiling, SuperSource-equivalence, program fan-out, return video. These meet or beat the rig.

---

## Note on the workspace

This assessment does not depend on PR #414 (the OHG Show WinUI workspace), which the owner rejected as unusable and which must not be merged. Every gap above is in the engine, the bridge, the shell services or the core — none of them is a panel-layout problem, and none is fixed by rebuilding that tab.
