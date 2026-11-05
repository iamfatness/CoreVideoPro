# Graphics social provider follow-ups

The [Slice 6 design](2026-11-04-graphics-phase3-slice6-social-feeds.md) defines the operator workflow and Program gate. Apply it to each additional provider. [#1118](https://github.com/iamfatness/CoreVideoPro/issues/1118) tracks Twitch. [#1119](https://github.com/iamfatness/CoreVideoPro/issues/1119) and [#1120](https://github.com/iamfatness/CoreVideoPro/issues/1120) track X and LinkedIn. The [YouTube qualification](../../reference/graphics-youtube-live-check.md) can run separately when the owner has a stream.

## Common behavior

Every source is selected and connected by the operator. Bind incoming plain text to the provider, selected source generation, stable item identity and exact displayed version. Show the author, text and provider attribution. Each item enters private review. Approval authorizes one version for a separate **Show approved** action. A change to displayed data clears its selection and approval. Removal, lost authority and uncertain event continuity revoke eligibility and retire affected Program output. Generic graphics controls, hotkeys, Shot Layouts and ordinary Take cannot air social items.

Use the existing private review and playout owner. Add provider attribution to the approved item and its native card only when the first additional adapter is wired. Keep content and decisions session-only. Bound response bytes, item counts, deduplication and reconnect work. Keep each credential local and scoped to its provider. Preserve a separate live acceptance record for each adapter. Provider credentials, approvals and test accounts determine which adapter is ready to qualify.

## Twitch chat

Start with plain-text messages from one selected channel. Prefer a registered public Twitch app with a device authorization flow and the `user:read:chat` user scope, subject to a controlled credential test. Validate the token and the authorized user at startup and hourly. Serialize token refresh and atomically replace rotated credentials. Use Helix to resolve the broadcaster and register the four required subscriptions against the EventSub WebSocket session. Require message, message delete, clear by user and clear room subscriptions before enabling review. [OAuth](https://dev.twitch.tv/docs/authentication/getting-tokens-oauth/#device-code-grant-flow) · [Token validation](https://dev.twitch.tv/docs/authentication/validate-tokens/) · [Event types](https://dev.twitch.tv/docs/eventsub/eventsub-subscription-types/)

Use the channel and message ID for review identity. Deduplicate delivery separately by the EventSub message ID. A single deletion withdraws its target. A user clear retires that user's pre-clear items. A room clear retires all pre-clear items. Keep a bounded ordering fence so an overlapping delivery cannot restore a cleared item. An unplanned disconnect can lose events and therefore invalidates the review generation. A provider-directed socket migration may preserve eligibility only if the replacement is verified and the subscription continuity is proven. Otherwise begin a new generation. Revocation or loss of a required moderation subscription stops Show. [WebSocket lifecycle](https://dev.twitch.tv/docs/eventsub/handling-websocket-events/)

### Twitch implementation decisions for owner review

The first adapter clears review eligibility during planned server migration
and reconnects to the canonical endpoint with fresh subscriptions. Continuity
preservation is deferred until it is proven. The first on-air Twitch card is a
separate bundled v1 template with fixed provider attribution, retaining the
existing YouTube template unchanged. The socket caps a complete text message at
64 KiB, deduplicates 4096 deliveries and permits three reconnect attempts.
The shared review bounds remain 200 items, 15-minute expiry and 4096 removal
or per-user ordering entries; overflow ends that generation. These are
conservative first-adapter choices, subject to owner review.

## X posts

Start with one selected, existing filtered-stream rule on an authorized project. Confirm project access, quota controls and an approved budget before any live test. Filter incoming events by the selected rule ID even if the provider connection carries multiple rules. Use the original post in the edit chain as the stable identity and the latest post ID as the provider version. An older version arriving out of order cannot replace a newer reviewed version. Verify latest visibility and version before Show. Connect a supported deletion and visibility change feed or establish a documented bounded revalidation path before offering the adapter. Stop on loss of that moderation evidence. Use the provider's display requirements as a separate release gate. [Filtered Stream](https://docs.x.com/x-api/posts/filtered-stream/introduction) · [Post Lookup](https://docs.x.com/x-api/posts/lookup/introduction) · [Compliance streams](https://docs.x.com/x-api/compliance/streams/introduction)

## LinkedIn organization comments

Start with top-level text comments on one selected organization post. Confirm the organization's Community Management API approval, the authenticated member's eligible page role, and the current comments read scope. Use a supported version header and the selected post's qualified URN. Keep the first adapter read-only. Prefer one bounded complete scan of comments. A partial scan or failed page cannot establish absence. Verify a missing approved comment by direct lookup, and revalidate the exact comment and revision before Show. Fail closed on role, scope or version rejection. Qualify display and removal behavior with a controlled organization post before public distribution. [Comments API](https://learn.microsoft.com/en-us/linkedin/marketing/community-management/shares/comments-api) · [App review](https://learn.microsoft.com/en-us/linkedin/marketing/community-management-app-review)
