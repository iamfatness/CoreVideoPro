# OHG Show Engine — Plan 2 Outcomes

Companion to `2026-08-04-ohg-show-engine-direction.md`. Records what shipped and, more
importantly, the contract gaps Plan 3 must resolve before it writes anything. Written at
the close of Plan 2 execution (2026-08-05).

**Shipped:** the decision layer — `handsQueue`, `speakerRecency` (three strategies),
`galleryDirector`, `lookDirector`, `programBus` — plus contracts/config extensions, the
gallery persistence node, and the barrel. 269 tests, typecheck and build clean. Both Plan 1
deferrals scheduled here are closed: the `ZoomIngest` revision counter and per-endpoint
`MukanaHealth`.

**Scope change made during this plan:** SPX graphics were dropped in favour of CoreVideo
Pro's integrated lower-third/overlay engine. The A1–D2 layout enumeration went with it —
those names only ever identified pre-authored SPX templates. `lookDirector` now emits
nameplates and the layout is implied by which positions a look occupies. The design spec
was swept for this; see §7 and the §10 decisions log.

## Plan 3 must open by resolving these

The final whole-branch review found four contract gaps that are invisible from inside any
single task and were therefore invisible to the per-task reviewers. They are not defects in
the code as briefed — they are seams between modules that no brief owned.

1. **The hands wire format does not connect.** `MukanaClient.request()` reads the response
   body, hands it to `parseMukanaPanelists`, and discards it; `parseHandsPayload` needs the
   raw text, and no accessor exposes it. Worse, the two disagree on shape: the actor
   reference documents `?req=hands` as JSON (`{"q":…,"hands":{prev,curr,next}}`), while
   `parseHandsPayload` accepts the three-line CSV form, which is the *Isadora-internal*
   `HandsAPI` feed — plausibly from the separate hands device, not Mukana. Against the real
   endpoint `fetchHands()` returns an empty DB with health `"ok"`: a silently always-empty
   queue reporting green, which is the worst failure mode this package can have. Decide the
   parser's input shape against the reference doc before writing anything that consumes a
   queue.

2. **The accessibility skip-roles rule is enforced in one of four places.** Spec §3.7 says
   it lives in `speakerRecency`; it does not. All three assigners take a bare
   `participantId` and know nothing about roles — only `ProgramBus` gates. So an
   interpreter is blocked from program but still evicts a panelist from the FILO pool,
   still swaps into the visible window, and still sorts to the front of the gallery. That
   is exactly the on-screen churn the rule exists to prevent, and it is spec §8's own named
   golden scenario. Either the assigner interface carries the role, or the orchestrator
   gates centrally before dispatch — and the spec must be corrected to match, with a test
   that an interpreter moves neither program nor a gallery position.

3. **`galleryCells` is missing from `ShowEngineConfig`.** The gallery is persisted and
   `GalleryDirector.fromJSON` throws on a cell-count mismatch, but nothing configures that
   count. If the orchestrator takes it from the adapter's `maxGalleryCells` capability
   instead, every host swap turns a normal restore into a `GalleryError`. Add the field
   (default 16) and decide whether the capability overrides it or merely validates it.

4. **`resolveLook` throws on a stale page and offers no way to avoid it.** `pageCount`
   shrinks when hands are lowered, so a per-tick re-resolve at an unchanged page throws
   mid-show. The throw is right for the *operator action* (`ohg.look.nextGuest`) and wrong
   for a re-resolve; the module gives both callers one behavior. Export `clampPage`, or
   make the orchestrator the sole clamper and document which caller gets which contract.

## Design decisions worth not re-litigating

- **`cut()` and `auto()` are deliberately identical.** They differ only in the transition
  the host performs. The code says so; don't "fix" one into asymmetry.
- **`VisibleSetAssigner` drops a participant only when the pool is genuinely full.** The
  plan's prose for that case was unsatisfiable; the shipped rule (evict least-recently-active
  overall, seat the newcomer, then apply the ordinary not-visible swap) was reviewed and
  judged correct — it composes existing rules instead of adding a bespoke path, and the
  class header states the exception honestly rather than claiming nobody ever loses a seat.
- **A pre-Plan-2 state file is rejected, not migrated.** `load()` returns `null` so the
  engine starts clean. Pre-release software; a migration path would be maintained forever.
- **The gallery never compacts**; `smartCells()` is a derived view that stores nothing.

## Carried into Plan 3

- `speakerRecency.ts` duplicates five private helpers between `FiloAssigner` and
  `VisibleSetAssigner` (~half of the latter). Extract a shared base **before** a fourth
  strategy or the orchestrator arrives — a fix applied to one `markRecent` and not the
  other is the realistic failure.
- Spec §5's normative shapes have drifted from `contracts.ts`: `QueueState` is
  `{prev,current,next}` in the spec and `{previous,current,upcoming}` in code; `Look` lacks
  `label` and `includesHost`. §5 is what Plans 5–6 implement against on other platforms —
  refresh it.
- Spec §4.2 still lists `ohg.gfx.rundown` — an SPX transport concept with no referent in
  CVP's overlay engine. Decide whether it dies with SPX while registering Plan 3's actions.
- `ProgramBus.onActiveSpeaker(participantId, role)` forces the orchestrator to decide what
  role an *unseated* speaker has; defaulting to `"panelist"` would let an unseated
  interpreter take program. Consider `Role | null` with a documented policy, or pass the
  `Panelist | null`.
- `PlacementChange.participantId` is `string | null` but no producer emits `null`. Narrow
  it or document why the variant exists.
- Smaller: `applyOrder`'s silent truncation needs a doc line; `galleryDirector` methods lack
  the doc comments `liveSlots` carries; `findChairSlots` and `resetFromSlots` depend on
  caller array order undocumentedly; `handsQueue`'s tuple cast could be a destructure.
- Revisit `noUncheckedIndexedAccess` (carried from Plan 1). This plan added a lot of
  index-and-guard code already written as if it were on.

## Process note

The format change worked. Plan 1 produced five review findings that originated in its own
sample implementation bodies; Plan 2 supplied interfaces, behavior prose, and tests only,
and **zero defects traced to transcription**. Where the code diverges from a naive reading
it is *better*, because someone reasoned about it. The full-pool ambiguity surfaced
pre-dispatch precisely because there was no body to fall back on.

Two adjustments for Plans 3–7:

1. **When two tasks share a file, state the shared surface explicitly.** Tasks 5 and 6 split
   `speakerRecency.ts` between implementers with no instruction about sharing, and the
   second had no body to extend and no mandate to refactor the first's — hence the
   duplication. Say "Task N extracts what Task N-1 wrote; its tests must not change."
2. **Add a cross-task contract pass to plan authoring.** The remaining defect class is
   contract mismatch between modules, which no per-task brief or reviewer can see. For each
   new module ask: what produces its input, what consumes its output, and do the types and
   wire formats actually meet? For the hands gap that pass is one question — "what does
   `fetchHands()` hand to `parseHandsPayload`?" — and it has no answer.
