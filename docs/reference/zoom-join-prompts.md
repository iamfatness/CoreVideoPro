# The SDK's join-time prompts must be ANSWERED (#475, 2026-09-12)

_Moved verbatim from `CLAUDE.md` (#737). Paths are relative to the repo root._

Live 2026-09-11: the owner's Zoom app was already in meeting 97682593786 on the
same account. CoreVideo joined at 08:44:43, the engine logged `ready` then
`auth_ok` then **nothing**, and 52 s later the shell gave up with "Timed out
waiting for Zoom meeting join result." Zoom red, Engine off, no reason.

**The engine implemented `IMeetingConfigurationEvent` not at all.** That interface
is how the SDK asks the APP to resolve join-time prompts, and it WAITS for an
answer. An unanswered prompt is not an error and produces no event — the join
simply hangs until our own timeout. Every prompt on it was unanswered; the
same-account collision (`onEndOtherMeetingToJoinMeetingNotification`) is just the
one that happened to fire. This is a ZComms talkback delivery law that was never
ported (`ZComms/src/zoom/zoom_client.cpp:526`).

`EngineConfigurationEvent` (`native/zoom-engine/engine/main.cpp`) now answers all
of them and reports a machine reason. Rules encoded there:

- **Answer on the callback thread, immediately.** The SDK destroys the handler
  after `EndOtherMeeting()` or `Cancel()`, so it CANNOT be held open while a
  dialog is put to the operator. The choice has to exist before the join is sent.
- **Never end the operator's own meeting by default.** `Cancel()`, then
  `join_failed` with `reason:"account-busy-elsewhere"`.
- **The takeover is EXPLICIT and PER JOIN.** `end_other_meeting` rides the join
  command (`ZoomEngineJoinCommand::endOtherMeeting` -> `"end_other_meeting":true`),
  read fresh on every join and never stored as engine state or a preference:
  ending the other session evicts the operator's own Zoom client, so it may only
  happen because they just asked. The shell offers it as a separate button
  ("Join and end my other Zoom session") that appears ONLY while
  `CanEndOtherZoomSession` is true, and that flag is cleared at the start of
  every join so it can never outlive the failure it answers.
- **A reason is wire vocabulary; operators must not read it.**
  `modules/ZoomJoinFailureMessage.h` is the ONE place it becomes English, so the
  two vocabularies cannot drift. An UNMAPPED reason passes through VERBATIM —
  mapping it to a generic sentence would recreate the silence this fixes.
- **The other prompts are answered too, loudly** (passcode/screen-name, webinar
  registration, webinar screen name, user info, expired-meeting recovery). Any
  new pure-virtual on that interface must be answered, not stubbed: a silent
  `{}` is a hang, not a no-op.

**Not a defect, do not "fix" it:** the engine process is still alive after a join
timeout. `ZoomEngineRuntime::retireTimedOutJoinLocked` quarantines it
(`restartBeforeJoin_`) deliberately and the next explicit join terminates it
outside the runtime lock. The comment at that site is the contract.

Tests: `ZoomEngineClient.TheTakeoverChoiceRidesTheJoinCommandAndDefaultsOff`,
`ZoomEngineRuntimeState.ASameAccountCollisionReadsAsOperatorLanguage` /
`AnUnmappedJoinFailureIsStillReported`, and
`ZoomJoinReconciliationTests.TheSameAccountCollisionIsOfferedTheTakeoverRetry`
(+ the every-other-failure and already-joined refusals). **The engine half has no
test** — it links the Zoom SDK and the prompt only fires against live Zoom, so
the callback itself is verified by a live join, not by CI.
