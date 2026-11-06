# Controlled Twitch social review check

[#1118](https://github.com/iamfatness/CoreVideoPro/issues/1118) owns the
provider qualification. Use a registered public Twitch app, a controlled
channel and consenting test users. A public Client ID is sufficient; do not
supply a client secret. Keep access tokens, refresh tokens, device codes and
chat payloads out of logs and issue attachments. Use synthetic display text.
The automated fixtures do not replace this provider check.

Use a dev rig or an owner-controlled session, with an isolated show and no
production outputs. Record build SHA, Windows/GPU, channel authority used,
UTC test window and pass/fail observations under the designated QA evidence
folder. Do not change the owner's show or operate an installed app unattended.

1. Open Graphics → Operate → Social review. Select Twitch, enter the channel
   and public app Client ID, and Connect. Follow Twitch activation explicitly.
   Deny once and verify review stays disconnected. Connect again and consent
   to read-only chat access. Review opens only after all four required
   subscriptions have been confirmed.
2. Send two short synthetic messages. Select each and read the full author,
   text and Twitch attribution. Approve the first. Program must remain
   unchanged. The pending second item must have Show disabled.
3. Use Show approved on the first. Observe its author, complete approved
   message and Twitch live chat attribution on isolated Program. Use ordinary
   Take and attempt the ordinary generic controls against the reserved social
   instance; those paths must not promote a pending item or bypass review.
4. Delete the approved message in Twitch. It must leave review, lose Show
   eligibility and disappear from Program. Repeat with a message deleted while
   native preparation is pending. A late delivery must not restore its approval.
5. Approve and show a message from a controlled second user, then clear that
   user's messages using Twitch moderation. Their pre-clear messages must be
   removed and the on-air card retired. After the timeout ends, a new message
   may enter review pending; the old approval must remain unusable.
6. Approve and show another message, then clear the channel chat. Pre-clear
   items must leave review and Program. A newly sent message remains pending.
7. Disconnect the socket or network. Approval and the on-air card must clear.
   Reconnect requires new review. A planned server move has the same reset
   policy in this first adapter; it does not preserve approval continuity.
8. Revoke the app's access in Twitch. Confirm the required subscription's
   revocation closes review, clears approvals and retires the card. A later
   connection needs authorization again. Validate startup and an hourly check
   against Twitch separately from the deterministic rotation fixtures.
9. Change provider and change show. Verify old messages, selected revisions
   and callbacks cannot air. Clear local data and confirm the local protected
   Twitch credential file is removed. Reopening the panel must not restore
   chat content or decisions. Verify the native card's 15-second expiry with
   the console disconnected.

Inspect the complete review text and controls on a 1080p operator monitor,
and retain the 1360×768 and 1600×900 compiled layout captures separately.
Record each unexercised event as unverified. Keep the adapter PR draft until
real deletion, user clear, room clear and revocation are qualified. The
repository fixtures verify ordering and races; the native GPU test verifies
the two bundled cards, private cue, explicit Show, Take refusal and expiry.
