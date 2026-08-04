/**
 * The live slot roster — the heart of the show engine.
 * A fixed array of `capacity` positions, each mapping to one video slot the
 * host can deliver. People keep their slot until an operator moves them, and
 * removal leaves a hole rather than compacting, because the arrangement on
 * screen is a deliberate editorial choice.
 */

import type { Panelist, Slot } from "./contracts.js";

export type LiveSlotsOptions = {
  capacity: number;
  utilityPinBase: number;
};

export class LiveSlots {
  private readonly capacity: number;
  private readonly utilityPinBase: number;
  private readonly seats: (Panelist | null)[];

  constructor(options: LiveSlotsOptions) {
    if (!Number.isInteger(options.capacity) || options.capacity < 1) {
      throw new Error(`LiveSlots capacity must be an integer >= 1, got ${options.capacity}`);
    }
    this.capacity = options.capacity;
    this.utilityPinBase = options.utilityPinBase;
    this.seats = new Array<Panelist | null>(options.capacity).fill(null);
  }

  slots(): Slot[] {
    return this.seats.map((panelist, index) => ({
      slot: index + 1,
      panelist: panelist === null ? null : { ...panelist }
    }));
  }

  occupiedCount(): number {
    return this.seats.reduce((count, seat) => (seat === null ? count : count + 1), 0);
  }

  slotOf(participantId: string): number | null {
    const index = this.seats.findIndex((seat) => seat?.participantId === participantId);
    return index === -1 ? null : index + 1;
  }

  /** Seat a panelist. Returns the slot taken, their existing slot, or null when full. */
  add(panelist: Panelist): number | null {
    const existing = this.slotOf(panelist.participantId);
    if (existing !== null) return existing;

    const slot = this.placementFor(panelist);
    if (slot === null) return null;

    this.seats[slot - 1] = { ...panelist };
    return slot;
  }

  removeSlot(slot: number): void {
    this.assertSlot(slot);
    this.seats[slot - 1] = null;
  }

  replace(slot: number, panelist: Panelist): void {
    this.assertSlot(slot);
    const previous = this.slotOf(panelist.participantId);
    if (previous !== null && previous !== slot) {
      this.seats[previous - 1] = null;
    }
    this.seats[slot - 1] = { ...panelist };
  }

  /**
   * Choose a slot for a newcomer. Task 10 extends this with the utility-PIN
   * tail rule; the base behavior is the first empty slot.
   */
  protected placementFor(_panelist: Panelist): number | null {
    return this.firstEmptySlot();
  }

  protected firstEmptySlot(): number | null {
    const index = this.seats.findIndex((seat) => seat === null);
    return index === -1 ? null : index + 1;
  }

  protected assertSlot(slot: number): void {
    if (!Number.isInteger(slot) || slot < 1 || slot > this.capacity) {
      throw new Error(`slot ${slot} is out of range 1..${this.capacity}`);
    }
  }
}
