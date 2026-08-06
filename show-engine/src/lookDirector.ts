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

import type { LookDefinition, PlateTone, QueueState, Slot } from "./contracts.js";
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
 * Resolve `look` against `context.slots` and `context.queue`, windowed to
 * `context.page`. Throws when `page` is negative or at/beyond the queue's
 * page count for this look — an operator pressing "next" past the end
 * should see an error, not silence.
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
    hostSlot,
    readerSlot,
    boxes,
    nameplates,
    page,
    pageCount
  };
}
