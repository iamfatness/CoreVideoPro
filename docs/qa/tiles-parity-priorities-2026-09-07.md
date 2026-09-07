# Tiles stability and OBS plugin parity: current execution priorities

Date: 2026-09-07. Pro audit: `ac5daaf`; OBS reference:
`67ae1732bf112c593d35902ca1eac307dd9bd1f2` (current main at audit time),
including `docs/CORE_PLUGIN_FUNCTIONALITY.md`.

The existing parity charter remains the requirement: every CoreVideo plugin
workflow must have a working Pro equivalent. Tiles selection stability is the
first gate. The shutdown race fix does not close the reported selection freeze.

## Audit baseline (before implementation)

| Capability | Current state | Evidence |
| --- | --- | --- |
| Core grid/aspect/spacing and fresh-member admission | Implemented; headless native tested | `TilesLayout.h`, `TilesMembership.h`, `TilesRenderPlanTest.cpp` |
| Program and Preview composition | Native switching tested; live UI selection freeze unresolved | `tiles-scene-smoke.mjs`, incident report |
| Border styling | Missing on Tiles; native explicitly disables it | `MediaCore.cpp` Tiles expansion |
| Radius and glow | Settings persist but not carried through Tiles wire/rendering | `TilesLayerPayloadBuilder.cs`, `ProductionModels.cs` |
| Animated reflow | Settings persist; native currently recomputes static geometry | same payload and native expansion |
| Manual assignment / never-show exclusions | Missing from membership policy/model | Tiles builder chooses video-enabled roster order |
| Per-tile crop and operator overrides | Missing from Tiles wire/model | Tiles builder and gallery route reconciliation |
| Wall background controls | Partial: native color and scene backgrounds exist; shell hardcodes black | Tiles builder, native background expansion |

A visible control, successful API acknowledgment, or stored preference is not
feature completion. Settings must visibly affect Preview, Program and the saved
recording. A headless native pass does not prove the WinUI scene-selection path.

## Execution order and acceptance

Implementation in progress on `codex/tiles-crash-hardening` now includes dedicated
Tiles border/radius/glow shaders, render-clock animation with separate Program and
Preview state, manual slots and exclusions, crop/rectangle/order overrides, and
solid/live-source backgrounds. Scene-selection notifications suppress two-way
binding echoes; presentation defers busy swap chains and records slow stages.
These are candidate changes, not proof that the original live freeze is resolved.

Pinned rectangles reserve their areas; automatic members occupy the largest
remaining free rectangle. Other disjoint regions remain background. The editor
currently uses numeric controls and API actions, without draggable tile handles.
Windows GPU, managed, package and live acceptance results must be recorded
separately; Metal source changes still require macOS build and pixel validation.

1. **Reproduce and fix scene-selection failure.** Use the candidate build and
   its exact manifest; capture UI and native state before shutdown. Exercise
   selecting an existing Tiles scene, creating one, reopening the saved show,
   entering/leaving the scene builder, and Take. Use live Zoom sources as well
   as deterministic inputs. Capture a UI hang dump if selection stops responding;
   the available native dump proves the subsequent shutdown race only.
2. **Complete the wall renderer.** Carry border/radius/glow settings into the
   native Tiles schema and renderer. Verify rendered pixels, persistence and
   Preview/Program isolation. Effects must degrade to visible clean tiles on
   failure. Then implement core-clock reflow and test empty-to-first-arrival,
   rapid joins/leaves, departure/re-entry, and settled pixel alignment.
3. **Complete operator control.** Implement manual slots, exclusions, per-tile
   crop/position overrides and wall backgrounds without replacing unrelated show
   settings. Verify identity stability through speaker changes, camera toggles,
   participant loss and reconnect.
4. **Run the Tiles production gate.** Cover 0/1/2/4/8/16 participants (and any
   higher advertised supported count), both buffer depths, repeated cue/Take,
   Program and ISO recording, and a sustained 60-minute rehearsal. Check actual
   external frame cadence, A/V alignment, source identity and recovery. Persist
   every missed-frame/error counter; do not relax the 60 fps requirement.
5. **Close remaining plugin parity gaps.** Audit Zoom routing/director, ISO
   lifecycle, audio/talkback and control/recovery command semantics against the
   pinned current plugin. Record implemented, incomplete and unverified separately;
   keep Tiles first rather than using unrelated feature work to defer its gate.

## Existing candidate evidence

Candidate `e787e8e`: 686 native tests, 15 targeted managed tests, bundled-runtime
probe and 918 package-file checks passed. Native Tiles smoke passed 20 activations
and switches away at each buffer depth with actual image-backed tiles and clean
exits. These checks do not establish that the user's live selection freeze is
fixed, nor that the missing styling or control features exist.
## Other plugin parity findings

| Capability | Current Pro status | Evidence / next acceptance |
| --- | --- | --- |
| Spotlight slots 1-8 | Gap | SDK spotlight callback is empty; roster-position fallback is not Zoom spotlight order. |
| Director manual take/release | Gap | Automatic timing exists; operator supersede/release is absent. |
| Targeted stale recovery / quality upgrade | Partial | Automatic supervisor recovery exists; targeted operator commands do not. |
| ISO participant-loss grace | Partial/unverified | Writers persist and align silence, but no explicit grace/rejoin identity contract. |
| Interpretation audio | Gap | No implementation found outside vendored SDK declarations. |
| Private talkback/intercom | Gap | Current plugin documents this; no Pro implementation or control action found. |
| Companion workflows | Partial | Pro module exists; live name-resolved assignment/recovery workflows remain incomplete. |
| Output health semantics | Partial | Distinguish waiting for a directed speaker, missing feed, stale pixels and recovery result in behavior tests. |

Audit paths include `native/zoom-engine/engine/main.cpp`,
`native/src/modules/ZoomMeetingSdkAdapter.cpp`, `ZoomActiveSpeakerDirector.h`,
`native-shell/CoreVideoPro.Control/ControlActionRegistry.cs`,
`IsoSourceSelectionResolver.cs` and `companion-module-corevideopro/src/main.ts`.
The fresh plugin documentation was inspected at the pinned commit above; these
are source-audit findings, not live acceptance results.
