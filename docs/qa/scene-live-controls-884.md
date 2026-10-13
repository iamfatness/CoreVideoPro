# Scene Builder live controls — #884

Owner reported on October 13, 2026 that edits reached the displayed image only after changing the source. The shell already sends the updated Preview draft. Native `set-preview-scene` deduplication omitted source scale and horizontal/vertical offsets, route border styling, and Tiles opacity/transparent background. A source identity change invalidated the signature and incidentally applied the previously ignored values.

The fix includes those properties and route identity in the native signature. Repeated edits now update the Preview composition without changing sources. Program remains the captured on-air scene until explicit Take; Save continues to persist the draft without publishing it.

Release verification on Windows, RTX 4090, October 13:

| Check | Result |
|---|---|
| `MediaCoreCommand.Preview*` | 5 passed, including repeated same-source scale, pan, tilt, fit, position, size, opacity, border and order edits; same-wall opacity and transparency; single-source composite/dedup and route grading |
| `D3D11Compositor.FramingChangesHeldSourcePixelsWithoutReplacingTheSource` | 1 passed; actual GPU pixels change with pan on an unchanged source frame and return byte-for-byte on reset |
| `TilesWorkspace.*` | 3 passed; wall bounds/opacity, authored ordering, hidden-wall fallback and duplicate guest identity |
| `git diff --check` | Passed |

Tests ran on the local review integration based on `41d70f4bf7ea473ac811a35e56040cc22361d920`, with Release native core SHA256 `e7d5836e4175cbf88f7639b7c5b9ae8d105b0ecbd5aebcc6199913a2977ec774` and test executable SHA256 `c1712fad33a8c702e24111577822f4467f6351fca866cfefd0a2c244b5b8167c`. The focused source patch is stacked on the Scene Builder PR (#880), excluding unrelated runtime integration commits.

Installed acceptance remains unverified: the owner's joined app was not replaced during this test. These finite control/pixel tests do not establish zero missed output deadlines or resolve the separately tracked Zoom resolution-ramp black flashes (#582).
