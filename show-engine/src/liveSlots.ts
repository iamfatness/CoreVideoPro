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

const LIVE_SLOTS_STATE_VERSION = 1;

/**
 * Thrown by `LiveSlots.fromJSON` when a persisted `LiveSlotsState` is
 * structurally present (it passed `StateStore.load`'s shallow shape check)
 * but is not a coherent roster: a foreign version, a capacity or seats-length
 * mismatch, a seat whose `slot` disagrees with its array index, or an entry
 * that is neither `null` nor an object carrying a `panelist`. Callers should
 * treat this as catchable and recoverable — e.g. downgrade to a clean start
 * plus a health warning — never let it take the engine down.
 */
export class LiveSlotsRestoreError extends Error {
  constructor(message: string) {
    super(message);
    this.name = "LiveSlotsRestoreError";
  }
}

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

  /**
   * Clear every seat and re-seat the given roster in order. A real meeting
   * roster can exceed `capacity`; this never throws for overflow — partial
   * seating is legitimate. Returns the panelists that could not be seated
   * (empty when everything fit) so a caller can surface it loudly rather
   * than have participants silently vanish.
   */
  rebuild(panelists: readonly Panelist[]): Panelist[] {
    this.seats.fill(null);
    const overflow: Panelist[] = [];
    for (const panelist of panelists) {
      if (this.add(panelist) === null) overflow.push({ ...panelist });
    }
    return overflow;
  }

  /**
   * Re-pull every seated panelist from the master database. Seats never move.
   * A participant who has vanished from the database keeps their seat but is
   * marked offline — visibly gone rather than silently dropped.
   *
   * `OverrideDb` is the authoritative source for role assignment; this method
   * only re-derives seat contents from it. If the re-pull leaves two seats
   * sharing an exclusive role, `enforceExclusiveRole` repairs the view by
   * walking seats in ascending index, so the **lowest slot number wins** and
   * every higher-numbered duplicate is demoted. That is a deliberately
   * different tie-break than `add`/`replace`, where the newly seated slot
   * always wins and the incumbent is demoted (newest-wins). Do not "fix"
   * this asymmetry — `refresh` is a view-level repair of whatever the
   * database says, not a re-run of seating policy.
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

  /**
   * Restore a roster from a persisted `LiveSlotsState`. This is the
   * "is this a coherent roster" check — `StateStore.load` only confirms the
   * document is shaped like a `PersistedShowState`; it never inspects individual seat
   * or panelist records. Throws `LiveSlotsRestoreError` (never a bare
   * `Error`) when the document fails that deeper check: a foreign version,
   * a capacity or seats-length mismatch, a seat whose `slot` disagrees with
   * its array index, or an entry that is neither `null` nor an object
   * carrying a `panelist` object. The error is named so a caller can catch
   * it specifically and downgrade to a clean start plus a health warning,
   * rather than the restore taking the engine down.
   */
  static fromJSON(state: LiveSlotsState, options: LiveSlotsOptions): LiveSlots {
    if (state.version !== LIVE_SLOTS_STATE_VERSION) {
      throw new LiveSlotsRestoreError(
        `persisted LiveSlots version ${JSON.stringify(state.version)} is not supported (expected ${LIVE_SLOTS_STATE_VERSION})`
      );
    }
    if (state.capacity !== options.capacity) {
      throw new LiveSlotsRestoreError(
        `persisted capacity ${state.capacity} does not match configured capacity ${options.capacity}`
      );
    }

    const seats: unknown[] = state.seats;
    if (seats.length !== state.capacity) {
      throw new LiveSlotsRestoreError(
        `persisted seats length ${seats.length} does not match capacity ${state.capacity}`
      );
    }

    const restored = new LiveSlots(options);
    seats.forEach((entry, index) => {
      if (entry === null) return;
      if (typeof entry !== "object") {
        throw new LiveSlotsRestoreError(`persisted seat at index ${index} is not an object`);
      }
      const candidate = entry as { slot?: unknown; panelist?: unknown };
      if (typeof candidate.panelist !== "object" || candidate.panelist === null) {
        throw new LiveSlotsRestoreError(
          `persisted seat at index ${index} is missing a panelist`
        );
      }
      if (candidate.slot !== index + 1) {
        throw new LiveSlotsRestoreError(
          `persisted seat at index ${index} claims slot ${JSON.stringify(candidate.slot)}, expected ${index + 1}`
        );
      }
      restored.seats[index] = { ...(candidate.panelist as Panelist) };
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

  /**
   * Demote every other seated holder of this seat's exclusive role. This is
   * a view-level repair, not the authoritative role assignment — `OverrideDb`
   * owns that. Called from `add`/`replace` right after seating `slot`, so
   * the tie-break there is newest-wins: the just-seated slot keeps the role
   * and every other holder is demoted. `refresh` also calls this, once per
   * seat in ascending index order after re-pulling from the database, which
   * makes ITS effective tie-break lowest-slot-wins instead — the first
   * exclusive-role holder encountered (by index) keeps it and any
   * higher-numbered duplicate seen afterward is demoted. Do not change
   * either tie-break without updating both call sites' expectations.
   */
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
