# THE LAW covers SLOTS, not just people (#506-follow-up, 2026-09-12)

_Moved verbatim from `CLAUDE.md` (#737). Paths are relative to the repo root._

The "sources keep reverting" family has a fourth member, and it is the same rule
one level up. From the owner's `launch.log`:

```
13:52:06  lifecycle: unassign slot 4 (was ZoomParticipant pid=16791552)   <- operator
13:52:06  slot-write: slot4 InShow 'true'->'false'  by=operator-unassign
14:00:03  slot-write: slot4 Kind 'Unassigned'->'ZoomParticipant' by=roster-sync
14:00:03  slot-write: slot4 ParticipantId ''->'33561600'          by=roster-sync
```

`ShowInputRosterService.SyncZoomParticipantSlots` fills **the first free slot**
with a newcomer — and a slot the operator deliberately emptied is the freest slot
there is. The existing memory (`_autoAssignSeenParticipantIds`) remembers the
PERSON the operator removed; nothing remembered the SLOT. So THE LAW held for the
participant and broke for the slot.

**The cost is not one wrong slot.** Every source-set change re-ranks the whole
resolution budget, so one phantom refill re-subscribes EVERY video source in the
meeting — a real engine-side renderer teardown each time. Measured on that
session: every camera at `churn` 4–12, `totalChurn` 77, every one's reason
`resolution-change`, which the owner saw as video flashes and guests dropping out
of the multiview and the Tiles wall. (#478's own fix is holding: 25 s across an
active-speaker flip moved churn not at all. Only source-set changes do this now.)

**Owner ruling (2026-09-12): a cleared slot is sticky until the MEETING ROSTER
EMPTIES.** `ShowInputsCoordinator._operatorClearedSlotNumbers` records it on the
one operator entry point (`UnassignShowInput`), `SyncZoomParticipantSlots` takes
it as `operatorClearedSlotNumbers` and skips those slots when filling, and it is
released when the roster goes empty (the meeting ending — the point the slot
layout stops meaning anything) or when the operator flips the auto-assign toggle,
which is an explicit "assign everyone now". It reserves against AUTO-assign only:
the operator may still place anything there, and a roster refresh never undoes
that.

**Two testing notes, both learned here:**

- **The leaf test proved nothing at first.** With a spare free slot earlier in the
  list the newcomer never wanted slot 4, so the test passed with and without the
  fix. It only became a real test once every slot was assigned, making the
  cleared slot the FIRST free one — the live shape.
- **The leaf test is not enough even when correct.** Dropping the
  `_operatorClearedSlotNumbers` argument at the CALL SITE leaves it green. The
  binding test is `ShowInputsCoordinatorTests.ARosterSyncNeverRefillsASlotTheOperatorUnassigned`,
  which drives unassign -> roster sync through the coordinator (the #481
  "test the whole decision, not the leaf" rule) and fails when the argument goes.
