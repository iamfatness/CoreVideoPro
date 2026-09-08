import { describe, expect, it, afterEach } from "vitest";
import { mkdtemp, rm } from "node:fs/promises";
import { tmpdir } from "node:os";
import { join } from "node:path";
import { nodeStateFs } from "./nodeStateFs.js";
import { StateStore, type PersistedShowState } from "../persistence.js";

const dirsToClean: string[] = [];

async function uniqueDir(): Promise<string> {
  const dir = await mkdtemp(join(tmpdir(), "show-engine-nodeStateFs-"));
  dirsToClean.push(dir);
  return dir;
}

afterEach(async () => {
  while (dirsToClean.length > 0) {
    const dir = dirsToClean.pop();
    if (dir === undefined) continue;
    await rm(dir, { recursive: true, force: true });
  }
});

describe("nodeStateFs", () => {
  it("mkdir is idempotent (creating an already-existing directory does not throw)", async () => {
    const fs = nodeStateFs();
    const dir = await uniqueDir();
    const nested = join(dir, "a", "b");
    await fs.mkdir(nested);
    await expect(fs.mkdir(nested)).resolves.toBeUndefined();
  });

  it("writeFile+readFile round-trips UTF-8 content, including non-ASCII", async () => {
    const fs = nodeStateFs();
    const dir = await uniqueDir();
    const path = join(dir, "state.json");
    const content = 'hello éèê world 日本語 😀';
    await fs.writeFile(path, content);
    const readBack = await fs.readFile(path);
    expect(readBack).toBe(content);
  });

  it("rename replaces an existing target", async () => {
    const fs = nodeStateFs();
    const dir = await uniqueDir();
    const from = join(dir, "from.json");
    const to = join(dir, "to.json");
    await fs.writeFile(from, "new content");
    await fs.writeFile(to, "stale content");
    await fs.rename(from, to);
    const readBack = await fs.readFile(to);
    expect(readBack).toBe("new content");
    await expect(fs.readFile(from)).rejects.toBeTruthy();
  });

  // Fixture copied from src/persistence.test.ts. Invariants that must hold for
  // StateStore.load() to accept it (see persistence.ts's shallow structural
  // check): slots.seats.length must equal slots.capacity (2 seats for
  // capacity 2 here — the check itself only requires an array, but the
  // *content* is what LiveSlots.fromJSON later depends on); gallery.assignments
  // is one entry per gallery.cells (2 assignments for cells: 2), each `cell`
  // referencing a valid 1-based cell id (1..cells) and `slot` referencing a
  // valid slot index (0-based, < capacity, or -1/none per GalleryDirector's
  // own contract — here both point at slot 0, the only occupied-or-not slot
  // index in range); overrides is keyed by the same PersonKey format
  // ("pin:<pin>") the override record's own `personKey` field carries.
  const state: PersistedShowState = {
    version: 3,
    slots: { version: 1, capacity: 2, seats: [null, null] },
    overrides: {
      "pin:1383": { personKey: "pin:1383", displayName: "J.J.", location: "CA", role: "host" }
    },
    gallery: { version: 1, cells: 2, assignments: [{ cell: 1, slot: 0 }, { cell: 2, slot: 0 }] },
    manualBoxes: {},
    lookId: null
  };

  it("a StateStore built over the real fs can save then load the same state", async () => {
    const dir = await uniqueDir();
    const store = new StateStore(join(dir, "state.json"), { fs: nodeStateFs() });
    await store.save(state);
    const loaded = await store.load();
    expect(loaded).toEqual(state);
  });
});
