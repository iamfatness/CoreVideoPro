# A Take is traceable, and "what Program rendered" no longer lies (2026-09-10)

_Moved verbatim from `CLAUDE.md` (#737). Paths are relative to the repo root._

Three things, all core-side, all born from the same live show. The first is a
correctness fix; the other two are instruments, deliberately built before any
further fix, because three of that night's wrong conclusions came from a
measurement rather than from the product.

- **THE RENDERED SCENE ID WAS STUCK, NOT LAGGING.** `programFrame.sceneId`
  (snapshot) sat on the pre-take scene for 15+ seconds while Program was
  demonstrably compositing the new one. Root cause, in `MediaCore::renderTick`'s
  buffered branch: it attributed the snapshot from
  `ICompositor::latestDeliveredProgramFrame`, which is a **PEEK** — the program
  buffer hands back the same delivered frame on every call until its delivery
  thread advances `latest_`, and `D3DProgramBuffer` deliberately refuses to
  advance it when the export it is paired with was busy, and CLEARS it when a
  packet expires. So "a frame came back" was never evidence Program moved, and
  when nothing came back there was **no else branch at all** — the attribution
  simply stopped being written and the old scene stood forever. The delivery
  SEQUENCE is now what says a new frame reached air, and the rest is the pure
  `core/RenderedSceneAttributionPolicy.h` (`OutputLifecyclePolicy` shape):
  Follow a new delivery with plan evidence, Hold through <=12 ticks (200ms) of
  delivery jitter, then Forget. `programFrame.sceneIdAttribution` publishes
  `live`/`holding`/`unknown` unconditionally alongside
  `sceneIdAttributionTicks` and `deliverySequence`, so the field can never again
  assert a scene nothing confirmed. **Rule: a peek is not an observation** — if a
  reader republishes the same value, key your freshness on a sequence the
  producer advances, not on the call succeeding.
- **ONE STRUCTURED RECORD PER TAKE**, per operator action and never per frame, so
  it is on by default without flooding the bounded log. Armed in `loadSceneGraph`
  when the scene id actually changes (Take is a client-side scene swap that sends
  ONE sync, so that IS the take on this wire) and completed on the first program
  render tick after it — the only place the "after" half exists. Carries scene id
  and renderPlanId on both sides, the layer ids on both sides, the wall keys,
  whether the wall was **adopted or reset** (since #448: whether the ONE
  `core::TilesWallSource` for that wall id survived the take with its generation
  intact — `wallAdoptedSettled` is computed as `wallExistedBefore &&
  generationAfter == generationBefore`, not from the deleted
  `TilesPlanAnimation::adoptSettledFrom`), whether the
  wall's live background (`tiles-source-bg:`) made the first program frame, and
  the subscription-churn delta across the take. `core/TakeRecordPolicy.h` turns
  those into the one-word answer to "did the wall rebuild or cut" — and it will
  NOT certify a clean cut when the background dropped or a subscription churned
  in the same tick, because both look identical on air. Lands as a `[take]` line
  in the bounded process log (which the support bundle already collects) and as
  a bounded 8-deep `takeRecords` node in `sessionState`. The outgoing plan is
  built once on the command thread; the render tick pays only a layer-id copy.
- **SUBSCRIPTION CHURN IS MEASURED PER SOURCE.** `ZoomEngineRuntime` keeps a
  ledger keyed by sourceUuid — a `generation` that increments on every real
  (re)subscribe or teardown, a cumulative `churn` count, and the REASON
  (`resolution-change` / `cap-eviction` / `unrouted` / `departure` / `resubscribe`),
  decided by the pure `modules/ZoomSubscriptionChurnPolicy.h`. Published unconditionally as
  `sessionState().zoomSubscriptionChurn` (engine:false with empty arrays when there is
  no engine — the multiviewer-node rule). Two things it was built to catch:
  resolution is part of the subscription key, so a raised resolution is a genuine
  engine-side renderer teardown; and a source dropped from the requested set is
  unsubscribed outright. **The ledger deliberately SURVIVES the unsubscribe** — a
  record erased with the subscription cannot answer the question it exists for —
  and is cleared only where `sentSubscriptions_` is (leave / rejoin / a new engine
  process). **The instrument did its job and the churn is now FIXED (#478,
  2026-09-11)**: on a real 12-person show it fired several times a minute
  (`totalChurn` 53->59 in 20 s). See "A SECOND, ENGINE-SIDE CONTRIBUTOR" above:
  resolution is a stable tier (an active-speaker flip sends nothing and moves no
  generation, follow-speaker route or not), and the shell subscribes
  only sources. A `resolution-change` now means a guest was cued onto or left a bus
  (both directions: there is no ratchet). Retire reasons are split so the ledger
  cannot blame the cap for ordinary events: `cap-eviction` ONLY when the shell's
  `videoSubscriptionShortfall` names that participant, `video-off` when the engine
  roster says their camera went off, else `unrouted` (an operator un-route); the
  node carries `lastCapEvictions` / `lastVideoOff` / `lastUnrouted`, plus
  `fullResolutionCap` / `fullResolutionDemoted` for the 1080P cap.
- **PER-SOURCE CONTINUITY IS PART OF THE VERDICT (slice 1 of the persistent-sources
  redesign, 2026-09-10).** The wall-only verdict above missed the owner's actual
  case: a Tiles gallery whose foreground AND media background are on both Preview
  and Program re-rendered on every cut, and the take record still said `cut`,
  because nothing was watching the SOURCES a shared scene depends on.
  `core/SourceContinuityLedger.h` observes every render tick over `videoFrames`
  (keyed by `frame.participantId`) and bumps a per-source `generation` whenever a
  frameId regresses (a decoder reopened) or a source reappears after
  `absentTicksBeforeRestart` (30 ticks, 500 ms at 60 Hz) — a cold start. Only
  frames with CONTENT count (`hasPixels() || hasI420()`): a metadata-only Zoom
  roster frame is neither observed by the ledger nor accepted as "had a frame".
  The take record now carries `fromSourceIds` (the frame keys — participantId,
  else sourceId — of the outgoing Program plan UNION the outgoing Preview plan,
  i.e. the before-set), `sources[]` ({sourceId, generationBefore, generationAfter,
  frameIdBefore, frameIdAfter, restarted}) for every source in that before-set
  that is also in the incoming plan, `restartedSources` + the boolean
  `sharedSourceRestarted`, and `missingSources` + the boolean `sourceMissing` (a
  source the take brought on air with no frame on its first program tick, judged
  only when a frame was EXPECTED — a media layer, or a source that was running
  within the ledger's own restart window when the take was armed). Any restart or
  missing source forces `verdict=rebuilt`, even when the wall itself cut cleanly.
  **A HELD frame counts as continuous.** A source slower than the render rate
  (a 30 fps guest, a still, a paused poster) re-presents the same frameId for
  several ticks; the ledger treats that as the same generation, not a stall. This
  deliberately differs from spec §4.1's "each kept advancing frameId" — requiring
  an advance on the take tick would call every slow source a rebuild. Only a
  regression or a >30-tick absence is a restart.
  **Known low-probability race (documented, not fixed):** the before-set is read
  when the Take's `load-scene-graph` arms the record. If a repeating spine sync
  applies the swapped Preview (the outgoing scene) BEFORE that `load-scene-graph`
  lands, the "outgoing Preview plan" is already the new one and the before-union
  can miss incoming sources that were on the old Preview — so a source can be
  judged missing/unshared rather than continuous. The shell sends both in one
  sync, so this needs an interleaved spine tick. Extends the existing rule "a peek is not an
  observation" one step further: **a counter that only counts submits is not
  continuity** — the ledger has to watch frameId actually advance across the
  take, not just that something arrived.

Tests: `native/tests/RenderedSceneAttributionTest.cpp` (the attribution defect
red/green, the policies, and the take record end to end),
`native/tests/SourceContinuityLedgerTest.cpp`, and
`ZoomEngineRuntime.SubscriptionChurnNamesResolutionChangesAndTeardowns`.
