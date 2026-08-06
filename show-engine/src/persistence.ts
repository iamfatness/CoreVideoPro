/**
 * Show-state persistence.
 * The slot roster and override table are rewritten on every roster change
 * during a live show, so saves are atomic: write a temp file, then rename over
 * the target. A crash mid-save leaves the previous good state intact.
 *
 * Loads are forgiving by design — a missing, unreadable, or malformed state
 * file yields null so the engine starts clean, rather than refusing to boot
 * before a show. `load()` only validates that the document is shaped like a
 * `ShowState` (an object with a numeric `slots.capacity`, an array
 * `slots.seats`, and an object `overrides`); it does not look inside
 * individual seat or panelist records. Deeper roster coherence is
 * `LiveSlots.fromJSON`'s job — see its doc comment and
 * `LiveSlotsRestoreError`.
 */

import type { LiveSlotsState } from "./liveSlots.js";
import type { OverrideRecord } from "./overrideDb.js";

export type ShowState = {
  version: 1;
  slots: LiveSlotsState;
  overrides: Record<string, OverrideRecord>;
};

export type StateFs = {
  readFile: (path: string) => Promise<string>;
  writeFile: (path: string, content: string) => Promise<void>;
  rename: (from: string, to: string) => Promise<void>;
  mkdir: (path: string) => Promise<void>;
};

const STATE_VERSION = 1;

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

  async save(state: ShowState): Promise<void> {
    const tempPath = `${this.path}.tmp`;
    await this.fs.mkdir(parentDirectory(this.path));
    await this.fs.writeFile(tempPath, JSON.stringify(state, null, 2));
    await this.fs.rename(tempPath, this.path);
  }

  /**
   * Read persisted state, or null when it is absent, corrupt, a foreign
   * version, or not shaped like a `ShowState`. This is a shallow structural
   * check only — `slots` must be an object with a numeric `capacity` and an
   * array `seats`, and `overrides` must be an object. It deliberately does
   * not look inside individual seat or panelist records; that coherence
   * check belongs to `LiveSlots.fromJSON`, which throws a catchable
   * `LiveSlotsRestoreError` on a broken roster rather than returning null.
   */
  async load(): Promise<ShowState | null> {
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
    const candidate = parsed as Partial<ShowState>;
    if (candidate.version !== STATE_VERSION) return null;

    if (typeof candidate.slots !== "object" || candidate.slots === null) return null;
    const slots = candidate.slots as Partial<LiveSlotsState>;
    if (typeof slots.capacity !== "number") return null;
    if (!Array.isArray(slots.seats)) return null;

    if (typeof candidate.overrides !== "object" || candidate.overrides === null) return null;

    return candidate as ShowState;
  }
}
