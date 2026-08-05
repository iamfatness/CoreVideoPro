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
    if (options.capacity < 1) {
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
