# Multiview input inspector

Linked issue: [#674](https://github.com/iamfatness/CoreVideoPro/issues/674).
Depends on the control-state contract: [control-state-events-spec.md](control-state-events-spec.md)
([#657](https://github.com/iamfatness/CoreVideoPro/issues/657) / [#659](https://github.com/iamfatness/CoreVideoPro/issues/659)).
This is not a work queue. [BACKLOG.md](BACKLOG.md) owns rank.

Owner intake 2026-09-26: the 10 Multiview inputs need a ZoomISO-style outputs
table — live assignment, negotiated format/fps, ISO status, and the actual Zoom
picture — without clicking a row to force a refresh.

## What ZoomISO is doing in that screenshot

ZoomISO's Video Outputs page is a **row per plant output**. Each row is:

enable · index · rec · name · mode · who · device · format · audio tap ·
status · preview · meters.

CVP is not a DeckLink ISO router. The equivalent plant is the **10 Multiview
slots** (show inputs). Those slots already exist. The gap is a live inspector
that tells the truth about what Zoom and the core are doing to each slot.

Do not clone ZoomISO's hardware device column or "Number of Outputs" stepper.
Ten slots are fixed. Output device is Program / ISO file / vcam / NDI, not a
DeckLink SDI jack, until #537 has pixels.

## Why values go stale

The shell already has most fields. They are formatted from the last full
snapshot merge. Zoom facts (resolution, fps, video-off, talking, subscription
generation) often arrive without a scene sync. Selecting Preview, toggling a
source, or "tickling" the row forces that sync and the labels jump. That is the
same class as #608: a fact existed; the projection did not move until an
unrelated command rebuilt the document.

#657 is the bus. This inspector is a **consumer** of that bus, not a new poll.

Rules:

- Pixels stay on the existing GPU Multiview shared texture. No CPU thumbnail
  decode, no extra Zoom subscribe just to paint a row preview.
- Metadata columns bind to versioned observations (`SourceHealthChanged`,
  `SubscriptionStateChanged`, `OutputLifecycleChanged`, roster facts).
- A row that only updates after Preview/Take/click fails acceptance.
- Desired cap (`1080p60`) and negotiated actual (`1280x720@30`) are two fields.
  Show both when they differ. Never print the wish as if Zoom delivered it.
- ISO `Rec` is core output lifecycle, not a checkbox that lies while the writer
  is idle (#551 still owes live `framesWritten`).

## Column map (10 rows, slot 1–10)

| Column | Authority | Display |
|---|---|---|
| On | Shell intent → core applied subscription | Toggle. Pending vs applied from control revision. |
| # | Slot index | 1–10. Fixed. |
| Rec | Core ISO session for that source | Off / armed / recording / error. Red while recording. |
| Name | Slot label / lower-third name | Operator-editable; persist with #593. |
| Mode | Slot binding kind | Zoom guest / follow-speaker / UVC / screen / media / share. Inferred from source, not a second type dropdown (SRC-1). |
| Selection | Person + source id | Grouped picker. Missing stays Missing, never silent reassign. |
| Format | Core source health observation | `WxH@fps` from last accepted Zoom/UVC frame. Dim the configured cap when actual differs. Stale if `frameAgeMs` exceeds 1500 ms (#624 class). |
| Audio | Mixer tap + Zoom mute fact | ISO / program / off. Zoom-muted vs strip-muted stay separate fields. |
| Status | Source health + subscription generation | Live / connecting / video-off / stalled / no incoming. |
| Preview | Multiview tile crop | Existing texture. Tally border: program amber, preview green, talking white. |
| Meters | Core observation, coalesced | Peak only, ≥15 Hz UI. |

Omit ZoomISO's Output Device and Participant Controls until SDI/NDI rows are
real. Options menu: ISO folder, exclude-from-director (#668), open-in-Preview.

## Formatting

One row height. Monospace for format (`1920×1080@30`). Status is a chip, not a
sentence. Truncate names with tooltip, do not wrap and shove columns. Disabled
slots stay visible and dimmed so the 10-slot map does not collapse.

Empty Zoom meeting: Selection shows "No Zoom guests — join a meeting". Format
and Preview show the no-meeting slate already produced by the core. Do not
leave last-meeting numbers in the format column after Engine off.

## Event binding

Each row holds `slotId`, last applied `SourceInstanceIdentity`, and the highest
observation revision it has painted.

| Fact | Row update |
|---|---|
| Participant video size/fps / video-off | Format + Status |
| Active speaker | Tally on Preview cell only; do not rebuild the table |
| Subscription generation / stall | Status; Preview slate follows core health |
| ISO start/stop / error | Rec chip |
| Engine / meeting epoch change | Wipe format, status, selection incarnation |

Do not rebuild the 10-row collection on roster churn. Patch cells. #509-class
197 ms rebuilds are forbidden here.

`#659` is the first roster consumer. This inspector may reuse that projection
for Selection/Status. It must not invent a fourth snapshot (#610 rule).

## Non-goals

- Variable output count
- DeckLink/AJA plant routing (#537)
- New thumbnail encoder
- Tick-based full table refresh as the source of truth (a 250 ms snapshot may
  only repair gaps, never be the happy path)
- Director scoring UI (#668)

## Acceptance

Installed meeting, no click on the row:

1. Guest turns video off → Status and Preview slate change.
2. Zoom drops the guest from 1080p30 to 720p15 → Format changes to the actual.
3. Arm ISO on that slot → Rec goes armed/recording from core lifecycle.
4. Engine off → format/status clear; they do not linger from the last meeting.
5. Selecting Preview is not required for 1–4.

Tests fail if the row mapper only runs inside a scene-sync / Take path.
