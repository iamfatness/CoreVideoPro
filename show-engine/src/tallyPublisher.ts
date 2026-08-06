/**
 * Tally derivation — answers, for every panelist, "am I on air right now?"
 * The legacy system derived this by parsing the hardware switcher's full
 * state and mapping its source IDs back to people. This engine *is* the
 * switcher, so `deriveTally` reads it straight off state other modules
 * already own: the program source, the live roster, the gallery grid, and
 * the current look resolution. Getting this wrong is visible on air in the
 * worst way, so every slot number is checked against the roster before it
 * is reported live — a box or gallery cell pointing at an empty or
 * out-of-range slot contributes nobody.
 */

import type { GalleryCell, ProgramSource, Slot } from "./contracts.js";
import type { LookResolution } from "./lookDirector.js";

export type TallyMode = "none" | "slot" | "activeSpeaker" | "gallery" | "look";

export type TallyState = {
  mode: TallyMode;
  onAirSlots: number[];
  onAirPins: string[];
  onAirParticipantIds: string[];
};

/** Find the roster entry for a slot number, or undefined if out of range. */
function findSlot(slots: readonly Slot[], slotNumber: number): Slot | undefined {
  return slots.find((entry) => entry.slot === slotNumber);
}

/**
 * Build a `TallyState` for `mode` from a raw list of candidate slot
 * numbers. Deduplicates and sorts ascending, then drops any slot that is
 * out of range or unoccupied. PINs are appended only for panelists who
 * have one, so `onAirPins` can be shorter than the other two arrays.
 */
function buildTally(mode: TallyMode, candidateSlots: readonly number[], slots: readonly Slot[]): TallyState {
  const uniqueSorted = Array.from(new Set(candidateSlots)).sort((a, b) => a - b);

  const onAirSlots: number[] = [];
  const onAirPins: string[] = [];
  const onAirParticipantIds: string[] = [];

  for (const slotNumber of uniqueSorted) {
    const entry = findSlot(slots, slotNumber);
    if (entry === undefined || entry.panelist === null) continue;
    onAirSlots.push(slotNumber);
    onAirParticipantIds.push(entry.panelist.participantId);
    if (entry.panelist.pin !== null) onAirPins.push(entry.panelist.pin);
  }

  return { mode, onAirSlots, onAirPins, onAirParticipantIds };
}

/** Slot numbers a "boxes" look puts on air: the host chair, reader chair, and every filled box. */
function boxesSlots(look: LookResolution): number[] {
  const result: number[] = [];
  if (look.hostSlot !== null) result.push(look.hostSlot);
  if (look.readerSlot !== null) result.push(look.readerSlot);
  for (const assignment of look.boxes) {
    if (assignment.slot !== null) result.push(assignment.slot);
  }
  return result;
}

/**
 * Derive who is currently on air from the program source and the state
 * other modules own. `look === null` for a `look` source means the
 * operator has selected a look the engine cannot resolve yet — mode
 * `look`, nobody on air, since claiming someone is live would be worse
 * than claiming nobody is.
 */
export function deriveTally(input: {
  source: ProgramSource;
  slots: readonly Slot[];
  gallery: readonly GalleryCell[];
  look: LookResolution | null;
  activeSpeakerSlot: number | null;
}): TallyState {
  const { source, slots, gallery, look, activeSpeakerSlot } = input;

  switch (source.kind) {
    case "black":
      return buildTally("none", [], slots);

    case "slot":
      return buildTally("slot", [source.slot], slots);

    case "activeSpeaker":
      return buildTally(
        "activeSpeaker",
        activeSpeakerSlot === null ? [] : [activeSpeakerSlot],
        slots
      );

    case "gallery": {
      const candidateSlots = gallery.filter((cell) => cell.slot !== 0).map((cell) => cell.slot);
      return buildTally("gallery", candidateSlots, slots);
    }

    case "look": {
      if (look === null) return buildTally("look", [], slots);
      return buildTally("look", boxesSlots(look), slots);
    }
  }
}

/** Structural equality of two `TallyState`s, for change detection before a republish. */
export function tallyEquals(a: TallyState, b: TallyState): boolean {
  if (a.mode !== b.mode) return false;
  return (
    arraysEqual(a.onAirSlots, b.onAirSlots) &&
    arraysEqual(a.onAirPins, b.onAirPins) &&
    arraysEqual(a.onAirParticipantIds, b.onAirParticipantIds)
  );
}

function arraysEqual<T>(a: readonly T[], b: readonly T[]): boolean {
  if (a.length !== b.length) return false;
  return a.every((value, index) => value === b[index]);
}
