# Native Build Agent Briefs

These documents hand off the CoreVideo Pro native build work to two coding
agents working in parallel. Read `00-native-build-plan.md` first for the full
context and architecture seam, then dispatch each agent with its brief.

- `00-native-build-plan.md` — the overall work plan, environment constraints,
  track split, coordination rules, and integration gate.
- `01-track-a-desktop-shell.md` — Agent A (Claude): Electron desktop shell +
  real main/renderer/native IPC bridge. Fully runnable in a Linux container
  with stubs.
- `02-track-b-native-media-core.md` — Agent B (Codex): C++20 native media-core
  process skeleton with stubbed, interface-isolated SDK adapters.

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
