# Show Input capacity: 16 sources, 8 Zoom feeds

**Owner ruling, 2026-11-13** (issue #1041; asked when CoreVideo Edge made more
sources practical):

- Show Inputs and the multiviewer hold **16 sources** (they held 10).
- **Pro itself pulls at most 8 Zoom video feeds**, all inside the 1080P grant.
  Before the ruling it pulled 10 (8 at 1080P).
- The other slots take any source the operator routes, for example:
  - CoreVideo Edge feeds over NDI or SRT
  - a screen share
  - a webcam or capture card
  - a clip or a browser source

  Two Edge nodes can fill all 16 slots with no Pro Zoom feed at all.
- **ISO recording stays at 8** (`kIsoReaderCount`, consumer NVENC session limits).
  Edge records its own guests locally.

## Where each number lives

| Number | Owner | Value |
|---|---|---|
| Show Input slots | `ShowInputRosterService.MaxShowInputs` | 16 |
| Multiview source tiles | `ShowInputRosterService.MaxMultiviewBoxes`, `GpuTierPolicy.ProductTileCapacity`, `StudioViewModel.MaxMultiviewTileCount`, core `MediaCore::multiviewTileCount_` default, `configureMultiviewer` clamp | 16 |
| Zoom video subscriptions | `ZoomMediaSpinePayloadBuilder.DefaultMaxVideoSubscriptions` (Mac: `ZoomSourceBudget.maxVideoSubscriptions`) | 8 |
| 1080P camera grant | `ZoomSubscriptionResolutionPolicy` `kMaxConcurrentFullResolutionCameras` | 8 (unchanged) |
| Zoom slots filled by roster auto-assign | `ShowInputRosterService.MaxAutoAssignedZoomSlots` (Mac: `AppModel.maxAutoAssignedZoomSlots`) | 8 |
| SRT / RTMP ingest listeners | `StudioViewModel.MaxSrtIngestSources` (ports 10000-10015), `RtmpIngestSourcesCoordinator.MaxSources` | 16 each |
| CPU-prepared sources active at once (NDI, SRT, RTMP, UVC, browser) | `CpuSourcePreparation::kMaxActive` | 24 |
| Shell-SHM captures (webcam, screen) | `ShmCapturePreparation::kMaxSources` (memory still bounded by `kBudgetBytes`, 512 MiB) | 24 |
| Sources-inspector thumbnail exports | `MediaCoreMonitors.cpp` `kMaxInspectorRows` | 16 |
| Multiview tile ceiling per GPU tier | `GpuTierPolicy.ForTier` (#439): under 8 GB or unknown 9, 8-16 GB 16, 16 GB+ 25 capped to 16 | unchanged |
| Per-guest ISO | `kIsoReaderCount`, `IsoSourceSelectionResolver.DefaultMaxIsoSources` | 8 (unchanged) |

## Behavior rules

- **Auto-assign stops at 8 Zoom slots.** A slot the operator filled with a
  Zoom guest by hand counts toward the 8, and the operator may always place a
  Zoom guest in any slot. A camera-on Zoom source past the video budget is
  never a silent frozen tile. Its tile reads "<name> · no video: subscription
  limit (8)", and a cued bus source is named on the PGM/PVW cell (see
  `gotchas.md`, the #478 entry).
- **Saved profiles.** Preferences v18 moves a saved multiview tile count of
  exactly 10 (the old maximum, "every input on the wall") to 16. Any other
  saved count is kept. Saved rosters load by slot number, so slots 11-16 start
  empty.
- **Wall geometry** (`CompositorLayout.h`). The pgmPvwTop/Large strip picks
  the 1-4 row count that gives the largest 16:9 tile, taking fewer rows on a
  tie. That gives the same wall as before through 12 sources, and three rows of
  1/6-width tiles at 13-16. The pgmPvwSide strip splits into two columns past
  8 sources.
- **Not changed:**
  - The OHG show engine keeps its own `capacity` of 10 (`ShowConfigValidator.hostCapacity`, persisted OHG configs; the Companion `ohg/slot/{slot}` expansion follows it).
  - The director's panel threshold (2..10) is a separate setting.
  - The media decoder pool (`MediaTransports` `kMaxDecoders` 16) counts clips, not slots.

## Not yet qualified

#517 recorded that Program render hits the one-frame budget at 16 Zoom
participants with the 1080p wall. A 16-source workload (8 Zoom + 8 NDI or SRT
from Edge) needs its own render-budget and outputs-drill measurement before
anyone claims 16 sources on air. The website and release notes must not
advertise 16 until it passes.
