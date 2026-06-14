# Native Build Agent Briefs

These documents hand off the CoreVideo Pro native build work to two coding
agents working in parallel. Read `00-native-build-plan.md` first for the full
context and architecture seam, then dispatch each agent with its brief.

> **Demo-driven roadmap:** for the sprint-by-sprint path to the first live demo
> (weekly sprints, a demo every Friday), open
> [`../roadmap/index.html`](../roadmap/index.html) in a browser.
>
> **Decision — Zoom capture path:** the vendored, proven engine
> (`native/zoom-engine/`) is the active capture path; Codex's
> `ZoomMeetingSdkAdapter` is parked. See
> [`06-decision-zoom-capture-path.md`](06-decision-zoom-capture-path.md).
>
> **Sprint 1 goal (active):** self-driving "join Zoom & see live feeds" objective
> for Codex — [`07-sprint-1-goal.md`](07-sprint-1-goal.md).

- `00-native-build-plan.md` — the overall work plan, environment constraints,
  track split, coordination rules, and integration gate.
- `01-track-a-desktop-shell.md` — Agent A (Claude): Electron desktop shell +
  real main/renderer/native IPC bridge. Fully runnable in a Linux container
  with stubs.
- `02-track-b-native-media-core.md` — Agent B (Codex): C++20 native media-core
  process skeleton with stubbed, interface-isolated SDK adapters.

### Round 2 — next-milestone plans

The round-1 scaffolding (Electron shell, IPC bridge, native core skeleton,
6-family bridge protocol, typed Zoom media spine) is **already built and merged**.
The round-2 plans pick up from that baseline — real integration behind the
existing seams, not new scaffolding:

- `03-track-a-next-milestones.md` — Agent A: wire the Zoom media spine through
  the real native bridge path, drive the spine controller + readiness in the UI,
  harden the supervisor, capability-gate Phase-2 outputs, packaging + e2e smoke.
- `04-track-b-next-milestones.md` — Agent B: implement real adapters behind the
  existing `native/` interfaces (Zoom SDK, GPU compositor, hardware encoder +
  recording, output senders, capture devices), gated by
  `COREVIDEO_ENABLE_DEV_ADAPTERS` so the default in-container build stays green.
- `05-dispatch-prompts.md` — ready-to-paste kickoff prompts for each agent.

## Ground rules for both agents

- **Environment constraint:** the default build/test target is a plain Linux
  container with no GPU, no Zoom SDK credentials, and no Blackmagic/AJA
  hardware. Everything must build and test green there using stubs. Real
  SDK/GPU/hardware code is isolated behind interfaces and config-gated for a
  Mac/Windows dev machine.
- **Contract source of truth:** the TypeScript protocol files
  (`src/engine/nativeBridgeProtocol.ts`, `src/engine/nativeMediaCoreProtocol.ts`,
  `src/engine/nativeMediaCoreCommands.ts`) define the wire format. Agent A owns
  edits to them; Agent B mirrors them on the C++ side. Land protocol changes
  first, in their own commits, before either side builds against them.
- **Stub-first, real-later:** both tracks must run end-to-end on stubs before
  any SDK-bound code is added.

## Assumptions (change before dispatch if wrong)

- Desktop shell: **Electron** (easiest to run/validate in-container; simplest
  C++ addon path).
- Layout: **monorepo** — new top-level `desktop/` and `native/` directories in
  this repo, not separate repos.
</content>
