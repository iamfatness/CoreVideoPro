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

import type { LookDefinition, PlateTone, QueueState, Slot, TallySource } from "./contracts.js";
import { queueOrder } from "./handsQueue.js";

export type BoxAssignment = { box: number; slot: number | null };

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

/** How many pages the current queue spans for `look`, minimum 1. */
export function pageCountFor(look: LookDefinition, queue: QueueState): number {
  if (look.boxes === 0) return 1;
  const candidateCount = queueOrder(queue).length;
  return Math.max(1, Math.ceil(candidateCount / look.boxes));
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
 * `queue`: at least 0, at most `pageCountFor(look, queue) - 1`. This is
 * the orchestrator's tool for a re-resolve at an unchanged page — the
 * page count shrinks as hands are lowered, so a page that was valid a
 * moment ago can fall out of range between ticks, and the orchestrator
 * wants the nearest still-valid page rather than a thrown error. Callers
 * making an explicit operator move (e.g. "next guest") should call
 * `resolveLook` directly and let its throw surface instead of clamping
 * here — clamping would silently swallow a "next" that ran off the end,
 * which is exactly the silence an operator control must not produce.
 *
 * This is the one function in this module that clamps instead of
 * throwing on an out-of-range value — the name itself is the guardrail: a
 * caller reaching for `clampPage` is asking for clamping explicitly. A
 * non-integer or `NaN` page still throws, because that indicates a caller
 * bug rather than a stale value, and rounding it would hide the mistake.
 */
export function clampPage(look: LookDefinition, queue: QueueState, page: number): number {
  if (!Number.isInteger(page)) {
    throw new Error(`page ${page} is invalid: page must be an integer`);
  }

  const pageCount = pageCountFor(look, queue);
  return Math.min(Math.max(page, 0), pageCount - 1);
}

/**
 * Resolve `look` against `context.slots` and `context.queue`, windowed to
 * `context.page`. Throws when `page` is negative or at/beyond the queue's
 * page count for this look — an operator pressing "next" past the end
 * should see an error, not silence. This is the tool for a direct
 * operator action: callers that re-resolve on every tick, where the page
 * count can shrink out from under an unchanged page, should call
 * `clampPage` first and pass its result here instead of catching this
 * throw.
 */
export function resolveLook(
  look: LookDefinition,
  context: { queue: QueueState; slots: readonly Slot[]; page: number }
): LookResolution {
  const { queue, slots, page } = context;

  if (page < 0) {
    throw new Error(`page ${page} is invalid: page must be >= 0`);
  }

  const pageCount = pageCountFor(look, queue);
  if (page >= pageCount) {
    throw new Error(`page ${page} is out of range: this look has ${pageCount} page(s)`);
  }

  const candidates = queueOrder(queue);
  const windowStart = page * look.boxes;
  const windowEnd = windowStart + look.boxes;
  const windowPins = candidates.slice(windowStart, windowEnd);

  const boxes: BoxAssignment[] = [];
  for (let index = 0; index < look.boxes; index += 1) {
    const pin = windowPins[index];
    const slot = pin === undefined ? null : findSlotByPin(slots, pin);
    boxes.push({ box: index + 1, slot });
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
    pageCount
  };
}
