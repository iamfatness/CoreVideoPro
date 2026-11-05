# Graphics social provider follow-up plan

Use the [provider design](../specs/2026-11-04-graphics-social-provider-followups.md) and keep each adapter behind a separate review. Start with fixtures. A provider approval or credential check belongs to the controlled integration stage.

## Twitch · #1118

1. Write a scripted EventSub and Helix fixture for welcome, subscription confirmation, duplicate deliveries, single deletion, user clear, room clear and revocation. Add failed subscription, missed keepalive, planned migration and unexpected reconnect cases. Prove a late delivery cannot resurrect a cleared approval.
2. Implement the bounded Twitch transport and typed event mapping. Connect review only after the removal subscriptions are established. Verify selected channel and authorized user on each subscription. Add device-code and validation fixtures for denied consent, expiry and serialized credential rotation.
3. Wire the selected source and provider attribution through private review and the native card. Test two items with one approved, approval alone, Show, ordinary Take and generic control refusal. Observe the approved text and attribution on isolated Program. Inspect the dialog at 1360×768 and 1600×900.
4. Qualify the registered app and a controlled Twitch channel. Exercise a real deletion, user clear, room clear and revocation. Record local and hosted evidence in the Twitch PR.

## X · #1119

1. Verify the project's read access, budget and ability to detect deletion or loss of visibility. Choose the revalidation contract from observed provider behavior and document the display scope.
2. Fixture multiple rules, one selected rule, edit histories arriving out of order, duplicate lines, expiry, quota exhaustion and lookup failures. Reuse the same per-version approval and Program tests.
3. Build the scoped adapter and qualify a controlled rule and consenting test posts before enabling it in a beta.

## LinkedIn · #1120

1. Verify the project's Community Management access, page role, scope, supported API version and approved display use.
2. Fixture multiple comment pages, a missing page, an edit with the same comment identity, a confirmed deletion, loss of permission and version rejection. Require fresh per-item approval after an edit and fail closed on uncertain visibility.
3. Build the read-only adapter and qualify a controlled organization post before enabling it in a beta.

## Independent YouTube follow-up

[#1117](https://github.com/iamfatness/CoreVideoPro/issues/1117) records the controlled [operator check](../../reference/graphics-youtube-live-check.md). [#1121](https://github.com/iamfatness/CoreVideoPro/issues/1121) tracks expanded deterministic safety coverage that can run without provider credentials.
