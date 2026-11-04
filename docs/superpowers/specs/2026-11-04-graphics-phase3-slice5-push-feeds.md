# Graphics Phase 3 Slice 5 — WebSocket and social feeds

Tracks [#1110](https://github.com/iamfatness/CoreVideoPro/issues/1110) and extends the [Phase 3 design](2026-11-03-graphics-phase3-data-production.md). Build the WebSocket adapter on the Slice 3 staging and publication contract in [PR #1105](https://github.com/iamfatness/CoreVideoPro/pull/1105). A compatible WebSocket producer is the first operator workflow. Social adapters qualify separately once a provider and moderation policy are selected.

## Operator workflow

In **Graphics → Design → Remote feeds**, add a **WebSocket snapshots** source, enter a public WSS endpoint and a stable key column, and enable the source. Use the accepted data preview to create a table, leaderboard, or crawl. Operate shows connection state, the last accepted check, data age, the next retry, and the staged, published, and Program revisions. **Reconnect now** requests a new connection within the retry floor. Poll-based sources continue to offer **Refresh now**.

⚑ The source starts disabled. The operator publishes a staged change explicitly or opts a playlist row into live follow. Use the existing prepared native publication path. A table changes on a frame boundary, a looping crawl at its loop boundary, and a single-pass crawl on its next Show or Cue. A prepared Cue retains its captured revision through Take. A disconnect or rejected message leaves the last saved data and Program content available. Socket work stays in the shell.

## Snapshot protocol

⚑ Accept complete snapshots from a producer that supports the CoreVideo envelope. Every WebSocket text message is one JSON object with exactly three properties. The producer sends a full snapshot at connection start, on a data change, and repeats its current snapshot at least every 10 seconds during idle periods.

| Property | Meaning |
| --- | --- |
| schemaVersion | Integer 1. |
| sequence | Positive integer through 9,007,199,254,740,991. The producer maintains it across reconnects and restarts and increments it when data changes. |
| rows | Nonempty, flat JSON row array in publisher order. Use the Slice 3 text, number, Boolean and stable-key rules. |

Example snapshot.

    {"schemaVersion":1,"sequence":41,"rows":[{"id":"a","name":"Ada","points":12}]}

A complete snapshot can skip sequence numbers. Reject malformed envelopes, duplicate properties, invalid UTF-8, duplicate row keys, incompatible types and schema changes as a whole. Treat external text as literal data. The first accepted snapshot establishes the source schema. Bound each message at 1 MiB of assembled UTF-8, 1,000 rows, 32 columns, and 128 receive completions. A fragmented message is admitted only after its last part arrives within a 10-second assembly deadline.

⚑ A source keeps a durable sequence watermark beside its last accepted dataset. Lower sequences are ignored and leave freshness unchanged. An identical replay of the current sequence confirms freshness. Reusing a sequence with different normalized rows rejects the message. A higher sequence with identical rows advances the watermark without creating a dataset revision. Persist a watermark advance and any corresponding dataset change atomically before acknowledging it in the UI or publishing. Keep the latest valid waiting snapshot per source and start at most one staging commit per source per second. A newer snapshot may replace the waiting slot while a commit is in flight. On reconnect, restart ordering from the durable watermark and discard the old socket's waiting snapshot. Use a new source when changing the endpoint, stable key, or producer sequence identity after the first accepted snapshot.

## Transport and lifecycle

⚑ Connect to public WSS on port 443 with normal TLS hostname and certificate validation. Use Slice 3's public-address policy at the dial actually used by the socket. Reject mixed public/private DNS answers and resolve again on each connection. Decline redirects, proxies, cookies, compression, credentials, query strings, and URL fragments in this adapter. Use one receive loop per source and admit at most four active WebSocket sources in a show. Reject an enable that exceeds this limit and identify the existing enabled sources. Keep the existing show-level feed limit.

Set a 10-second connection deadline. Apply a receive budget per source across reconnects of 2 MiB and 20 completed messages per rolling second. Count discarded messages and fragmented bytes against the budget. Close an over-budget, oversized, binary, or stalled transport and enter retry. A malformed but bounded text snapshot is rejected and surfaced without closing a healthy transport. Mark accepted data stale after 30 seconds without an accepted check, and reconnect after 60 seconds without one, including when the socket is open. Back off exponentially from 5 seconds to 5 minutes. Reset the backoff after a valid accepted snapshot or identical replay, and rate-limit manual reconnects to the same 5-second floor.

Use the current show lifetime, feed definition generation and connection identity to fence pending work. Disabling, editing, deleting, switching shows or shutdown cancels pending connect, receive, decode, retry and staging work. Recheck identity at the document commit and before live follow. A commit completed before invalidation remains the last saved dataset. Keep diagnostics to feed IDs, elapsed time, counts and sanitized outcome categories. Keep endpoints, credentials, provider errors and payloads out of diagnostics. A test-only connection seam permits local TLS fixtures.

## Social qualification gate

Before adding a social adapter, record the owner-selected provider, intended content, permitted authorization and scopes, selection and moderation workflow, retention and deletion rules, and a test account or fixture. Keep provider authorization separate from anonymous WSS. The first social adapter uses explicit operator approval before external messages can reach Program. Any unattended publication of social content needs a separate owner ruling. Qualify one provider and its revoke, disconnect, moderation, and recovery behavior before adding another.

## Acceptance

- Stage a local synthetic scoreboard and crawl from a controlled WSS producer. Verify a fragmented snapshot, a burst, and duplicate or stale sequences. Show that only accepted snapshots can create dataset revisions and that an outage leaves Program on the last published revision.
- Reconnect during a burst, deliver an old-socket completion after the replacement connects, then deliver a newer snapshot. Confirm freshness, sequence and publication behavior through manual Publish and opt-in live follow. Reopen the show and confirm the durable watermark rejects an older snapshot.
- Exercise a looping and a single-pass crawl and a pinned Cue alongside live-follow updates. Verify the existing boundaries and the first frame of a table revision. Confirm disabled startup, cancellation, and independent pull-feed operation.
- Use a controlled TLS fixture for the protocol and certificate path. Record installed-show and any public-provider checks separately.

[WebSocket protocol](https://www.rfc-editor.org/rfc/rfc6455) · [.NET ClientWebSocket connection API](https://learn.microsoft.com/en-us/dotnet/api/system.net.websockets.clientwebsocket.connectasync?view=net-9.0)
