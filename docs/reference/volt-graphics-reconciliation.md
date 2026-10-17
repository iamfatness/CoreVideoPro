# Volt Graphics: reconciliation with existing work

Review material for [#924](https://github.com/iamfatness/CoreVideoPro/issues/924),
requested October 16, 2026. Source baseline: main
`079e79caaa51e30a550be060def774e1fdc8d047`. The separately installed stream-control
review build `ce73404` adds no native graphics implementation.

The supplied [original proposal](../design-reference/volt-graphics-original-2026-10-16.md)
is preserved byte-for-byte as product input, not an executable
work order. Its architecture and feature ambition inform the
[reconciled specification](volt-graphics-spec.md); the
[delivery plan](volt-graphics-implementation-plan.md) describes dependencies and
verification. Only `docs/BACKLOG.md` ranks work. This reconciliation does not
close existing issues, assert installed acceptance, or authorize implementation.

## Source-grounded inventory

| Area | What the inspected source establishes | How the proposal changes |
|---|---|---|
| Lower-third looks | `LowerThirdAppearance` already defines compact-solid, minimal-accent and broadcast looks, geometry, typography, colors, opacity and logo options. | Preserve these as compatibility templates; do not rebuild a minimal hard-coded lower third as a new foundation. |
| Designer and presets | `LowerThirdDesignerControl` has a draft preview, comparison, Apply/Cancel, style sections, saved looks and duplication. `OverlaysViewModel.Appearance` persists named appearances. | Build a coherent template/instance workflow around this consumer. Expand capabilities after mapping every old control and saved value. Existing controls are not proof of native effect or acceptance. |
| Source text | `SourceLowerThirdText` and `StudioViewModel.SourceLowerThirdText` provide source-bound overrides. #592/#593 still carry output/identity acceptance. | Keep text ownership in Sources. Graphic-instance overrides are explicit, reversible and do not rewrite guest metadata. Preserve intentional blank secondary text. |
| Native styles | `MediaCore::setBrandKit` parses `lowerThirdAppearance`; Program plan construction consumes it. | #764's October 2 blanket assertion that styles have no consumer is no longer a complete description. Audit each remaining field; do not assume the whole issue is fixed. |
| Windows rasterization | `CompositorOverlayRaster` uses DirectWrite/D2D/WIC and a content-signature texture cache; its header explicitly places it on the render thread. Cache eviction is count-based at eight entries. | Reuse layout/pixel semantics, but move cache-miss work off the deadline. A generic multi-layer engine cannot safely inherit synchronous raster/upload or an eight-entry count-only cache. |
| Metal | `MetalCompositorAdapter.mm` contains Core Text/ImageIO rasterization with a last-signature overlay texture cache. Its introductory portable-font/later-CoreText comment is stale. | Extend and qualify the real Core Text path. Generalized graphics needs multi-entry byte admission rather than alternating unrelated content through one cached texture. This is a static architectural risk, not a measured live stall diagnosis. |
| Preview | `syncPreviewScene` currently describes statically rendered Preview overlays without an independent animation clock. | Distinguish authoring simulation from cue Preview and Program runtime. Animated native authoring preview is additional work, not current capability. |
| Core compositor | Program render plans already contain lower-third/bug overlay layers, animation-related state and legacy fallback paths. | Add a focused graphics adapter and prove coexistence before removing old fallbacks. Do not add another GPU video bus or renderer. |
| Director state | C++ `Director.h` already produces deterministic scene recommendations and slot bindings. Typed `NativeMediaCoreAutoProduction` and generated raw observation accessors exist. `MagicSceneCoordinator` requires populated bindings before acting. | #761's original no-model diagnosis cannot be copied unchanged. Trace wire -> mapper -> recommendation -> binding -> execution; record the exact broken seam, if reproduced. A recommendation is not an executed Take. |
| TypeScript overlays | `show-engine/src/overlayDirector.ts` structurally derives nameplates/questions/headlines, deliberately avoiding changes for identical values. | Preserve derivation behavior and tests. It may submit intent through an adapter; it must not run a second graphics animation/presentation authority. No verified native-migration inventory justifies deleting it now. |
| Contracts | `contracts/` generates additive lifecycle/identity models for C++, C# and Swift; it explicitly does not generate the whole legacy protocol. | Add a graphics contract slice through existing tooling. Do not promise generated whole-protocol TypeScript/React or a shell that has been removed. Verify any additional consumer before adding its generated model. |
| Data integrations | The proposal lists spreadsheets, Sheets, JSON, XML/RSS and WebSockets as targets. This review does not establish a working generic native graphics data-controller framework. | Make them later gated consumers. Define transport/data limits, credentials, stale-data behavior and mapping semantics before enabling them. |

Paths referenced above are in `native/src/{core,compositor,modules}/`,
`native-shell/CoreVideoPro.{WinUI,MediaCore}/`, and `show-engine/src/`.
These are static-code findings, not claims that all paths were exercised in the
installed app. This task makes no live app/API/meeting changes.

## Existing issue boundaries

| Issue | Retained responsibility | Relationship to Volt |
|---|---|---|
| [#847](https://github.com/iamfatness/CoreVideoPro/issues/847) | Three looks; usable appearance designer; real rendering, Apply/Cancel and preset acceptance | First graphics consumer and compatibility baseline. Close only against its original output criteria. |
| [#592](https://github.com/iamfatness/CoreVideoPro/issues/592) | Stable unchanged metadata, casing, no fragmented text or spurious transition | Content/binding regression gate across all templates; keep independent failure evidence. |
| [#593](https://github.com/iamfatness/CoreVideoPro/issues/593) | Source-bound primary/secondary text, default/reset/blank behavior, identity and persistence | Remains the source-data owner; no second competing guest-text store. |
| [#764](https://github.com/iamfatness/CoreVideoPro/issues/764) | Field-by-field consumer evidence for previously dropped settings | Graphics work covers brand/style/image/overlay-behavior fields only. LUT, PTZ/audio routing and unrelated capture fields are not closed by this milestone. |
| [#761](https://github.com/iamfatness/CoreVideoPro/issues/761) | Truthful Magic Scene / Set & Forget binding and execution | Existing automation must be traced and qualified before graphics rules can depend on its recommendation events. |
| [#848](https://github.com/iamfatness/CoreVideoPro/issues/848) | Consistent direct/precise adjustments, units, reset and feedback | Designer interaction contract; preserve sliders plus exact entry and keyboard access. |
| [#839](https://github.com/iamfatness/CoreVideoPro/issues/839) | Webcam-only OH framer and loudness overlay | Remains a separate output-specific path. Volt Program graphics must not turn that overlay on or burn it into clean outputs. |
| [#517](https://github.com/iamfatness/CoreVideoPro/issues/517), [#916](https://github.com/iamfatness/CoreVideoPro/issues/916) | Media/output continuity and source-monitor acceptance | Graphics qualification is additive. Successful graphic appearance does not close output/monitor performance findings. |
| [#924](https://github.com/iamfatness/CoreVideoPro/issues/924) | This specification/reconciliation and review | Tracks proposed new generic instance, bug/clock and data/controller scopes until the owner approves and ranks their implementation issues. |

## Decisions that replace the supplied draft

1. Use **Graphics** in the app; **Volt Graphics** remains the working subsystem
   name. No separate app, NDI hop, plugin purchase or second production renderer.
2. Start with a minimal declarative package loader **and the existing three real
   lower-third consumers** in the same vertical slice. No skeleton-only PR.
3. Separate template design, cue, on-air update and stream/output scope. Template
   editing is never an implicit Program action. Compatibility Apply remains an
   explicit atomic on-air edit when that is what its label says.
4. Core owns accepted graphics state, timing, arbitration and render revisions.
   Workers own preparation; UI owns drafts/intent. Existing optional show-engine
   rules are clients of that authority, not replaced wholesale.
5. Begin with an admission limit of eight composited instances per bus and a
   bounded shared preparation/cache budget. These are qualification limits, not
   a promise of sixteen arbitrary 4K layers or unlimited effects.
6. Deliver lower thirds, logo bugs and clocks as the first complete product
   milestone. General data tables/crawls follow after lifecycle and automation
   evidence. Sports, AE/PSD import, MAM and external key/fill remain later intake.
7. Remove unsupported vendor comparisons and fixed week estimates as acceptance
   claims. Competitive parity, licensing and third-party features were not
   researched in this task; the target is a usable native workflow, measured on
   named hardware. Estimate after consumer/worker spikes and template trials.
