# Tiles incident: September 7, 2026

## Observed failure

The local test used `artifacts/render-stall-final/win-unpacked`, managed build
`0abd88eb75ce6e7f74207a54900f86136634782b`, rather than the September 7 public
alpha. Last completed UI snapshot: 11:44:30 EDT. Native rendering and Zoom frame
receipt continued until approximately 11:45:05. Close was requested at 11:45:49;
a second close forced exit at 11:46:04.

The native crash dump records `ucrtbase!abort` / `terminate`, exception
`0xc0000409`. Disassembly identifies assignment of a newly created thread over a
still-joinable video-ingest thread. Native symbols available locally did not
match this binary, so no mismatched PDB was forced. This establishes a shutdown
race, not the cause of the earlier loss of UI updates.

## Changes

- Serialize terminal Zoom runtime shutdown with incoming events. Reject late
  events and never replace an existing joinable ingest worker; join outside the
  event mutex. Regression tests exercise the stop-before-join window directly.
- Restore default gallery settings when a saved scene declares `dynamic-gallery`
  but omits its gallery settings. The observed saved shape previously became an
  ordinary active-speaker scene. Existing sync rules suppress stale ordinary
  routes once the gallery settings are restored.

## Validation boundary

686 native tests pass, including both shutdown regressions. Targeted managed
scene persistence and Tiles payload tests pass. The separate native Tiles smoke
harness reports its actual source, scene, and frame evidence; it does not exercise
WinUI/DXGI presentation or a real Zoom meeting.

The initial UI freeze remains unconfirmed. Reproduce with the candidate package,
retain the exact build manifest, and capture a WinUI hang dump before closing if
it happens again. Do not claim a repaired live-meeting Tiles freeze based solely
on the shutdown or headless native tests.