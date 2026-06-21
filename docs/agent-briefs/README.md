# Native Build Agent Briefs

Electron has been removed from CoreVideo Pro. Do not use archived Electron dispatch prompts, `desktop/`, `typecheck:desktop`, `dev:desktop`, `pack:desktop`, or Playwright-Electron tasks.

Active product paths:

- `studio/` - native C++ Studio shell for fast desktop validation.
- `native-shell/` - WinUI shell for Windows demos and packaging.
- `native/` - C++ media core and Zoom/GPU/encoder adapters.
- `native-core/` - Node mirror for protocol/runtime tests while native parity hardens.
- `src/` - shared operator state, contracts, and renderer logic that can be hosted by native shells.

Active records:

- `06-decision-zoom-capture-path.md` - decision record: the vendored standalone Zoom
  capture engine (`native/zoom-engine/`) is the primary Zoom capture path.

The earlier native-media-core build briefs (skeleton + next-milestones dispatch
prompts) have been completed and are superseded by
[`../native-production-completion-plan.md`](../native-production-completion-plan.md),
which carries the current per-feature plan and status.

Ground rules:

- Native media work must stay buildable with `COREVIDEO_STUB=ON` by default.
- Real Zoom SDK, GPU, encoder, NDI/SRT/WebRTC, and hardware code must stay behind explicit dev-machine gates.
- The default app validation path is `npm run build:studio` and `npm run run:studio`, plus native media-core and native-shell tests.
