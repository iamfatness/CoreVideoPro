# CoreVideo Pro architecture remediation plan

Status: implementation and validation in progress on `codex/architecture-reliability`.

Implemented: Windows test gate, atomic production preference saves and recovery,
LAN control policy, generated lifecycle/operation/version contract slice, recording
lifecycle and shell adoption, asynchronous Zoom join with bounded input mailbox,
native route-binding policy, exact-candidate release evidence gate, and migration
documentation. The branch preserves compatibility with legacy protocol clients.

Remaining acceptance work: complete legacy generated protocol coverage,
durable participant identity and native atomic Take migration, outbound response
backpressure and client overload reconciliation, designated live rigs, macOS media
execution, two-hour soak, physical fault injection and clean-machine package proof.
See [ownership map](architecture-ownership.md), [contract inventory](../contracts/README.md)
and [migration notes](architecture-migration.md). These items are not implied complete
by the automated test results.

Baseline: architecture review of `660f6266f04f780fcb9e49dacfda409418e84852`. Refresh against current `main` before editing and revalidate each finding. Preserve intervening fixes. Use small `codex/` branches and reviewable PRs. The review checkout remains available at `review-source/`.

**Outcome**

Operators can trust recording/streaming status, execute time-sensitive commands during Zoom operations, recover saved production configuration, and upgrade across compatible protocol versions. Releases must carry evidence for the application and native adapters they actually ship.

Keep the C++ media core, Zoom subprocess, native shells, GPU transport, and existing adapter interfaces. Deliver bounded reliability changes first, then consolidate policy. No replacement UI framework or new always-running policy process is needed for this plan.

**Architecture decisions**

- The core owns observed media and output lifecycle state. Shells submit intent and present snapshots; a requested boolean never establishes that media is flowing.
- Separate a process epoch, operation ID, and output-session ID. Results from a terminated process or superseded operation cannot update the current session.
- Shared production policy belongs in a platform-neutral native module. Shells retain presentation state, user interaction, and platform integration. Inventory TypeScript show-engine behavior and migrate useful policy incrementally through shared scenario fixtures; do not immediately port the whole package.
- Use a versioned JSON Schema contract with checked-in generated models and deterministic generation. Prove generator suitability on a small vertical slice before adopting dependencies; retain JSON-line transport. Capability/version negotiation controls compatibility.
- Long SDK operations use workers and publish completion back to the serialized state executor. Do not parallelize arbitrary state mutations.
- Treat output activity, output health, and file finalization as distinct facts. A healthy remaining stream continues when another destination fails; the aggregate status becomes degraded.

**Phase 1 — immediate safeguards**

| PR | Work | Acceptance criteria |
|---|---|---|
| 1 — Windows test gate | Add `CoreVideoPro.WinUI.Tests` to Windows CI and root `test:gate`; capture test results as artifacts. | Existing MediaCore, Control, and WinUI suites execute; a deliberate failing WinUI test fails the gate; Windows publish still succeeds. Baseline from review: 395 MediaCore and 733 WinUI tests passed. |
| 2 — durable production saves | Introduce a narrowly scoped atomic JSON file store; write a same-directory temporary file, flush, replace, and preserve the last good backup. Serialize concurrent saves. Return distinct missing/corrupt/unreadable/recovered load outcomes. Preserve schema migrations and encrypted secret fields. | Inject interruption before replacement, truncated JSON, write failure, and interrupted migration. Previous configuration remains recoverable. First launch stays normal. Corruption is surfaced, not silently treated as an empty show. Backup never replaces a valid file with corrupt input. |
| 3 — LAN control policy | Validate bind address/authentication before starting HTTP/WS. Require a token for non-loopback binding; keep loopback compatibility. Reject unauthorized HTTP and WS requests before invoking actions. Separate OSC LAN enablement from the shared LAN switch. | LAN HTTP startup without a token fails with an actionable message; valid token succeeds; missing/wrong token fails; localhost remains functional. OSC remains local unless separately configured with an explicit trusted-network policy. Update Companion setup instructions. |

Primary files: `.github/workflows/ci.yml`, `package.json`, `ProductionOutputPreferencesStore.cs`, `HttpControlServer.cs`, `MainWindow.xaml.cs`, and related test projects.

For PR 3, a bearer token on plain HTTP is not protection against network interception. Document the trusted-network boundary; remote/untrusted-network operation requires TLS or an authenticated tunnel. Do not silently expose OSC when securing HTTP.

**Phase 2 — contracts and output truth**

**PR 4: contract foundation.** Add `contracts/` for schema, compatibility fixtures, and generation tooling. Begin with handshake, errors, asynchronous-operation envelopes, and output lifecycle. Generate C++, C#, TypeScript, and Swift representations for this slice. Specify wire names, numeric widths, optional/null behavior, unknown enum handling, and additive-field compatibility. Use immutable DTOs where practical.

Acceptance: all clients deserialize the same golden messages; invalid payloads fail with structured errors; supported older messages remain readable; an unsupported major version produces an explicit incompatibility response; regeneration produces no diff in CI. Existing messages continue through adapters while migration proceeds. Do not require a full-protocol rewrite before subsequent fixes.

**PR 5: authoritative core output lifecycle.** Introduce separate desired activity, actual lifecycle, and health per recording session and per stream destination. Model at least idle, starting, live, stopping, finalizing where applicable, completed, and failed. Keep degraded health separate from lifecycle. Replace optimistic encoder `active` as the source of observed truth. Carry process/session identity through queued encoder/sender work. Emit writer-start, progress, failure, and finalization outcomes.

Acceptance: delayed start remains starting; a failed writer cannot report live; startup failures and failures after acknowledgement remain visible; one failed stream cannot imply all destinations are healthy. A stop acknowledgement means accepted, while completion means the writer finalized. Stale callbacks cannot revive a stopped or newer session. Existing frame/audio backlog protections remain bounded.

**PR 6: shell adoption and recovery semantics.** Update Windows transport coordinator/view models, macOS model, and development clients to consume the lifecycle contract. Remove `actual || requested` from observed activity. Preserve desired state explicitly for reconciliation. Show per-destination status and finalization progress. Add compatibility adapters for old snapshots without presenting unknown activity as verified live.

Recovery rules: core restart marks prior output sessions interrupted; any resumed recording gets a new segment/session identity and an explicit continuity warning. A lost reply produces an unknown/reconciling state and snapshot query, not an unqualified success or blind replay. Identify retryable desired-state commands separately from edge-triggered operations such as Take.

Acceptance: UI behavior is tested with fake bridge event sequences covering delayed start, late failure, mixed destination success, stop/finalize, lost acknowledgement, and restart. A record/stop/restart sequence produces correctly identified, playable files with honest interruption reporting. Test legacy clients and new clients against supported core versions.

Dependencies: PR 4 precedes 5; PR 5 precedes 6. Keep the new contract additive until both shipping shells are migrated.

**Phase 3 — responsive command processing**

**PR 7: asynchronous Zoom lifecycle and bounded queues.** Build on PR 4's operation envelope. Join/authentication is accepted immediately, runs on the Zoom lifecycle worker, and reports progress/completion. Cancellation and leave invalidate the active operation; core shutdown waits only for a bounded teardown interval and safely disposes or terminates the subprocess when necessary. SDK calls obey its documented threading rules.

Keep state application serialized. Add a bounded mailbox: coalesce explicitly replaceable full-state syncs at enqueue time; retain ordering for output commands; reserve capacity for cancellation/stop; return explicit overload errors instead of silently dropping commands. Never classify replaceable work using substring matching. Ensure snapshot coalescing cannot discard an embedded one-time action.

Initial acceptance targets, to validate on the designated rig:

- During an injected 30-second join delay, Stop/Take acceptance p95 is at most 250 ms and maximum is below one second.
- Program rendering continues without a join-induced pause. Measure the existing frame-time baseline and reject regressions against it.
- Queue size and bytes stay within configured limits under sustained input; normal-state updates coalesce and overload is observable.
- Cancel/rejoin, SDK failure, process exit, and stale completion preserve current session state.
- A late accepted Take never executes twice after timeout/retry. Distinguish command acknowledgement from media-effect completion in the measurements.

Primary files: `native/src/rpc/JsonRpcServer.cpp`, Zoom runtime, native core command dispatch, and both shell supervisors. Do not hold `coreMutex` while waiting for a lifecycle worker or while joining it.

**Phase 4 — ownership and complete contract coverage**

**PRs 8a–8c: incremental consolidation.**

1. Create a written ownership map for show document/routing, source lifecycle, transport sessions, and UI presentation. Inventory current Windows/macOS/TypeScript rules and record intentional differences. Complete generated schema coverage message family by message family.
2. Extract transport/source coordination behind narrow interfaces; make coordinators constructible without launching the core. Replace the broad host interface gradually with focused capabilities. Preserve Windows UI-thread dispatch and macOS main-actor constraints.
3. Extract one shared native policy slice first: stable participant identity/routing and Take behavior. Establish golden scenarios for reconnect, missing guest, preview edit, Take, and active-speaker selection. Migrate both shells to that slice, then retire duplicate implementations only after usage searches and parity tests establish that they are unused.

Acceptance: both shells produce equivalent routing/output intent for shared scenarios; presentation-only differences remain local; behavior tests cover the extracted responsibility. Line-count reduction is not the acceptance metric. Do not combine broad class moves with untested behavior changes.

The development React/Node implementation remains a supported contract client and deterministic test harness. TypeScript show-engine features that are not yet shipping remain explicitly scoped as such until deliberately integrated.

**Phase 5 — release proof and documentation**

**PR 9: exact-commit release gates.** Refactor reusable CI validation so release jobs directly depend on required suites, rather than hoping that branch checks were run. Record source SHA, adapter/build flags, SDK/runtime versions, OS/GPU/driver, artifact hash, and test outcome. Tag releases must fail if required evidence is missing, failed, skipped without an allowed reason, or belongs to a different commit/configuration.

Add a dedicated hardware lane or an evidence-ingestion gate for controlled Windows rig tests. Start with:

- Real Zoom multi-participant ingest plus program and selected ISO recording and stream output.
- A two-hour simultaneous record/stream soak; inspect memory slope, frame drops, queue depths, command latency, and measured A/V drift. Establish numeric media-quality thresholds from the baseline before declaring pass/fail.
- Network interruption per destination, camera unplug/replug, core/Zoom process termination, disk exhaustion, and shutdown during finalization. Use fault injection where physical simulation is impractical.
- Open and decode output artifacts; verify expected streams, durations, timestamps, A/V alignment, and intelligible content. File existence or nonzero size is insufficient.
- Clean-machine installation/update and launch for the packaged configuration. Require macOS equivalents for macOS release artifacts.

Acceptance: the release cannot be published by its workflow without required automated and hardware evidence for that candidate. Archive logs, test results, media metadata, and configuration. Keep credentials and personal meeting content out of public artifacts.

**PR 10: documentation and rollout.** Update the architecture diagram, current native-UVC behavior, macOS status, actual RPC transport, output lifecycle meanings, LAN configuration, recovery behavior, and capability inventory. Derive compile-time capability facts from the build manifest; keep hardware verification as a separately dated evidence record. Publish a migration note for protocol and control clients.

Acceptance: a developer can build the documented configuration, an operator can distinguish starting/live/finalized, and a reviewer can trace every advertised verified capability to current evidence.

**Delivery order and rollback**

Execute PRs 1–3 first. Then 4 → 5 → 6 → 7 → 8 → 9 → 10. Prepare the hardware harness during earlier implementation so verification is not discovered at the end. The first release-workflow test dependency can land with PR 1; PR 9 adds the full media evidence gate.

For contract changes, use additive fields and capability negotiation before removing legacy fields. Preserve a known-good configuration backup before migrations. For behavioral changes, retain a narrow temporary compatibility path only when it remains truthful about output state. Do not roll back to optimistic success reporting or silently bypass mandatory release checks.

**Completion definition**

All seven architecture findings have implemented fixes or, for large ownership changes, the specifically scoped shared policy slice and explicit remaining ownership map. Generated contracts cover the supported command/snapshot surface. Windows and macOS clients pass shared lifecycle/routing fixtures. Release automation requires the relevant exact-commit tests and media evidence. No remaining issue may be called complete solely because unit tests pass; any unfinished consolidation is tracked explicitly rather than hidden behind this plan's completion.

Next review step: validate the combined branch and review the changes before merge;
provision the controlled release rig and finish the explicit migration acceptance items.
