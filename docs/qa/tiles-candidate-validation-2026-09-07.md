# Tiles candidate validation — 2026-09-07

This candidate addresses the known shutdown race and adds selection feedback
protection, bounded busy-swap-chain presentation, and Tiles renderer/operator
controls. It does not establish the cause or resolution of the original live
WinUI selection freeze.

## Evidence

- Full native Release suite: **701 passed, 0 failed**. Decorated native Tiles
  smoke passed 20 activations and 20 switches away at each buffer depth (2/3),
  with 1/2/4/8 actual image-backed members, Program membership checks, Preview
  surface progress and clean child-process exits. This is functional headless
  evidence, not live camera or display-cadence acceptance.

- Explicit Windows Release native build succeeded. Four decoration tests verify
  real D3D BGRA and limited-range I420 pixels, effect-off behavior, side crop,
  extreme-setting bounds, and clean-shader fallback after effect compilation
  failure. The JSON-to-render-plan test verifies decoration and halo fields.
- Managed validation passed: 62 focused tests before the final editor hydration
  change, then 42 Tiles/persistence/feedback tests, 30 command-builder tests and
  all 54 Control tests. Counts overlap and must not be added together.
- Fresh self-contained Windows publish passed. A hidden `--verify-runtime`
  process returned success with `windowOpened:false` and four bundled runtime
  modules. No product window was launched.
- Live-selection API harness syntax and mocked restoration regression passed.
  Restoration requires native Program identity and frame progress, including
  when local state already claims the initial scene.

Local detailed evidence is under `artifacts/tiles-styling/`,
`artifacts/tiles-parity-managed-tests.log`, and
`artifacts/tiles-parity-runtime-probe.json`. Artifacts are not public release
contents.

## Remaining acceptance

- Run `scripts/qa/tiles-live-selection.mjs` against the running candidate in the
  designated test meeting; it never launches the app. Verify real scene
  selection, creation, saved-show reopen, scene-builder transitions and Take.
  If the UI stalls, preserve its hang dump before closing the app.
- Validate live participant loss/rejoin and independent feeds through the
  advertised count; exercise both buffer depths, Program/ISO recordings and
  a 60-minute rehearsal with external frame-cadence and A/V evidence.
- Metal code needs macOS compilation and pixel validation. Historical OBS crop
  texel rounding has not been independently matched.
- Pinned rectangles reserve their areas. Automatic tiles occupy the largest
  remaining free rectangle; disjoint smaller areas stay background. Editing is
  numeric/API based; drag handles are not implemented.
- The other plugin parity gaps remain tracked in
  `tiles-parity-priorities-2026-09-07.md`; this is not full parity acceptance.

No physical presentation or sustained 60 fps claim follows from these checks.
