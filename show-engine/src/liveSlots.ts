/**
 * The live slot roster — the heart of the show engine.
 * A fixed array of `capacity` positions, each mapping to one video slot the
 * host can deliver. People keep their slot until an operator moves them, and
 * removal leaves a hole rather than compacting, because the arrangement on
 * screen is a deliberate editorial choice.
 */

import { EXCLUSIVE_ROLES, type Panelist, type Slot } from "./contracts.js";

export type LiveSlotsOptions = {
  capacity: number;
  utilityPinBase: number;
};

export type LiveSlotsState = {
  version: 1;
  capacity: number;
  seats: ({ slot: number; panelist: Panelist } | null)[];
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
    this.enforceExclusiveRole(slot);
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
    this.enforceExclusiveRole(slot);
  }

  /** Clear every seat and re-seat the given roster in order. */
  rebuild(panelists: readonly Panelist[]): void {
    this.seats.fill(null);
    for (const panelist of panelists) {
      this.add(panelist);
    }
  }

  /**
   * Re-pull every seated panelist from the master database. Seats never move.
   * A participant who has vanished from the database keeps their seat but is
   * marked offline — visibly gone rather than silently dropped.
   */
  refresh(db: Map<string, Panelist>): void {
    this.seats.forEach((seat, index) => {
      if (seat === null) return;
      const fresh = db.get(seat.participantId);
      this.seats[index] =
        fresh === undefined ? { ...seat, online: false, videoOn: false } : { ...fresh };
    });

    this.seats.forEach((seat, index) => {
      if (seat !== null && isExclusive(seat.role)) this.enforceExclusiveRole(index + 1);
    });
  }

  toJSON(): LiveSlotsState {
    return {
      version: 1,
      capacity: this.capacity,
      seats: this.seats.map((panelist, index) =>
        panelist === null ? null : { slot: index + 1, panelist: { ...panelist } }
      )
    };
  }

  static fromJSON(state: LiveSlotsState, options: LiveSlotsOptions): LiveSlots {
    if (state.capacity !== options.capacity) {
      throw new Error(
        `persisted capacity ${state.capacity} does not match configured capacity ${options.capacity}`
      );
    }
    const restored = new LiveSlots(options);
    state.seats.forEach((seat, index) => {
      if (seat !== null) restored.seats[index] = { ...seat.panelist };
    });
    return restored;
  }

  /**
   * Utility participants (graphics bots, playback machines) carry a PIN at or
   * above `utilityPinBase` and seat from the end, keeping the low slots free
   * for people. `pin - utilityPinBase` is the offset from the last slot.
   */
  protected placementFor(panelist: Panelist): number | null {
    const utilitySlot = this.utilitySlotFor(panelist);
    if (utilitySlot !== null) return utilitySlot;
    return this.firstEmptySlot();
  }

  private utilitySlotFor(panelist: Panelist): number | null {
    if (panelist.pin === null) return null;
    const pin = Number(panelist.pin);
    if (!Number.isInteger(pin) || pin < this.utilityPinBase) return null;

    const target = this.capacity - (pin - this.utilityPinBase);
    for (let slot = Math.min(target, this.capacity); slot >= 1; slot -= 1) {
      if (this.seats[slot - 1] === null) return slot;
    }
    return null;
  }

  /** Demote every other seated holder of this seat's exclusive role. */
  private enforceExclusiveRole(slot: number): void {
    const seated = this.seats[slot - 1];
    if (seated === undefined || seated === null || !isExclusive(seated.role)) return;

    this.seats.forEach((other, index) => {
      if (other === null || index === slot - 1) return;
      if (other.role === seated.role) {
        this.seats[index] = { ...other, role: "panelist" };
      }
    });
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

function isExclusive(role: Panelist["role"]): boolean {
  return (EXCLUSIVE_ROLES as readonly string[]).includes(role);
}
