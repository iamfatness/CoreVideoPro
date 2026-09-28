# Multiview input inspector execution plan

Linked issue: [#674](https://github.com/iamfatness/CoreVideoPro/issues/674).
Requirements: [multiview-input-inspector-spec.md](multiview-input-inspector-spec.md).
This is a dependency plan, not a ranked work queue. [BACKLOG](BACKLOG.md) owns
priority. No foundation-only PR lands on `main` without a shipping consumer.
Slices A–F merged as #675–#680. The inspector consumes the versioned roster
projection; it must not invent a fourth snapshot. Issue #674 remains open for
installed no-click video-off, ISO arm, and Engine-off transitions and ten-row
GPU load evidence. The preferred Zoom resolution choice is #681; Sources
layout cleanup is #590. Current rank lives only in [BACKLOG](BACKLOG.md).

## Why this order

Stale labels are a projection bug, not a paint bug. Binding Format / Status /
Rec to versioned facts has to land before the ZoomISO-looking row chrome.
Pixels already exist on the Multiview texture. Do not add a thumbnail encoder.

## Slice A — row model and cell patch

Add a focused `MultiviewInputRow` projection (WinUI + the same fields on macOS
later) keyed by slot 1–10. Hold `slotId`, applied `SourceInstanceIdentity`,
and the highest observation revision painted. Patch cells on fact; never
rebuild the 10-row collection on roster churn.

Consumer: Format and Status update from a fake-bridge `SourceHealthChanged` /
video-off fact without `SetScene` or Take. Test fails if the mapper only runs
inside scene-sync.

Do not restyle the table in this slice beyond readable `WxH@fps` and status
chips. Do not grow `StudioViewModel` with per-cell formatters; keep a small
row mapper type.

Depends on #659's roster/source identity if Zoom guests are the payload. Until
that lands, the fake-bridge fixture is the consumer proof.

## Slice B — negotiated format vs configured cap

Publish actual last-frame width/height/fps and `frameAgeMs` on the existing
source-health observation. The row shows actual. When configured cap differs,
dim the wish next to it. Age > 1500 ms marks Format stale (#624 class) without
clicking the row.

Consumer: a Zoom (or fake) downshift 1080p30 → 720p15 changes the cell.
Engine-off / meeting epoch change wipes last-meeting numbers.

## Slice C — ISO Rec from output lifecycle

Bind Rec to `OutputLifecycleChanged` for that source's ISO session: off /
armed / recording / error. Do not infer Rec from a checkbox. Live
`framesWritten` stays #551; this slice only needs session state.

Consumer: arm/stop ISO on a slot updates Rec with no Preview select.

## Slice D — ZoomISO-style chrome on the existing 10 slots

One row height, monospace format, chips, dimmed disabled slots that do not
collapse, preview cell as a crop of the existing Multiview texture with tally
borders (program amber, preview green, talking white). Options: open in
Preview, exclude-from-director later (#668). No DeckLink device column. No
output-count stepper.

Consumer: Sources / Studio inspector shows all ten slots in this layout.
Formatting-only; facts already flow from A–C.

## Qualification

- Fake-bridge: video-off, resolution drop, ISO arm, engine-off wipe, no
  Preview/Take required.
- Native observation fields present on stub core.
- Installed meeting evidence on #674 before close: guest video-off, fps/size
  change, ISO arm, Engine off. Record SHA and that the row moved without a
  click.

## Review gate

A reviewer must answer: which observation revision the cell last painted, what
happens on epoch change, why Preview is not in the update path, and which test
fails if the mapper is moved back into scene-sync.
