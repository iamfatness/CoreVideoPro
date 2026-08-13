/**
 * The look director — resolves a `LookDefinition` against the live roster
 * and the hands-raised queue into a concrete on-screen arrangement: which
 * roster slot sits in the host and reader chairs, which slot fills each
 * guest box, and the nameplates to draw for every occupied position. This
 * is the successor to the legacy SuperSource preset + SPX field payload:
 * the look declares shape, this module works out who currently fills it.
 * Paging replaces the old substring-window trick for moving through a
 * candidate queue longer than the look has boxes. Never mutates its inputs
 * and never assigns or reads back a role beyond locating the host/reader
 * chairs — `OverrideDb` remains the sole authority on editorial roles.
 */

import type {
  BoxFill,
  Capability,
  LookDefinition,
  PlateTone,
  QueueState,
  Slot,
  TallySource
} from "./contracts.js";
import { canUse } from "./capabilities.js";
import { queueOrder } from "./handsQueue.js";

export type BoxAssignment = { box: number; slot: number | null };

/** Manual box assignments: box number → roster slot number. */
export type ManualBoxAssignments = Record<number, number>;

export type NameplatePosition = { kind: "host" } | { kind: "reader" } | { kind: "box"; box: number };

export type Nameplate = {
  position: NameplatePosition;
  slot: number;
  name: string;
  location: string;
  tone: PlateTone;
};

export type LookResolution = {
  lookId: string;
  scenePreset: string;
  plateTone: PlateTone;
  tallySource: TallySource;
  hostSlot: number | null;
  readerSlot: number | null;
  boxes: BoxAssignment[];
  nameplates: Nameplate[];
  page: number;
  pageCount: number;
  boxFill: BoxFill;
};

/**
 * Scan the roster for the slots holding the `host` and `reader` roles, in
 * ascending slot order. The roster guarantees at most one holder of each
 * role, so the first match wins. Returns `null` for either chair when
 * nobody currently holds it.
 */
export function findChairSlots(slots: readonly Slot[]): {
  hostSlot: number | null;
  readerSlot: number | null;
} {
  let hostSlot: number | null = null;
  let readerSlot: number | null = null;
  for (const entry of slots) {
    if (entry.panelist === null) continue;
    if (hostSlot === null && entry.panelist.role === "host") hostSlot = entry.slot;
    if (readerSlot === null && entry.panelist.role === "reader") readerSlot = entry.slot;
  }
  return { hostSlot, readerSlot };
}

/**
 * The strategy actually in effect for filling `look`'s guest boxes this
 * tick. `"queue"` only when the look declares `"queue"` and `handsQueue` is
 * usable; every other case — a `"manual"` look, or a `"queue"` look whose
 * hands feed is unavailable or disabled — resolves to `"manual"`. This one
 * rule is the whole degradation story for a hands feed dying mid-show:
 * `resolveLook` treats an omitted `handsQueue` in its context the same way,
 * substituting an unusable capability before it ever reaches here.
 */
export function effectiveBoxFill(look: LookDefinition, handsQueue: Capability): BoxFill {
  if (look.boxFill === "queue" && canUse(handsQueue)) {
    return "queue";
  }
  return "manual";
}

/** Stand-in for an omitted `handsQueue` capability: never usable. */
const NO_HANDS_QUEUE: Capability = { state: "disabled", detail: null };

/** How many pages the current queue spans for `look`, minimum 1. */
export function pageCountFor(look: LookDefinition, queue: QueueState): number {
  if (look.boxes === 0) return 1;
  const candidateCount = queueOrder(queue).length;
  return Math.max(1, Math.ceil(candidateCount / look.boxes));
}

/**
 * How many pages `look` actually has under the fill strategy in effect.
 * Manual fill has exactly one page — there is no queue to window through —
 * so paging is inert rather than fatal when the hands feed dies. Both
 * `clampPage` and `resolveLook` derive their page range from this one
 * function, which is what keeps the documented clamp-then-resolve sequence
 * safe: they cannot disagree about how many pages exist.
 */
function pageCountForFill(look: LookDefinition, queue: QueueState, boxFill: BoxFill): number {
  return boxFill === "queue" ? pageCountFor(look, queue) : 1;
}

/** Find the roster slot number whose panelist carries the given PIN. */
function findSlotByPin(slots: readonly Slot[], pin: string): number | null {
  for (const entry of slots) {
    if (entry.panelist !== null && entry.panelist.pin === pin) return entry.slot;
  }
  return null;
}

/** Find the panelist seated at a given roster slot number. */
function findPanelistBySlot(slots: readonly Slot[], slotNumber: number) {
  for (const entry of slots) {
    if (entry.slot === slotNumber) return entry.panelist;
  }
  return null;
}

/**
 * Clamp `page` into the valid range for `look` against the current
 * `queue` and the fill strategy `handsQueue` puts in effect: at least 0,
 * at most `pageCount - 1`. This is the orchestrator's tool for a
 * re-resolve at an unchanged page — the page count shrinks as hands are
 * lowered, so a page that was valid a moment ago can fall out of range
 * between ticks, and the orchestrator wants the nearest still-valid page
 * rather than a thrown error. Callers making an explicit operator move
 * (e.g. "next guest") should call `resolveLook` directly and let its throw
 * surface instead of clamping here — clamping would silently swallow a
 * "next" that ran off the end, which is exactly the silence an operator
 * control must not produce.
 *
 * `handsQueue` must be the same capability the following `resolveLook`
 * call is given, and an omitted one means unusable in both — otherwise the
 * two disagree about the page range. That disagreement was a real bug: a
 * hands feed dying while the operator sat on page 2 degraded the look to
 * manual fill (one page) while a capability-blind clamp still returned 2,
 * and the tick then threw. A capability state must never be able to throw
 * a tick, so the clamp reads the effective fill exactly as the resolver
 * does.
 *
 * This is the one function in this module that clamps instead of
 * throwing on an out-of-range value — the name itself is the guardrail: a
 * caller reaching for `clampPage` is asking for clamping explicitly. A
 * non-integer or `NaN` page still throws, because that indicates a caller
 * bug rather than a stale value, and rounding it would hide the mistake.
 */
export function clampPage(
  look: LookDefinition,
  queue: QueueState,
  page: number,
  handsQueue?: Capability
): number {
  if (!Number.isInteger(page)) {
    throw new Error(`page ${page} is invalid: page must be an integer`);
  }

  const boxFill = effectiveBoxFill(look, handsQueue ?? NO_HANDS_QUEUE);
  const pageCount = pageCountForFill(look, queue, boxFill);
  return Math.min(Math.max(page, 0), pageCount - 1);
}

/**
 * Resolve `look` against `context.slots` and `context.queue`, windowed to
 * `context.page`. Throws when `page` is negative or at/beyond the valid
 * page range for this look's effective fill strategy — an operator
 * pressing "next" past the end should see an error, not silence. This is
 * the tool for a direct operator action: callers that re-resolve on every
 * tick, where the page count can shrink out from under an unchanged page,
 * should call `clampPage` first and pass its result here instead of
 * catching this throw — handing `clampPage` the same `handsQueue` given
 * here, so the clamp and this call agree on the page range.
 *
 * `context.handsQueue` decides, via `effectiveBoxFill`, whether boxes fill
 * from the queue or from `context.manualBoxes`; an omitted `handsQueue` is
 * treated as unusable, the same as one that is `unavailable` or
 * `disabled`. Under manual fill a stale assignment — naming a slot that is
 * empty or does not exist in the roster — resolves to an empty box rather
 * than throwing, since manual assignments are expected to outlive roster
 * churn.
 */
export function resolveLook(
  look: LookDefinition,
  context: {
    queue: QueueState;
    slots: readonly Slot[];
    page: number;
    handsQueue?: Capability;
    manualBoxes?: ManualBoxAssignments;
  }
): LookResolution {
  const { queue, slots, page, manualBoxes } = context;
  const boxFill = effectiveBoxFill(look, context.handsQueue ?? NO_HANDS_QUEUE);

  if (page < 0) {
    throw new Error(`page ${page} is invalid: page must be >= 0`);
  }

  const pageCount = pageCountForFill(look, queue, boxFill);
  let boxes: BoxAssignment[];

  if (boxFill === "queue") {
    if (page >= pageCount) {
      throw new Error(`page ${page} is out of range: this look has ${pageCount} page(s)`);
    }

    const candidates = queueOrder(queue);
    const windowStart = page * look.boxes;
    const windowEnd = windowStart + look.boxes;
    const windowPins = candidates.slice(windowStart, windowEnd);

    boxes = [];
    for (let index = 0; index < look.boxes; index += 1) {
      const pin = windowPins[index];
      const slot = pin === undefined ? null : findSlotByPin(slots, pin);
      boxes.push({ box: index + 1, slot });
    }
  } else {
    if (page !== 0) {
      throw new Error(`page ${page} is invalid: manual box fill has a single page`);
    }

    boxes = [];
    for (let index = 0; index < look.boxes; index += 1) {
      const boxNumber = index + 1;
      const assignedSlot = manualBoxes?.[boxNumber];
      const slot =
        assignedSlot !== undefined && findPanelistBySlot(slots, assignedSlot) !== null
          ? assignedSlot
          : null;
      boxes.push({ box: boxNumber, slot });
    }
  }

  const { hostSlot: seatedHostSlot, readerSlot: seatedReaderSlot } = findChairSlots(slots);
  const hostSlot = look.includesHost ? seatedHostSlot : null;
  const readerSlot = look.includesReader ? seatedReaderSlot : null;

  const nameplates: Nameplate[] = [];

  if (hostSlot !== null) {
    const panelist = findPanelistBySlot(slots, hostSlot);
    if (panelist !== null) {
      nameplates.push({
        position: { kind: "host" },
        slot: hostSlot,
        name: panelist.displayName,
        location: panelist.location,
        tone: look.plateTone
      });
    }
  }

  if (readerSlot !== null) {
    const panelist = findPanelistBySlot(slots, readerSlot);
    if (panelist !== null) {
      nameplates.push({
        position: { kind: "reader" },
        slot: readerSlot,
        name: panelist.displayName,
        location: panelist.location,
        tone: look.plateTone
      });
    }
  }

  for (const assignment of boxes) {
    if (assignment.slot === null) continue;
    const panelist = findPanelistBySlot(slots, assignment.slot);
    if (panelist === null) continue;
    nameplates.push({
      position: { kind: "box", box: assignment.box },
      slot: assignment.slot,
      name: panelist.displayName,
      location: panelist.location,
      tone: look.plateTone
    });
  }

  return {
    lookId: look.id,
    scenePreset: look.scenePreset,
    plateTone: look.plateTone,
    tallySource: look.tallySource,
    hostSlot,
    readerSlot,
    boxes,
    nameplates,
    page,
    pageCount,
    boxFill
  };
}
