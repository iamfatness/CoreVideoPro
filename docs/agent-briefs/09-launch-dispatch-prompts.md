# Launch Dispatch Prompts (Paid v1.0)

Ready-to-paste kickoff prompts for the three parallel agents driving the
**Paid v1.0 launch** plan in `08-launch-plan-v1.md`. Each prompt inlines the key
constraints so the agent can start without further context. All three work the
same branch and must keep the default in-container build green on every commit
(see the coordination rules in `08-launch-plan-v1.md` §3).

---

## Claude — Lane A (Desktop + Renderer)

> You are working in the `iamfatness/CoreVideoPro` repo on branch
> `claude/wonderful-darwin-34tjph`. Read **`docs/agent-briefs/08-launch-plan-v1.md`**
> — you are **Lane A** (§4). Work milestones **A1→A7 in order**, starting with
> **A1** (wire the Zoom media spine through the real native bridge).
>
> You **own** the TS protocol files (`src/engine/nativeBridgeProtocol.ts`,
> `nativeMediaCoreProtocol.ts`, `nativeMediaCoreCommands.ts`) and
> `desktop/coreProtocol.ts` — land each protocol change in its own commit so
> Codex can mirror it. You also define two typed seams for Grok and land their
> interface + Node stub first: `src/engine/licenseClient.ts` (A5) and
> `src/engine/captionBrokerClient.ts` (A7).
>
> Keep `npm run typecheck && npm run typecheck:desktop && npm run test` green
> in-container with stubs. Commit per milestone; keep a draft PR updated.

---

## Codex — Lane B (Native Media Core)

> You are working in the `iamfatness/CoreVideoPro` repo on branch
> `claude/wonderful-darwin-34tjph`. Read **`docs/agent-briefs/08-launch-plan-v1.md`**
> — you are **Lane B** (§5). Work milestones **B1→B7 in order**, starting with
> **B1** (`zoom-media-spine-sync` handler), then **B2** (live feeds via the
> vendored `corevideo-zoom-engine` — the decided capture path, see
> `06-decision-zoom-capture-path.md`).
>
> **Never edit the TS protocol files** — mirror them in `native/src/core/Protocol.h`
> and `native-core/src/protocol.ts`, and keep `ContractParityTest` green, extending
> it for every new request/response type. All real SDK/GPU/encoder code stays
> behind `COREVIDEO_ENABLE_DEV_ADAPTERS` so the default build is stub-only and
> green. Verify with `cmake -S native -B native/build -DCOREVIDEO_STUB=ON &&
> cmake --build native/build && ctest --test-dir native/build` and
> `npm run test:native-core`. Dev-adapter work is validated on a Mac/Windows box.
> Commit per milestone; keep a draft PR updated.

---

## Grok — Lane C (Release Eng / QA / Ops)

> You are working in the `iamfatness/CoreVideoPro` repo on branch
> `claude/wonderful-darwin-34tjph`. Read **`docs/agent-briefs/08-launch-plan-v1.md`**
> — you are **Lane C** (§6). Work milestones **C1→C7**, starting with **C1**
> (stabilize + segment the test suite and add the native stub build + native-core
> tests to CI) and **C2** (contract-parity + Playwright-Electron e2e gates).
>
> You work in `.github/`, `tests/e2e/`, `desktop/` build config, and a **new
> `backend/` (or `services/`) directory** plus cloud infra (Supabase / Cloudflare
> / Stripe). **Do not edit the TS protocol files or `src/engine/` business logic
> or `native/` modules** — consume the typed seams Claude lands
> (`licenseClient.ts` for C6, `captionBrokerClient.ts` for C7). Everything must be
> env-gated so the default in-container build stays green with **no** cloud
> credentials. Ask before creating billable cloud resources. Commit per milestone;
> keep a draft PR updated.
