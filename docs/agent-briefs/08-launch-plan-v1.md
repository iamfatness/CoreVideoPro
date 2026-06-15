# CoreVideo Pro — Road to Paid v1.0 Launch (3-Agent Parallel Plan)

> **Audience:** three coding agents working **simultaneously** — **Claude**, **Codex**, **Grok**.
> Read the whole doc once, then live in your own lane. The coordination rules in
> *§3* are what keep us from colliding.
>
> **Repo:** `iamfatness/CoreVideoPro` · **Branch:** `claude/wonderful-darwin-34tjph`
> · **Companion docs:** the rest of `docs/agent-briefs/` and `docs/roadmap/index.html`.

---

## 1. Context — why this plan exists

CoreVideo Pro is a Zoom-native live-production studio ("Pro Zoom productions in
minutes, not hours"). Today it is an **architecturally complete skeleton**: ~27k
lines of typed TypeScript, a full operator UI on simulated data, an Electron
shell with a typed IPC bridge + `MediaCoreSupervisor`, a C++20 native-core
skeleton with stubbed/dev-gated adapters, and a vendored OBS-free Zoom capture
engine. **Everything runs green in-container on stubs.**

What does *not* exist yet is the part users pay for: the **real native media
pipeline** (Zoom raw ingest → GPU compositor → hardware encoder → RTMP/file),
the **release machinery** (signing, notarization, auto-update, CI that actually
gates), and the **commerce layer** (accounts, licensing, billing, trial/
watermark enforcement) required for a *paid* launch.

The previous plan split work across two agents (Claude = desktop/renderer,
Codex = native core) on a demo-every-Friday cadence. **This plan adds a third
agent (Grok)** owning QA / CI / release hardening + commerce/ops infrastructure,
and re-points all three lanes at one goal:

> **Definition of done — Paid v1.0:** a non-engineer downloads a *signed,
> notarized* installer, signs in, pays/activates a license, joins a real Zoom
> meeting, clicks Magic Scene, runs a 4-person webinar with lower-thirds +
> captions + audio leveling, records a stable 1080p MP4 and/or streams to RTMP,
> and Set & Forget runs it hands-off — all meeting the MVP exit criteria in
> `COREVIDEO_PRO_PRODUCT_SPEC.md`, with crash reporting and auto-update live.

---

## 2. The three lanes

| Lane | Agent | Owns | Builds/validates on |
|---|---|---|---|
| **A — Desktop + Renderer** | **Claude** | Electron shell, renderer/UI integration, the **TS protocol files** (source of truth), capability-gated UI, onboarding/settings, **license + paywall UI** (behind a typed seam) | In-container (stubs) |
| **B — Native Media Core** | **Codex** | C++20 media core: Zoom capture, GPU compositor, HW encoder + recording, output senders, audio DSP, capture devices — real adapters behind `COREVIDEO_ENABLE_DEV_ADAPTERS` | Mac/Windows **dev machine** (CI stub build stays green) |
| **C — Release Eng / QA / Ops** | **Grok** | CI stabilization + native-build CI, contract-parity gates, Playwright-Electron e2e, **packaging/signing/notarization/auto-update**, crash + telemetry, **licensing + billing backend** (Supabase/Cloudflare/Stripe) + caption transcription broker | In-container + cloud (Supabase/CF) |

**Why this split is collision-free:** Codex never touches TS or renderer code;
Claude owns the typed contracts and UI; Grok works in `.github/`, `tests/e2e/`,
`desktop/` build config, and a **new `backend/` (or `services/`) directory** plus
cloud infra — not in `src/engine/` business logic or `native/` modules. The only
shared surfaces are (a) the **TS protocol files** (Claude writes, Codex mirrors,
Grok asserts parity) and (b) two **new typed seams** Claude defines for Grok:
the **license/account client** and the **caption broker client**.

---

## 3. Coordination rules (read these — they prevent merge pain)

1. **TS protocol files are the single source of truth.** Only **Claude** edits:
   - `src/engine/nativeBridgeProtocol.ts`
   - `src/engine/nativeMediaCoreProtocol.ts`
   - `src/engine/nativeMediaCoreCommands.ts`
   - `desktop/coreProtocol.ts`
   Land each protocol change in **its own commit** before either other agent
   builds against it. **Codex** mirrors them in `native/src/core/Protocol.h` and
   `native-core/src/protocol.ts`. **Grok** never edits them — only asserts them
   via `ContractParityTest` and e2e.
2. **Two new typed seams, owned by Claude, implemented by Grok:**
   - **License/account client** — a typed interface (e.g.
     `src/engine/licenseClient.ts`) the renderer calls; Grok backs it with the
     real Supabase/Stripe service and a Node stub for in-container tests.
   - **Caption broker client** — typed interface for streaming transcription;
     Grok backs it with the cloud broker; stub returns synthetic cues.
   Claude lands the interface + stub first; Grok implements the real service.
3. **Stub-first, real-later, always green.** The default in-container build must
   stay green on every commit: `npm run typecheck && npm run typecheck:desktop &&
   npm run test && npm run test:native-core`, plus
   `cmake -S native -B native/build -DCOREVIDEO_STUB=ON && cmake --build
   native/build && ctest --test-dir native/build`. All real SDK/GPU/encoder/
   billing code is config- or env-gated.
4. **Commit per increment; keep one draft PR per lane updated.** Reference the
   milestone IDs (A#, B#, C#) in commit messages.
5. **Dev-machine handoffs (Codex):** anything needing Zoom SDK / GPU / HW encoder
   / capture hardware can't be validated in CI — gate it, note it `// REQUIRES
   DEV MACHINE`, and validate on the Mac/Windows box.

---

## 4. Lane A — Claude (Desktop + Renderer)

**Baseline (built — do not redo):** Electron shell (`desktop/main.ts`,
`preload.ts`, `ipcRouter.ts`, `mediaCoreClient.ts` supervisor with crash-restart,
`coreStub.ts`), 6-family bridge protocol, fully typed Zoom media spine
(`src/engine/zoomMediaSpine*.ts`, `zoomSdkReadiness.ts`, `zoomWindowsSdkPackage.ts`),
full operator UI on simulated engines.

| ID | Milestone | Key files / reuse | Gate |
|---|---|---|---|
| **A1** | Wire the Zoom spine through the real native bridge (the key open seam). Add `zoom-media-spine-sync` to `desktop/coreProtocol.ts`, route it to the `MediaCoreSupervisor` child, and have `NativeHostZoomMediaSpineSyncEngine` use it instead of `buildFallbackZoomMediaSpineSnapshot()`. | `src/engine/zoomMediaSpineNativeSync.ts`, `desktop/ipcRouter.ts`, `desktop/coreProtocol.ts` | Join/sync round-trips Electron→main→core; renderer shows spine-sourced roster, not the fallback. |
| **A2** | Renderer drives `ZoomMediaSpineSessionController.joinProduction/syncProduction/leave` on join + tick. Add a pre-flight panel rendering `assessZoomSdkReadiness()` / `inspectZoomWindowsSdkPackage()`. | `src/engine/zoomMediaSpineSessionController.ts` | Readiness report renders; blocked states disable Join. |
| **A3** | Supervisor hardening + status. Surface `MediaCoreSupervisor` health (handshake timeout, restart count, `__crash`) into `describeRuntimeEnvironment`; add handshake timeout + bounded sync backpressure. | `desktop/mediaCoreClient.ts`, `src/engine/runtimeEnvironment.ts` | Killing the stub core shows recovering + auto-restart in the UI. |
| **A4** | Capability-gated UI. Read `profile.capabilities` and gate Phase-2 outputs (NDI/SRT/WebRTC/virtual cam) + paid-tier features so they only enable when announced. | `src/engine/nativeMediaCoreProtocol.ts` | A stub profile without `srt-output` hides/disables SRT arming. |
| **A5** | **License/account seam + paywall UI.** Define `src/engine/licenseClient.ts` (typed interface + Node stub), wire trial/watermark/720p gates and tier limits (participant count, captions, chroma) per the spec's pricing. Add sign-in + activation + "upgrade" flows. *Land the interface first for Grok.* | new `src/engine/licenseClient.ts`, `src/App.tsx` | Stub license states (trial/expired/Creator/Pro) gate features correctly; watermark + 720p cap apply on trial. |
| **A6** | **Onboarding + Settings.** First-run wizard (hardware/permission/encoder check), persistent settings (recording folder, stream presets, audio device, brand kit) via Electron IPC-backed store, error boundaries + user-friendly recovery UI. | `src/App.tsx` (Settings tab is currently stubbed), new settings store in `desktop/` | First run shows wizard; settings persist across restart; a thrown render error shows a recovery panel, not a white screen. |
| **A7** | **Caption broker seam.** Define typed caption-broker client + stub; wire real-time caption cues + style controls into the program canvas with adaptive placement. *Land interface for Grok.* | new `src/engine/captionBrokerClient.ts`, existing caption overlay engine | Stub cues render with attribution + adaptive placement; ready for Grok's real broker. |

**Verify:** `npm run typecheck && npm run typecheck:desktop && npm run test`
green; `npm run desktop` manual smoke.

---

## 5. Lane B — Codex (Native Media Core)

**Baseline (built — do not redo):** `native/` C++20 core (`CMakeLists.txt`,
`COREVIDEO_STUB` default ON, `COREVIDEO_ENABLE_DEV_ADAPTERS` default OFF),
`src/main.cpp` JSON-RPC stdio loop, `src/modules/Interfaces.h`, `StubModules.cpp`,
gated `PlatformAdapters.cpp`, `ContractParityTest`; Node mirror `native-core/`;
**vendored `native/zoom-engine/` (`corevideo-zoom-engine`)** — the **decided,
only active Zoom capture path** (see `06-decision-zoom-capture-path.md`).
The parked `ZoomMeetingSdkAdapter` stays building under its flag but gets no new work.

| ID | Milestone | Reuse (do NOT re-implement) | Gate |
|---|---|---|---|
| **B1** | Implement `zoom-media-spine-sync` (added by A1) in both C++ core and `native-core/src/zoomMediaSpine.ts`: consume `ZoomMediaSpineSyncPayload`, return `ZoomMediaSpineNativeSnapshot`. | `native-core/src/zoomMediaSpine.ts`, `native/src/core/Protocol.h` | Parity test covers the new request; stub round-trips with A's supervisor. |
| **B2** | **Live feeds (Sprint-1 north star).** Build `corevideo-zoom-engine` on dev machine (`-DCOREVIDEO_ENABLE_DEV_ADAPTERS=ON -DZOOM_SDK_DIR=…`). Frame path: open the engine's shm per subscribed participant → `tryReadFrame` → `i420ToRgbaThumbnail` → base64 → emit `zoom-video-frame` (~10–15fps). Map roster + active speaker. | `native/src/zoom/ShmFrameReader.h`, `native/src/zoom/I420Convert.h`, `desktop/coreProtocol.ts` `zoom-video-frame`, `src/ParticipantVideoCanvas.tsx`, `src/engine/zoomVideoFrames.ts` | ≥3 live participant tiles, <1s first frame, correct roster + active speaker against a real call. |
| **B3** | GPU compositor: implement `Compositor` consuming `RenderPlan` — D3D11 (Win) / Metal (mac). Announce `gpu-compositor`/`scene-graph-rendering`/`dynamic-overlays`/`chroma-key`/`smart-framing`. | `src/engine/nativeMediaCoreRenderPlan.ts` | Renders the scene graph for an integration Take on a dev GPU. |
| **B4** | HW encoder + recording: `Encoder` (NVENC/Quick Sync/VideoToolbox) + `RecordingSink` (real MP4/MOV). Announce `program-recording`/`iso-recording`. | encoder-session + recording-session cmds in `nativeMediaCoreCommands.ts`; `MediaFoundationEncoderAdapter.cpp` | Records a real, clean 1080p MP4 on a dev machine. |
| **B5** | Output senders: RTMP first (libavformat), then SRT/WebRTC/NDI. Announce matching `*-output` only when built. | `src/engine/{srtOutput,webrtcOutput,ndiOutput}.ts`, `RtmpOutputSenderAdapter.cpp` | RTMP push visible on a real platform (YouTube/Twitch). |
| **B6** | **Real audio DSP** (replaces the `AudioStub`): per-participant gain/leveling/limiter/noise-suppression + EBU R128 master, A/V sync. Delegate via the audio bridge commands. | `desktop/ipcRouter.ts` `AudioStub` (the seam it replaces), `AudioMixer` interface | Balanced program audio without manual riding; meter + limiter accurate. |
| **B7** | Capture devices: Blackmagic DeckLink + AJA NTV2 behind their own flags (`COREVIDEO_WITH_DECKLINK`/`_AJA`). *Stretch for v1.0 — one device is the target.* | `src/engine/captureDevices.ts`, `HardwareCaptureDeviceAdapters.cpp` | Enumerates + ingests one real device on a dev rig. |

**Ownership:** never edit TS protocol files — mirror them and keep
`ContractParityTest` green, extending it for every new request/response type.
**Verify:** in-container stub build + `npm run test:native-core` green;
dev-adapter builds validated on Mac/Windows.

---

## 6. Lane C — Grok (Release Eng / QA / Ops)

**Baseline:** CI today (`.github/workflows/ci.yml`) only runs typecheck + tests
on Linux; CodeQL is a stub. `desktop/electron-builder.yml` exists (NSIS/DMG/
AppImage targets) but **no signing, no notarization, no auto-update, no icons**
(`desktop/build/` doesn't exist), no commerce backend, captions are a mock stub.
The full vitest suite is **flaky/slow in the sandbox** — stabilize it before it
can gate.

| ID | Milestone | Key files / infra | Gate |
|---|---|---|---|
| **C1** | **Stabilize & segment the test suite.** Split fast unit vs slow UI/integration; fix timeouts; make `npm run test` a reliable gate. Add the **native stub build + ctest** and **`test:native-core`** to CI. | `.github/workflows/ci.yml`, `vite.config.ts` | CI green & deterministic on 3 consecutive runs; native stub build gated in CI. |
| **C2** | **Contract-parity + e2e gates.** Wire `ContractParityTest` into CI; author the Playwright-Electron smoke (`playwright.config.ts` exists, no tests yet) asserting join→Magic Scene→Take→core-ack on the stub core. | `tests/e2e/`, `playwright.config.ts` | Smoke passes in CI headless; protocol drift fails the build. |
| **C3** | **Packaging, signing, notarization.** Add `desktop/build/` icons (`.icns`/`.ico`/`.png`) + NSIS/DMG branding; wire `electron-builder` code-signing (`CSC_LINK`, `APPLE_ID`, etc. via CI secrets) + macOS notarization; produce signed mac/Win artifacts in CI. | `desktop/electron-builder.yml`, `desktop/scripts/launch.mjs`, new release workflow | Signed, notarized installers build in CI; Gatekeeper/SmartScreen clean on a real machine. |
| **C4** | **Auto-update + release automation.** Integrate `electron-updater` with signature verification + an update feed; version-bump + changelog + GitHub Release artifact upload automation. | new `.github/workflows/release.yml`, `CHANGELOG.md` | A v0.x→v0.x+1 delta update installs end-to-end; tagging a release publishes signed artifacts. |
| **C5** | **Crash + telemetry.** Integrate crash/error reporting (Sentry or equivalent) in main + renderer; wire the existing `supportBundle.ts` to an upload endpoint; minimal usage/feature + performance telemetry behind a consent toggle. | `src/engine/supportBundle.ts`, `desktop/main.ts` | A forced crash produces an uploaded report with a redacted bundle; telemetry respects opt-out. |
| **C6** | **Licensing + billing backend** (paid v1.0). Stand up Supabase (accounts, license/entitlement table) + Stripe (Creator/Pro/Team tiers + 14-day trial) + license verification endpoint, behind **Claude's `licenseClient.ts` seam** (§3). Keep the in-container Node stub authoritative for tests. | new `backend/` or `services/`, Supabase + Stripe + Cloudflare | Trial issuance, purchase, activation, and entitlement check work end-to-end against staging; renderer gates match real entitlements. |
| **C7** | **Caption transcription broker** behind **Claude's `captionBrokerClient.ts` seam** (§3). Cloud streaming transcription (cloud-first per spec) with attribution; usable real-time latency. | new caption service (Cloudflare Worker / Supabase Edge), `captionBrokerClient.ts` | Live program audio yields captions with usable latency + attribution in the app. |

**Verify:** CI green + deterministic; signed artifacts produced; staging billing
+ caption services reachable; all gated so the default in-container build stays
green without any cloud credentials.

---

## 7. Unified sprint timeline (demo every Friday)

Each sprint has **one demoable goal**. Lanes run in parallel; the **owner** drives
the demo. Earlier sprints front-load the critical path (native pipeline +
release machinery); commerce lands once the pipeline is provable.

| Sprint | Demo (the Friday proof) | A — Claude | B — Codex | C — Grok |
|---|---|---|---|---|
| **1** | Join a real Zoom call, see ≥3 live participant tiles + correct roster/active-speaker | A1, A2 | **B1, B2** | C1 |
| **2** | Build a 2–3 person show live; cut grid / speaker-focus / PIP; lower-thirds from real names | A2, A3 | (B2 polish) | C2 |
| **3** | Record a real 1080p MP4 and/or go live to RTMP | A4 | **B3, B4, B5** | C3 |
| **4** | Balanced per-participant audio + real-time captions on the program | A7 | **B6** | C7 |
| **5** | Auto-produced show: Set & Forget + screen-share switching + brand kit, hands-off | A5 (license gates), A6 | (B7 stretch) | C5 |
| **6** | Install the **signed** app; sign in, **activate a paid license**, run a full show end-to-end | A5, A6 | hardening | **C4, C6** |
| **7 (launch hardening)** | Non-engineer runs a paid show from a notarized install with crash reporting + auto-update live | polish | dev-machine validation of all MVP exit criteria | C4, C5, C6, C7 final |

---

## 8. v1.0 launch checklist (exit criteria)

Tracks the spec's MVP exit criteria **plus** the paid-launch additions:

- [ ] Join Zoom → clean participant feeds (B2)
- [ ] Magic Scene → polished show in <10 min (A + B3)
- [ ] 4-person webinar: screen share + lower-thirds + captions + leveling (B3/B6/C7)
- [ ] Stable 1080p MP4 recording (B4)
- [ ] RTMP stream to a real platform (B5)
- [ ] Set & Forget runs a show unattended; manual override always wins (A5 — logic already built)
- [ ] No OBS dependency (already true)
- [ ] **Signed + notarized installers, mac + Windows** (C3)
- [ ] **Auto-update live** (C4)
- [ ] **Crash reporting + diagnostics upload** (C5)
- [ ] **Accounts + licensing + Stripe billing; trial/watermark/720p + tier gates enforced** (A5 + C6)
- [ ] **Onboarding wizard + persistent settings + error recovery UI** (A6)
- [ ] **CI green & deterministic; e2e smoke + contract parity gate every PR** (C1, C2)

---

## 9. Verification (anyone can run the in-container gate)

```bash
# Renderer + desktop + native (stub) — must be green on every commit, all lanes
npm run typecheck && npm run typecheck:desktop && npm run test && npm run test:native-core
cmake -S native -B native/build -DCOREVIDEO_STUB=ON && cmake --build native/build && ctest --test-dir native/build

# Desktop smoke (Lane A)
npm run desktop        # Electron launches, renderer shows native runtime status (not "Mock studio")

# E2E + CI (Lane C)
npx playwright test    # join → Magic Scene → Take → core-ack round-trip on the stub core

# Dev-machine only (Lane B) — real pipeline
cmake -S native -B native/build-dev -DCOREVIDEO_ENABLE_DEV_ADAPTERS=ON -DZOOM_SDK_DIR=… && cmake --build native/build-dev
```

**Dev-machine + cloud handoffs** (cannot run in CI, validate manually):
Zoom SDK raw-data privilege + real meeting (B2); GPU/encoder on real hardware
(B3/B4/B5); signing certs + notarization (C3); Supabase/Stripe staging (C6);
caption broker latency (C7).

---

## 10. Open decisions to confirm before/early in execution

1. **Zoom raw-data privilege** must be enabled on the Zoom app or B2 yields empty
   video — confirm before Sprint 1 (surface immediately if frames are empty).
2. **Code-signing identities** — Apple Developer ID + Windows EV cert must be
   provisioned and added to CI secrets for C3.
3. **Stripe + Supabase project/org** for C6 — confirm which org/project to use;
   ask before creating billable resources.
4. **Caption provider** (C7) — cloud transcription vendor choice (cost/latency).

---

## 11. Dispatch

Ready-to-paste kickoff prompts live in `09-launch-dispatch-prompts.md`. Each
points an agent at its lane (§4 / §5 / §6) and the coordination rules (§3).
