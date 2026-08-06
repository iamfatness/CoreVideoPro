/**
 * Show-state persistence.
 * The slot roster, override table, and gallery arrangement are rewritten on
 * every roster or gallery change during a live show, so saves are atomic:
 * write a temp file, then rename over the target. A crash mid-save leaves
 * the previous good state intact.
 *
 * Loads are forgiving by design — a missing, unreadable, or malformed state
 * file yields null so the engine starts clean, rather than refusing to boot
 * before a show. `load()` only validates that the document is shaped like a
 * `PersistedShowState` (an object with a numeric `slots.capacity`, an array
 * `slots.seats`, an object `overrides`, a `gallery` object carrying a
 * numeric `cells` and an array `assignments`, an object `manualBoxes`, and a
 * `lookId` that is a string or null); it does not look inside individual
 * seat, panelist, or gallery-cell records. Deeper coherence is
 * `LiveSlots.fromJSON`'s and `GalleryDirector.fromJSON`'s job — see their
 * doc comments and their named restore errors.
 *
 * A state file written before the gallery was added has no `gallery` key.
 * Such a file is rejected outright (load() returns null, so the engine
 * starts clean) rather than migrated with a default gallery: this is
 * pre-release software with no deployed state files, and a silent
 * migration path would have to be maintained forever for no benefit.
 *
 * `STATE_VERSION` is the guard that makes that rejection reachable when a
 * change is invisible to the shallow shape check. Version 2 re-keyed
 * `overrides` from PIN to `PersonKey` and re-shaped `OverrideRecord` from
 * `pin` to `personKey`. Both changes live *inside* the override records,
 * which `load()` deliberately does not inspect — so a version-1 file would
 * otherwise pass every structural check and restore overrides under keys
 * (`"1383"`) that `buildPanelistDb` never looks up (`"pin:1383"`), silently
 * dropping every operator-assigned role at the first restart of a show.
 * Bumping the version turns that silent loss into the same clean start the
 * pre-gallery files get. Version 3 adds `manualBoxes` (the operator's box →
 * roster-slot fallback assignments) paired with `lookId` (which look those
 * assignments belong to) — without the pairing, restarting into a different
 * look would silently inherit a previous look's manual boxes, seating the
 * wrong person in a box that means something else under the new look. Any
 * future change to what lives inside `overrides` or `slots.seats` must bump
 * it again for the same reason.
 */

import type { GalleryState } from "./galleryDirector.js";
import type { LiveSlotsState } from "./liveSlots.js";
import type { ManualBoxAssignments } from "./lookDirector.js";
import type { OverrideRecord } from "./overrideDb.js";
import type { PersonKey } from "./personKey.js";

export type PersistedShowState = {
  version: 3;
  slots: LiveSlotsState;
  overrides: Record<PersonKey, OverrideRecord>;
  gallery: GalleryState;
  manualBoxes: ManualBoxAssignments;
  lookId: string | null;
};

export type StateFs = {
  readFile: (path: string) => Promise<string>;
  writeFile: (path: string, content: string) => Promise<void>;
  rename: (from: string, to: string) => Promise<void>;
  mkdir: (path: string) => Promise<void>;
};

export const STATE_VERSION = 3;

function parentDirectory(path: string): string {
  const slashIndex = path.lastIndexOf("/");
  const backslashIndex = path.lastIndexOf("\\");
  const index = Math.max(slashIndex, backslashIndex);

  if (index < 0) return ".";
  if (index === 0) return path[0]; // Return "/" or "\" (root-level path)
  return path.slice(0, index);
}

export class StateStore {
  private readonly path: string;
  private readonly fs: StateFs;

  constructor(path: string, deps: { fs: StateFs }) {
    this.path = path;
    this.fs = deps.fs;
  }

  async save(state: PersistedShowState): Promise<void> {
    const tempPath = `${this.path}.tmp`;
    await this.fs.mkdir(parentDirectory(this.path));
    await this.fs.writeFile(tempPath, JSON.stringify(state, null, 2));
    await this.fs.rename(tempPath, this.path);
  }

  /**
   * Read persisted state, or null when it is absent, corrupt, a foreign
   * version, or not shaped like a `PersistedShowState`. This is a shallow
   * structural check only — `slots` must be an object with a numeric
   * `capacity` and an array `seats`, `overrides` must be an object,
   * `gallery` must be an object with a numeric `cells` and an array
   * `assignments`, `manualBoxes` must be an object, and `lookId` must be a
   * string or null. A state file with no `gallery` key at all (written
   * before the gallery landed) fails this check and returns null rather
   * than being migrated, and a file carrying any `version` but the current
   * one is rejected before the shape is even looked at — that is what
   * catches a change inside the override records, which this check cannot
   * see. It deliberately does not look inside individual seat, panelist, or
   * gallery-cell records; that coherence check belongs to
   * `LiveSlots.fromJSON` and `GalleryDirector.fromJSON`, which throw their
   * own catchable named errors on a broken roster or gallery rather than
   * returning null.
   */
  async load(): Promise<PersistedShowState | null> {
    let content: string;
    try {
      content = await this.fs.readFile(this.path);
    } catch {
      return null;
    }

    let parsed: unknown;
    try {
      parsed = JSON.parse(content);
    } catch {
      return null;
    }

    if (typeof parsed !== "object" || parsed === null) return null;
    const candidate = parsed as Partial<PersistedShowState>;
    if (candidate.version !== STATE_VERSION) return null;

    if (typeof candidate.slots !== "object" || candidate.slots === null) return null;
    const slots = candidate.slots as Partial<LiveSlotsState>;
    if (typeof slots.capacity !== "number") return null;
    if (!Array.isArray(slots.seats)) return null;

    if (typeof candidate.overrides !== "object" || candidate.overrides === null) return null;

    if (typeof candidate.gallery !== "object" || candidate.gallery === null) return null;
    const gallery = candidate.gallery as Partial<GalleryState>;
    if (typeof gallery.cells !== "number") return null;
    if (!Array.isArray(gallery.assignments)) return null;

    if (
      typeof candidate.manualBoxes !== "object" ||
      candidate.manualBoxes === null ||
      Array.isArray(candidate.manualBoxes)
    ) {
      return null;
    }

    if (typeof candidate.lookId !== "string" && candidate.lookId !== null) return null;

    return candidate as PersistedShowState;
  }
}
