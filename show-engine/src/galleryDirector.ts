/**
 * The gallery director — a fixed grid of `cells` screen positions, each
 * displaying one roster slot (or blank). Unlike the roster it reads from,
 * the gallery has no notion of "who belongs where" beyond the operator's
 * explicit choices: removing a cell's occupant leaves a hole exactly where
 * it was, because the arrangement on screen is a deliberate editorial
 * choice, not an incidental ordering. `smartCells()` offers hosts who want
 * a compacted grid a derived, on-demand view of the same state — blanks
 * removed, remaining assignments renumbered — without ever storing that
 * view separately.
 */

import type { GalleryCell, Slot } from "./contracts.js";

export type GalleryDirectorOptions = {
  cells: number;
};

export type GalleryState = {
  version: 1;
  cells: number;
  assignments: GalleryCell[];
};

const GALLERY_STATE_VERSION = 1;

/**
 * Thrown by `GalleryDirector.fromJSON` when a persisted `GalleryState` is
 * structurally present but not a coherent gallery: a foreign version, a
 * `cells` count that disagrees with the configured one, an `assignments`
 * array whose length doesn't match `cells`, or an entry whose `cell` field
 * doesn't match its position. This mirrors `LiveSlotsRestoreError` — callers
 * should catch it and downgrade to a clean start rather than crash before
 * air.
 */
export class GalleryError extends Error {
  constructor(message: string) {
    super(message);
    this.name = "GalleryError";
  }
}

export class GalleryDirector {
  private readonly cellCount: number;
  private readonly slots: number[];

  constructor(options: GalleryDirectorOptions) {
    if (!Number.isInteger(options.cells) || options.cells < 1) {
      throw new Error(`GalleryDirector cells must be an integer >= 1, got ${options.cells}`);
    }
    this.cellCount = options.cells;
    this.slots = new Array<number>(options.cells).fill(0);
  }

  cells(): GalleryCell[] {
    return this.slots.map((slot, index) => ({ cell: index + 1, slot }));
  }

  smartCells(): GalleryCell[] {
    return this.slots
      .filter((slot) => slot !== 0)
      .map((slot, index) => ({ cell: index + 1, slot }));
  }

  empty(): void {
    this.slots.fill(0);
  }

  resetFromSlots(slots: readonly Slot[]): void {
    this.slots.fill(0);
    const occupied = slots.filter((slot) => slot.panelist !== null);
    for (let index = 0; index < occupied.length && index < this.cellCount; index += 1) {
      const occupiedSlot = occupied[index];
      if (occupiedSlot === undefined) continue;
      this.slots[index] = occupiedSlot.slot;
    }
  }

  replace(cell: number, slot: number): void {
    this.assertCell(cell);
    this.assertSlot(slot);
    this.slots[cell - 1] = slot;
  }

  remove(cell: number): void {
    this.assertCell(cell);
    this.slots[cell - 1] = 0;
  }

  applyOrder(slotOrder: readonly number[]): void {
    const filtered = slotOrder.filter((slot) => slot !== 0);
    for (const slot of filtered) this.assertSlot(slot);
    for (let index = 0; index < this.cellCount; index += 1) {
      this.slots[index] = index < filtered.length ? (filtered[index] ?? 0) : 0;
    }
  }

  occupiedCount(): number {
    return this.slots.reduce((count, slot) => (slot === 0 ? count : count + 1), 0);
  }

  toJSON(): GalleryState {
    return {
      version: GALLERY_STATE_VERSION,
      cells: this.cellCount,
      assignments: this.cells()
    };
  }

  /**
   * Restore a gallery from a persisted `GalleryState`. Throws `GalleryError`
   * (never a bare `Error`) when the document fails coherence checks: a
   * foreign version, a `cells` count that disagrees with the configured
   * one, an `assignments` array whose length doesn't match `cells`, or an
   * entry whose `cell` field doesn't match its position. Named so a caller
   * can catch it specifically and downgrade to a clean start.
   */
  static fromJSON(state: GalleryState, options: GalleryDirectorOptions): GalleryDirector {
    if (state.version !== GALLERY_STATE_VERSION) {
      throw new GalleryError(
        `persisted GalleryDirector version ${JSON.stringify(state.version)} is not supported (expected ${GALLERY_STATE_VERSION})`
      );
    }
    if (state.cells !== options.cells) {
      throw new GalleryError(
        `persisted cells ${state.cells} does not match configured cells ${options.cells}`
      );
    }
    if (state.assignments.length !== state.cells) {
      throw new GalleryError(
        `persisted assignments length ${state.assignments.length} does not match cells ${state.cells}`
      );
    }

    const restored = new GalleryDirector(options);
    state.assignments.forEach((assignment, index) => {
      if (assignment.cell !== index + 1) {
        throw new GalleryError(
          `persisted assignment at index ${index} claims cell ${JSON.stringify(assignment.cell)}, expected ${index + 1}`
        );
      }
      if (!Number.isInteger(assignment.slot) || assignment.slot < 0) {
        throw new GalleryError(
          `persisted assignment at cell ${assignment.cell} has invalid slot ${JSON.stringify(assignment.slot)}`
        );
      }
      restored.slots[index] = assignment.slot;
    });
    return restored;
  }

  private assertCell(cell: number): void {
    if (!Number.isInteger(cell) || cell < 1 || cell > this.cellCount) {
      throw new Error(`cell ${cell} is out of range 1..${this.cellCount}`);
    }
  }

  private assertSlot(slot: number): void {
    if (!Number.isInteger(slot) || slot < 0) {
      throw new Error(`slot ${slot} must be an integer >= 0`);
    }
  }
}
