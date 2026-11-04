# Graphics Phase 3 Slice 5 — implementation plan

Build [#1110](https://github.com/iamfatness/CoreVideoPro/issues/1110) against the [slice spec](../specs/2026-11-04-graphics-phase3-slice5-push-feeds.md). The planning PR can land independently. Start implementation from main once it includes the Slice 3 feed staging and publication path, or stack on PR #1105 with that dependency explicit. Keep work in task order. Record the red test, make it green, review the task diff, then commit.

## 1. Normalize complete snapshots and persist ordering

Start with failing MediaCore tests for version, integer bounds, duplicate properties, strict UTF-8, stable keys, schema mismatch and a valid sequence jump. Test newer identical rows, conflicting reuse, older replay after reopening, and document save failure. Confirm rejected data advances neither the watermark nor the dataset revision.

Reuse GraphicsLocalData and the Slice 3 typed schema and projection validation. Add an additive WSS feed format and a source-scoped watermark. In GraphicsWorkspaceLibrary.Feeds, commit the watermark and any new revision under the same show document write, using the existing generation fence. Make the watermark-only result distinguishable from a changed dataset, so acknowledgement can update freshness independently of live follow. Preserve loading and saving earlier shows and the three pull formats.

Commit **Validate and persist graphics WebSocket snapshots**.

## 2. Connect securely and bound receive work

Start with a fake resolver and connector and a controllable framed socket. Test denied destinations, mixed DNS, DNS rebinding at the dial seam, IPv4-mapped private addresses, untrusted certificates, redirects, compressed or binary input, fragmented messages, cancellation, oversized assemblies and receive-budget exhaustion across reconnects.

Extract only the address vetting and direct connection policy shared with GraphicsFeedTransport. Use ClientWebSocket.ConnectAsync with an explicitly configured HttpMessageInvoker and SocketsHttpHandler.ConnectCallback. Preserve the validated IP used by the socket and TLS verification against the requested hostname. Use one read loop, bounded assembly, and the shared cancellation lifetime. Exercise the actual upgrade and TLS validation with a controlled synthetic endpoint through a test-only address seam. Keep the production private-address rule active in production transport tests.

Commit **Read bounded graphics WebSocket messages**.

## 3. Reconnect and stage without replay

Write fake-clock tests for the initial accepted snapshot, identical replays, stale status, idle reconnect, backoff and manual reconnect floor. Add a slow sink with a burst, disable or switch shows during each await, and complete an old socket after replacement. Test sequence fences after reopening and when an in-flight commit fails. Confirm one staged write per cadence and a single replaceable waiting slot.

Add a focused shell connection coordinator beside GraphicsFeedPoller. Reuse TimeProvider and the existing show ownership in StudioViewModel.GraphicsFeeds. Keep socket and decoder work off the UI thread and marshal staging through the existing owner. Route accepted updates through GraphicsPlayoutService.Data and existing document writes. Distinguish read, validated, durably accepted and published progress. Verify live follow, manual publication, pinned Cue and looping and single-pass crawls with the existing test facade.

Commit **Reconnect and stage graphics push feeds safely**.

## 4. Expose the operator workflow and qualify the WebSocket adapter

Start with a red layout probe for WSS setup, Reconnect now, stale and retry status, and staged versus published data at 1360×768 and 1600×900. Add controls to the existing Remote feeds and Operate surfaces. Keep scope-specific exceptions in the existing user-facing status path. Verify the four-socket limit and independent polling sources.

Run the three shell suites and contract parity. Use a fresh report directory and inspect both screenshots, with a 1080p capture where available. Exercise the spec with a controlled WSS fixture, a synthetic table and crawl, and a development Program run. Verify existing native publication behavior through the shipped paths. Run exact native filters if needed for pixel or boundary evidence. If implementation changes native code, build it with the local SDK and run the full native suite once, at the end, with the installed app closed and isolated shared memory. Keep evidence under D:/CoreVideo-QA/tmp. Preserve the installed app, owner show files and OAuth broker configuration.

Update CLAUDE.md and docs/reference/graphics-playout.md. Review the complete diff and open a PR with test counts, flagged decisions, and any installed or public-provider checks still open.

Commit **Expose and qualify graphics WebSocket feeds**.

## Social follow-up

The first social adapter starts from a separate issue and provider-specific spec after the owner chooses the provider and approves the moderation policy. Its test-first plan covers provider authorization, message selection, explicit approval, retention, deletion and revocation. Qualify that provider end to end using the staged publication contract before adding another.
