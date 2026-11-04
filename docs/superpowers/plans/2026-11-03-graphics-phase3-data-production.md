# Graphics Phase 3 — implementation plan

Requirements are in the [Phase 3 spec](../specs/2026-11-03-graphics-phase3-data-production.md). The [ranked backlog](../../BACKLOG.md) determines when implementation starts. Keep Phase 2 installed acceptance separate. Land each implementation slice through its own review with the relevant issue, test-first evidence and a real operator entry point.

## Slice 1 — local data and native table

1. Start with a failing import-to-Show fixture for a keyed CSV leaderboard. Include an older show document and a malformed or oversized import. Verify deterministic ties and typed sort.
2. Add show-scoped source definitions, bounded CSV/JSON parsing and immutable validated revisions. Reuse the existing graphics document transaction and design pin lifecycle. Keep the previous accepted revision on parse failure.
3. Add the first native table component, including header, row styles, column bounds and overflow. Test native pixels, resource admission and the ability to replace a ready table at a frame boundary without replaying entrance motion.
4. Wire local import and binding in Design, then Cue, Show, Publish and Hide in Operate. Use the pinned version through reinstall and re-show. Verify the full saved-document path and capture isolated UI layouts at 1360×768 and 1600×900.

First deliverable: an operator can import a local scoreboard and show a prepared table or leaderboard. No remote account is needed.

## Slice 2 — crawl and safe publication

1. Add injected-clock tests for cue versus Program, clip edges, direction, pause/resume, loop boundaries and explicit restart.
2. Implement the bounded native crawl using existing text preparation and tick-driven offsets. Avoid per-frame text raster allocation.
3. Add operator controls, explicit live follow and per-revision state. Exercise simultaneous old/new preparations, late results and removal while a crawl is playing.
4. Verify Program and NDI alpha match on a controlled fixture. Record the initial frame and resource budget at 1080p.

Depends on Slice 1 revision and publication contracts.

## Slice 3 — remote pull data

1. Build the adapter contract against a deterministic transport and clock. Cover cancellation, request and decode limits, credential redaction, URL/redirect policy, stale results and capped backoff.
2. Implement the first authenticated or explicitly published Google Sheets read path as agreed for the target show. Add RSS and REST/JSON through the same contract with format-specific parsing tests.
3. Surface disabled, refreshing, fresh, stale and rejected states and the accepted data age in the existing operator workflow. Test connectivity loss, a newer reply overtaking an older one and explicit recovery.
4. Verify one real provider in an isolated profile and record which credentials and network conditions were exercised.

Depends on Slice 1. Start with a documented provider authentication decision before adding provider-specific dependencies.

## Slice 4 — data-aware shot layouts

1. Test a shot layout with two data-bound rows and one invalid pin. Assert Program is unchanged on refusal and Take waits until every required cue is ready.
2. Extend the existing ShotLayout document and cue path for typed value overrides and revision selection. Preserve older layouts, hotkeys and control action IDs.
3. Add the editor and an inspectable preflight result. Verify cancellation and retries after a lost response. Trace cue preparation to the same Program Take used today.

Depends on Slice 1. Remote feeds can use the same revision contract when Slice 3 is available. Audio actions are separate scope.

## Slice 5 — push and social adapters

The [Slice 5 spec](../specs/2026-11-04-graphics-phase3-slice5-push-feeds.md) and [test-first plan](2026-11-04-graphics-phase3-slice5-push-feeds.md) detail custom HTTPS pull, WebSocket push, and the social qualification gate for [#1110](https://github.com/iamfatness/CoreVideoPro/issues/1110).

1. Add bounded WebSocket ingestion with reconnect, message ordering, cancellation and batch coalescing tests.
2. Qualify a single social provider end to end before adding another. Specify its auth, message selection and retention policy separately. Treat external text as data.
3. Test repeat disconnects and large bursts with a live graphic already on Program. Use the Slice 3 status and last-good behavior.

Depends on Slice 3. Provider choice and moderation policy need owner input when this slice is scheduled.

## Slice 6 — HTML isolation and production qualification

1. Build a contained HTML transparency and resource-budget spike with a local fixture. Gate HTML implementation on isolation, alpha, process restart and frame provenance. Publish a measured spike result even if the integration is deferred.
2. Run the 1080p output and NDI receiver workload for the shipped data graphics. Repeat at 4K on supported hardware. Record frame continuity, preparation, GPU and memory data before proposing higher limits.
3. Qualify SDI only with a supported device and frame-matched fill/key capture. Keep its evidence separate from NDI and from the HTML result.

HTML and SDI can advance independently of the native data feature release when their gates are met.

## Verification and handoff

- Start each implementation task with a reproducible red test. Use exact native filters and the injected clock for timing. Review the diff against the spec before moving on.
- Run MediaCore, Control and WinUI suites, a D3D11 native build and targeted native pixel, contract and concurrency tests. Run the full native suite at the end of a native slice only with the installed app closed. Use an isolated virtual camera shared-memory directory.
- Use isolated shows, synthetic feeds and separate report directories under `D:\CoreVideo-QA\tmp`. Inspect both required viewport screenshots. Preserve the installed app, owner show files and OAuth broker configuration. No focus stealing or meeting URLs in evidence.
- Update `CLAUDE.md` and the operator reference with each shipped capability. Each PR records green and red evidence, flagged decisions, and installed acceptance that remains open.
