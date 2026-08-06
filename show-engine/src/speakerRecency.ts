/**
 * Speaker-recency position assignment. A limited pool of on-screen positions
 * has to track whoever is actually talking in a meeting with more
 * participants than positions. `FiloAssigner` is the simplest such policy:
 * a first-in-last-out router that fills free positions as newcomers speak
 * and, once full, evicts whoever has gone quiet longest to make room. It
 * never moves someone who already holds a position. `PositionAssigner` is
 * the shared interface other assignment strategies (see the sibling
 * `VisibleSetAssigner`) implement alongside this one.
 */

/** One position's on-screen occupant changing to a new participant. */
export type PlacementChange = { position: number; participantId: string | null };

/** A strategy that maps active speakers onto a fixed pool of positions. */
export interface PositionAssigner {
  /** Notify the assigner that `participantId` is the active speaker. */
  onActiveSpeaker(participantId: string): PlacementChange[];
  /** A fresh snapshot of position -> participantId for occupied positions. */
  positions(): Map<number, string>;
  /** Clear all state and reseat `participantIds` from position 1. */
  reset(participantIds: readonly string[]): void;
}

/**
 * First-in-last-out position assigner: positions `1..capacity` fill in
 * ascending order while the pool has room; once full, the least recently
 * active occupant is evicted to make room for the newcomer.
 */
export class FiloAssigner implements PositionAssigner {
  private readonly capacity: number;
  private readonly occupants = new Map<number, string>();
  /** Recency order, least recent first, most recent last. */
  private readonly recency: string[] = [];

  constructor(options: { capacity: number }) {
    if (!Number.isInteger(options.capacity) || options.capacity < 1) {
      throw new Error(`FiloAssigner capacity must be at least 1, got ${options.capacity}`);
    }
    this.capacity = options.capacity;
  }

  onActiveSpeaker(participantId: string): PlacementChange[] {
    const existingPosition = this.findPosition(participantId);
    if (existingPosition !== null) {
      this.markRecent(participantId);
      return [];
    }

    const freePosition = this.findFreePosition();
    if (freePosition !== null) {
      this.occupants.set(freePosition, participantId);
      this.markRecent(participantId);
      return [{ position: freePosition, participantId }];
    }

    const leastRecent = this.recency[0];
    if (leastRecent === undefined) {
      throw new Error("FiloAssigner: pool reported full but no occupant found to evict");
    }
    const evictedPosition = this.findPosition(leastRecent);
    if (evictedPosition === null) {
      throw new Error("FiloAssigner: recency entry has no matching position");
    }
    this.occupants.set(evictedPosition, participantId);
    this.removeFromRecency(leastRecent);
    this.markRecent(participantId);
    return [{ position: evictedPosition, participantId }];
  }

  positions(): Map<number, string> {
    return new Map(this.occupants);
  }

  reset(participantIds: readonly string[]): void {
    this.occupants.clear();
    this.recency.length = 0;
    for (const [index, participantId] of participantIds.entries()) {
      const position = index + 1;
      if (position > this.capacity) break;
      this.occupants.set(position, participantId);
      this.recency.push(participantId);
    }
  }

  private findPosition(participantId: string): number | null {
    for (const [position, occupant] of this.occupants) {
      if (occupant === participantId) return position;
    }
    return null;
  }

  private findFreePosition(): number | null {
    for (let position = 1; position <= this.capacity; position++) {
      if (!this.occupants.has(position)) return position;
    }
    return null;
  }

  private markRecent(participantId: string): void {
    this.removeFromRecency(participantId);
    this.recency.push(participantId);
  }

  private removeFromRecency(participantId: string): void {
    const index = this.recency.indexOf(participantId);
    if (index !== -1) this.recency.splice(index, 1);
  }
}

/**
 * Position assigner for a gallery larger than the on-screen window:
 * `capacity` positions exist, but only positions `1..visible` are on
 * screen. Everyone always holds a position — when an off-screen
 * participant becomes the active speaker they *swap* places with the
 * least recently active visible occupant rather than evicting anyone.
 * Capacity is only ever exceeded by an unknown speaker arriving at a
 * completely full pool, which is the sole case where an occupant (the
 * least recently active one overall) loses their seat.
 */
export class VisibleSetAssigner implements PositionAssigner {
  private readonly capacity: number;
  private readonly visible: number;
  private readonly occupants = new Map<number, string>();
  /** Recency order, least recent first, most recent last. */
  private readonly recency: string[] = [];

  constructor(options: { capacity: number; visible: number }) {
    if (!Number.isInteger(options.capacity) || options.capacity < 1) {
      throw new Error(`VisibleSetAssigner capacity must be at least 1, got ${options.capacity}`);
    }
    if (!Number.isInteger(options.visible) || options.visible < 1) {
      throw new Error(`VisibleSetAssigner visible must be at least 1, got ${options.visible}`);
    }
    if (options.visible > options.capacity) {
      throw new Error(
        `VisibleSetAssigner visible (${options.visible}) cannot exceed capacity (${options.capacity})`
      );
    }
    this.capacity = options.capacity;
    this.visible = options.visible;
  }

  onActiveSpeaker(participantId: string): PlacementChange[] {
    const existingPosition = this.findPosition(participantId);
    if (existingPosition !== null) {
      this.markRecent(participantId);
      if (this.isVisible(existingPosition)) {
        return [];
      }
      return this.swapWithStalestVisible(participantId, existingPosition);
    }

    const freePosition = this.findFreePosition();
    if (freePosition !== null) {
      this.occupants.set(freePosition, participantId);
      this.markRecent(participantId);
      if (this.isVisible(freePosition)) {
        return [{ position: freePosition, participantId }];
      }
      return this.swapWithStalestVisible(participantId, freePosition);
    }

    return this.seatIntoFullPool(participantId);
  }

  positions(): Map<number, string> {
    return new Map(this.occupants);
  }

  reset(participantIds: readonly string[]): void {
    this.occupants.clear();
    this.recency.length = 0;
    for (const [index, participantId] of participantIds.entries()) {
      const position = index + 1;
      if (position > this.capacity) break;
      this.occupants.set(position, participantId);
      this.recency.push(participantId);
    }
  }

  /**
   * Capacity-forced path: an unknown participant arrives with no free
   * position anywhere in the pool. The least recently active occupant
   * overall (visible or not) is evicted outright to free their seat for
   * the newcomer; if that freed seat isn't visible, the standard
   * not-visible rule then swaps the newcomer into the stalest visible
   * position.
   */
  private seatIntoFullPool(participantId: string): PlacementChange[] {
    const leastRecentOverall = this.recency[0];
    if (leastRecentOverall === undefined) {
      throw new Error("VisibleSetAssigner: pool reported full but no occupant found to evict");
    }
    const freedPosition = this.findPosition(leastRecentOverall);
    if (freedPosition === null) {
      throw new Error("VisibleSetAssigner: recency entry has no matching position");
    }
    this.occupants.delete(freedPosition);
    this.removeFromRecency(leastRecentOverall);

    this.occupants.set(freedPosition, participantId);
    this.markRecent(participantId);

    if (this.isVisible(freedPosition)) {
      return [{ position: freedPosition, participantId }];
    }
    return this.swapWithStalestVisible(participantId, freedPosition);
  }

  /** Swap `participantId`, currently seated off-screen at `fromPosition`, with the least recently active visible occupant. */
  private swapWithStalestVisible(participantId: string, fromPosition: number): PlacementChange[] {
    const stalestVisible = this.leastRecentVisiblePosition();
    if (stalestVisible === null) {
      throw new Error("VisibleSetAssigner: no visible occupant available to swap with");
    }
    const displaced = this.occupants.get(stalestVisible);
    if (displaced === undefined) {
      throw new Error("VisibleSetAssigner: visible position reported occupied but has no occupant");
    }
    this.occupants.set(stalestVisible, participantId);
    this.occupants.set(fromPosition, displaced);
    return [
      { position: stalestVisible, participantId },
      { position: fromPosition, participantId: displaced }
    ];
  }

  private isVisible(position: number): boolean {
    return position <= this.visible;
  }

  private findPosition(participantId: string): number | null {
    for (const [position, occupant] of this.occupants) {
      if (occupant === participantId) return position;
    }
    return null;
  }

  private findFreePosition(): number | null {
    for (let position = 1; position <= this.capacity; position++) {
      if (!this.occupants.has(position)) return position;
    }
    return null;
  }

  /** The visible position occupied by the least recently active visible occupant, or null if none is visible. */
  private leastRecentVisiblePosition(): number | null {
    for (const participantId of this.recency) {
      const position = this.findPosition(participantId);
      if (position !== null && this.isVisible(position)) {
        return position;
      }
    }
    return null;
  }

  private markRecent(participantId: string): void {
    this.removeFromRecency(participantId);
    this.recency.push(participantId);
  }

  private removeFromRecency(participantId: string): void {
    const index = this.recency.indexOf(participantId);
    if (index !== -1) this.recency.splice(index, 1);
  }
}

/**
 * Pure ordering strategy for gallery arrangement: nothing is evicted and
 * nothing swaps, the whole participant set is simply sorted most
 * recently active first. Reproduces the legacy "speaking score" bucket
 * sort without the fixed-size array.
 */
export class RecencyScores {
  /** participantId -> recency rank, higher meaning more recently active. Absent means never spoken. */
  private readonly rank = new Map<string, number>();
  private nextRank = 0;

  onActiveSpeaker(participantId: string): void {
    this.rank.set(participantId, this.nextRank++);
  }

  /**
   * Returns `participantIds` sorted most-recently-active first. Unranked
   * participants sort last, in their original relative order. Always a
   * permutation of the input — nothing is added or dropped.
   */
  order(participantIds: readonly string[]): string[] {
    return [...participantIds].sort((a, b) => {
      const rankA = this.rank.get(a);
      const rankB = this.rank.get(b);
      if (rankA === undefined && rankB === undefined) return 0;
      if (rankA === undefined) return 1;
      if (rankB === undefined) return -1;
      return rankB - rankA;
    });
  }

  reset(): void {
    this.rank.clear();
    this.nextRank = 0;
  }
}
