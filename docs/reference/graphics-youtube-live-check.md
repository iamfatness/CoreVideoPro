# Controlled YouTube chat acceptance

Use [#1117](https://github.com/iamfatness/CoreVideoPro/issues/1117) for the result and [#1116](https://github.com/iamfatness/CoreVideoPro/pull/1116) for the candidate. Use a candidate build that contains Social review, an isolated show with local Program monitoring, and one public test livestream you control. Use synthetic messages from consenting accounts. Enter an API-restricted YouTube Data API key directly into CoreVideo. The current live-drag beta predates Social review.

1. Open **Graphics → Operate → Social review**. Enter the 11-character video ID and connect. Verify the status identifies the intended stream.
2. From YouTube, post two distinct messages such as **Test A — approved** and **Test B — pending**. Wait for both to appear. Select B and check that Show is unavailable. Select A, read the author and text, approve it, and confirm Program stays clear.
3. Select **Show approved** for A. Verify the author, exact text and YouTube attribution in the local Program picture. Confirm B stays private. Watch for the 15-second card expiry.
4. Delete A as a moderator or retract it from its posting account. Keep the session connected. Wait for the provider poll and verify A disappears from review and cannot be shown again. If the native timer takes the card off before the poll, record the timer and provider removal separately. That observation verifies withdrawal of eligibility. To verify early removal from Program, repeat with a fresh message and place the deletion shortly before the next poll while the card is still visible.
5. Repeat with a fresh item and disconnect while it is shown. Check the card clears and the queue empties. Reconnect and confirm any replayed items require fresh approval. Use **Clear local data** when done if you want to remove the saved key.

Record candidate SHA, observed outcomes and approximate poll and removal times in #1117. Keep evidence to synthetic content and leave credentials out of screenshots and issue text. If a provider event does not arrive, record the event type, connection state and elapsed time. The deterministic fixture and the live result are separate evidence.
