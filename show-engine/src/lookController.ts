/**
 * Look selection and paging, carved out of `showEngine.ts` (Task 2). Owns
 * which look is selected, the operator's paging position within it, the
 * manual box assignments an operator plants for manual-fill boxes, and the
 * refusal recorded when a paging move can't happen — exactly what
 * `ShowEngine.setLook`/`nextGuest`/`prevGuest`/`assignBox`/`clearBox` and
 * their backing fields did before, moved verbatim (doc comments largely
 * ported as-is).
 *
 * What did NOT move: `clampPage`/`resolveLook` (`lookDirector.ts`) and their
 * paired call in `ShowEngine.tick()`. This class has no `tick()` of its own
 * and does no resolving against the roster/queue — it only tracks selection
 * and paging STATE, and `adjustPage`'s own bounds check (which must agree
 * with `resolveLook`'s page range without calling it — see that method's own
 * doc comment). `ShowEngine` still calls `clampPage` and `resolveLook`
 * directly, adjacent, sharing one per-tick capability value, reading/writing
 * this controller's `page()`/`setPage()` and `selected()`/`manualBoxes()`
 * around that pair — splitting `clampPage`/`resolveLook` themselves across
 * this module boundary is exactly the drift the file-level doc comment on
 * `ShowEngine` warns against.
 */

import type { BoxFill, Capability, LookDefinition, QueueState } from "./contracts.js";
import { effectiveBoxFill, pageCountFor, type ManualBoxAssignments } from "./lookDirector.js";

export type { ManualBoxAssignments } from "./lookDirector.js";

/**
 * WHY a paging refusal was set, tracked separately from the human-readable
 * message so a caller (`ShowEngine.tick()`'s `clearStaleRefusal` call) can
 * decide which refusals it's allowed to clear without parsing the message
 * itself. `"fill"` (the active look's boxes aren't currently filling from
 * the queue) and `"no-look"` (no look was selected at all) are both facts
 * about CURRENT state that a later tick can independently re-check and clear
 * once no longer true — a dead hands feed recovering, or a look finally
 * getting selected. `"range"` (the attempted move ran off the end of the
 * current page window) is NOT auto-cleared: it was true about a specific
 * attempted move, not a standing condition, and clearing it merely because
 * the look still fills from the queue (the ONLY fill strategy under which an
 * out-of-range move is even possible) would wipe it out on the very next
 * tick — before an operator polling on any normal cadence could ever see it.
 * Only a subsequent `adjustPage` (success or another refusal) or `select`
 * clears a `"range"` refusal.
 */
export type PagingRefusalKind = "no-look" | "fill" | "range";

export class LookController {
  private readonly looks: readonly LookDefinition[];

  private selectedLookId: string | null = null;
  private currentPage = 0;
  private manualBoxAssignments: ManualBoxAssignments = {};
  private refusalMessage: string | null = null;
  private refusalKind: PagingRefusalKind | null = null;

  constructor(deps: { looks: readonly LookDefinition[] }) {
    this.looks = deps.looks;
  }

  /** The currently-selected `LookDefinition`, or `null` when none is selected. */
  selected(): LookDefinition | null {
    return this.lookById(this.selectedLookId);
  }

  /**
   * Select the active look by id. Throws on an unknown look id. Switching
   * to a *different* look clears `manualBoxes` entirely (spec §3.2) — box 1
   * of one arrangement is not box 1 of another, so carrying manual
   * assignments across a look change would put the wrong person in the
   * wrong window. Re-selecting the SAME look is a no-op on `manualBoxes`,
   * so an idempotent re-select never wipes the operator's work. The actual
   * resolve-against-the-roster/clamp-the-page work happens in
   * `ShowEngine.tick()`.
   */
  select(lookId: string): void {
    const look = this.lookById(lookId);
    if (look === null) {
      throw new Error(`ShowEngine.setLook: unknown look id ${JSON.stringify(lookId)}`);
    }
    if (this.selectedLookId !== lookId) {
      this.manualBoxAssignments = {};
    }
    this.selectedLookId = lookId;
    // A paging refusal recorded against the PREVIOUS look (or against no
    // look at all) has nothing to say about this one — Fix round 1,
    // Finding 4: it must not survive a look change and read as a stale,
    // misattributed reason once the new look's own paging (or lack of it)
    // is what's actually in effect.
    this.refusalMessage = null;
    this.refusalKind = null;
  }

  /**
   * Adopt a persisted look selection — but ONLY when nothing has explicitly
   * selected one yet (`selectedLookId` is still its field-init `null`).
   * A `lookId` this configuration no longer defines cannot be adopted: it
   * would resolve to nothing, forever, and be written straight back to disk
   * on the next save. Dropped, with a warning returned for the caller to
   * surface and persist — `null` when there is nothing to warn about
   * (`lookId` is `null`, or names a look this configuration still defines).
   * Never re-persists the discarded id: the unknown `lookId` is never
   * adopted as `selectedLookId`, so a subsequent read of `selected()` (and
   * anything built from it, e.g. `ShowEngine.buildPersistedState`) cannot
   * resurrect it.
   */
  adoptRestored(lookId: string | null): string | null {
    const knownLookId = lookId !== null && this.lookById(lookId) === null ? null : lookId;
    let warning: string | null = null;
    if (knownLookId === null && lookId !== null) {
      warning =
        `persisted look ${JSON.stringify(lookId)} is not defined in this show's configuration; ` +
        `it was discarded along with the manual box assignments that belonged to it, and no look is selected`;
    }
    this.selectedLookId = this.selectedLookId ?? knownLookId;
    return warning;
  }

  /** The operator's current paging position within the selected look's queue window. */
  page(): number {
    return this.currentPage;
  }

  /**
   * Test-only escape hatch: writes the pending page directly, with **no**
   * clamping. Production code should never call this — operator paging goes
   * through `adjustPage`, and `ShowEngine.tick()`'s own `clampPage` step is
   * what keeps the page sane between ticks, deriving the valid range from
   * the current capability state and queue every time. This exists so a
   * property test can plant a page the way a *stale* one would actually
   * arrive at the top of a tick — e.g. left over from a fill-strategy flip
   * or a look change — and prove `tick()` survives it rather than throwing.
   */
  setPage(page: number): void {
    if (!Number.isInteger(page)) {
      throw new Error(`ShowEngine.setPage: page ${page} is invalid: page must be an integer`);
    }
    this.currentPage = page;
  }

  /**
   * Move the paging window forward/back by `delta`. Only takes effect when
   * the active look's boxes are actually filling from the hands queue
   * (`effectiveBoxFill(...) === "queue"`) — under manual fill (a manual
   * look, or a queue look whose hands feed just died) there is no queue
   * window to move through — AND only when the move stays inside the
   * current page range: `lookDirector.ts`'s own docs are explicit that
   * clamping a direct operator move "would silently swallow a 'next' that
   * ran off the end, which is exactly the silence an operator control must
   * not produce" (Fix round 1, Finding 3). Either refusal reason — wrong
   * fill strategy, or off the end — is recorded via `refusal()` instead of
   * throwing or silently doing nothing (spec §4). The page itself is left
   * untouched by a refused move.
   *
   * `queue` and `handsQueue` are the caller's already-resolved, already-
   * stripped values for THIS call — `ShowEngine` derives them fresh (the
   * same stripped queue and freshly-resolved capability `tick()`'s own
   * `clampPage`/`resolveLook` pair would use) rather than this controller
   * reaching back into roster/capability state it does not own.
   */
  adjustPage(delta: number, queue: QueueState, handsQueue: Capability): void {
    const look = this.selected();
    if (look === null) {
      this.refusalMessage = "paging refused: no look is selected";
      this.refusalKind = "no-look";
      return;
    }

    const fill = effectiveBoxFill(look, handsQueue);
    if (fill !== "queue") {
      this.refusalMessage = `paging refused: box fill is ${fill}, not queue-driven`;
      this.refusalKind = "fill";
      return;
    }

    const pageCount = pageCountFor(look, queue);
    const target = this.currentPage + delta;

    if (target < 0 || target >= pageCount) {
      this.refusalMessage = `paging refused: page ${target} is out of range (this look has ${pageCount} page(s))`;
      this.refusalKind = "range";
      return;
    }

    this.refusalMessage = null;
    this.refusalKind = null;
    this.currentPage = target;
  }

  /** The operator's current manual box assignments (box number → roster slot number). */
  manualBoxes(): ManualBoxAssignments {
    return this.manualBoxAssignments;
  }

  /**
   * Write or overwrite one manual box assignment. Meaningful only under
   * manual box fill (`resolveLook` simply ignores manual assignments for a
   * look currently filling from the queue), but recorded unconditionally —
   * the operator may be setting it up in advance of a fill-strategy switch.
   * Throws for a box number outside the ACTIVE look's `1..boxes` range —
   * caller error, not a state-changed-under-me refusal, so it throws rather
   * than getting a typed `refusal()`-style response. When no look is
   * selected yet there is no range to validate against, so only a
   * non-positive-integer box number is rejected.
   *
   * `slot` is validated too (Task 10, scenario 2 — it previously was not
   * validated AT ALL). `resolveLook` renders a manual box whose assigned
   * slot holds nobody as EMPTY, so `assignBox(1, -5)` or `assignBox(1, 2.5)`
   * used to be accepted, persisted, and then silently render a blank box —
   * an operator action reporting success while doing nothing, the exact
   * silence `nextGuest`'s typed refusal exists to avoid. The rule is
   * `GalleryDirector.assertSlot`'s verbatim (`galleryDirector.ts`), for the
   * same reason it is that rule there: **`0` is legal and means "blank this
   * box"** (the same convention a blank gallery cell uses), a negative or
   * fractional slot never is. No upper bound is enforced here — this
   * controller does not know the show's capacity, and a slot number too
   * HIGH still resolves safely to an empty box, exactly as
   * `parseProgramSource` reasons about `slot:<n>`.
   */
  assignBox(box: number, slot: number): void {
    this.assertBox("assignBox", box);
    if (!Number.isInteger(slot) || slot < 0) {
      throw new Error(
        `ShowEngine.assignBox: slot ${slot} is invalid: slot must be an integer >= 0 (0 blanks the box)`
      );
    }
    this.manualBoxAssignments = { ...this.manualBoxAssignments, [box]: slot };
  }

  /**
   * Remove one manual box assignment, leaving the rest untouched.
   *
   * Validates `box` exactly as `assignBox` does (Task 10, scenario 2 — it
   * previously validated NOTHING). `delete` on a key that cannot exist is a
   * silent no-op, so `ohg.look.box.clear 0` and `ohg.look.box.clear 9` on a
   * two-box look both answered `{kind:"ok"}` to a Companion button while
   * changing nothing. The two halves of the same operator control must
   * agree on what a box number is.
   */
  clearBox(box: number): void {
    this.assertBox("clearBox", box);
    const next = { ...this.manualBoxAssignments };
    delete next[box];
    this.manualBoxAssignments = next;
  }

  /** The shared box-number rule for `assignBox`/`clearBox` — one definition so the two can never disagree. */
  private assertBox(method: "assignBox" | "clearBox", box: number): void {
    const look = this.selected();
    if (!Number.isInteger(box) || box < 1 || (look !== null && box > look.boxes)) {
      throw new Error(
        look === null
          ? `ShowEngine.${method}: box ${box} is invalid: box must be a positive integer`
          : `ShowEngine.${method}: box ${box} is out of range for look ${JSON.stringify(look.id)} (1..${look.boxes})`
      );
    }
  }

  /**
   * Restore a persisted manual-box assignment set. A restored assignment
   * set belongs to whatever look was selected when it was saved — applying
   * it under a different look would put whoever the operator put in box 1
   * of one arrangement into box 1 of another, which is not the same seat —
   * so it is only adopted when `lookId` (the look the PERSISTED assignments
   * belonged to) still matches the currently-selected look id. Call this
   * AFTER `adoptRestored` has already run for the same restore, passing the
   * SAME raw persisted `lookId` (not whatever `adoptRestored` may have
   * discarded it to) — the comparison against `selected()`'s id is what
   * decides adoption, mirroring `ShowEngine.restore()`'s original ordering.
   */
  restoreManualBoxes(boxes: ManualBoxAssignments, lookId: string | null): void {
    this.manualBoxAssignments = lookId === this.selectedLookId ? { ...boxes } : {};
  }

  /** Why the last `adjustPage` call did not move the page, or `null` when the last attempt (if any) succeeded. */
  refusal(): { message: string; kind: PagingRefusalKind } | null {
    if (this.refusalMessage === null || this.refusalKind === null) return null;
    return { message: this.refusalMessage, kind: this.refusalKind };
  }

  /**
   * Clear a `"no-look"`/`"fill"` refusal once its cause no longer holds —
   * `"no-look"` clears unconditionally (this is only ever called by
   * `ShowEngine.tick()` when a look HAS just resolved: see the call site),
   * `"fill"` clears only once `effectiveFill` reports the look's boxes are
   * actually filling from the queue again. A `"range"` refusal is
   * deliberately left untouched — see `PagingRefusalKind`'s own doc comment
   * for why (it would wipe out on the very next tick, since queue fill is
   * the only strategy an out-of-range move can even happen under).
   */
  clearStaleRefusal(effectiveFill: BoxFill): void {
    const noLookRefusalResolved = this.refusalKind === "no-look";
    const fillRefusalResolved = this.refusalKind === "fill" && effectiveFill === "queue";
    if (noLookRefusalResolved || fillRefusalResolved) {
      this.refusalMessage = null;
      this.refusalKind = null;
    }
  }

  /** Look up a `LookDefinition` by id, or `null` for a `null` id. Never throws — `select` is the validating gate. */
  private lookById(lookId: string | null): LookDefinition | null {
    if (lookId === null) return null;
    return this.looks.find((candidate) => candidate.id === lookId) ?? null;
  }
}
