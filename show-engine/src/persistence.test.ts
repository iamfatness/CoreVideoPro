import { describe, expect, it } from "vitest";
import { StateStore, type ShowState, type StateFs } from "./persistence.js";

const state: ShowState = {
  version: 2,
  slots: { version: 1, capacity: 2, seats: [null, null] },
  overrides: {
    "pin:1383": { personKey: "pin:1383", displayName: "J.J.", location: "CA", role: "host" }
  },
  gallery: { version: 1, cells: 2, assignments: [{ cell: 1, slot: 0 }, { cell: 2, slot: 0 }] }
};

function fakeFs(seed: Record<string, string> = {}): StateFs & { files: Map<string, string> } {
  const files = new Map<string, string>(Object.entries(seed));
  return {
    files,
    async readFile(path) {
      const content = files.get(path);
      if (content === undefined) {
        const error: NodeJS.ErrnoException = new Error(`ENOENT: ${path}`);
        error.code = "ENOENT";
        throw error;
      }
      return content;
    },
    async writeFile(path, content) {
      files.set(path, content);
    },
    async rename(from, to) {
      const content = files.get(from);
      if (content === undefined) throw new Error(`ENOENT: ${from}`);
      files.delete(from);
      files.set(to, content);
    },
    async mkdir() {
      /* directories are implicit in the fake */
    }
  };
}

describe("StateStore", () => {
  it("writes to a temp file then renames into place", async () => {
    const fs = fakeFs();
    const order: string[] = [];
    const store = new StateStore("/show/state.json", {
      fs: {
        ...fs,
        async writeFile(path, content) {
          order.push(`write:${path}`);
          await fs.writeFile(path, content);
        },
        async rename(from, to) {
          order.push(`rename:${from}->${to}`);
          await fs.rename(from, to);
        }
      }
    });

    await store.save(state);
    expect(order).toEqual([
      "write:/show/state.json.tmp",
      "rename:/show/state.json.tmp->/show/state.json"
    ]);
    expect(fs.files.has("/show/state.json.tmp")).toBe(false);
  });

  it("round-trips a saved state", async () => {
    const fs = fakeFs();
    const store = new StateStore("/show/state.json", { fs });
    await store.save(state);
    expect(await store.load()).toEqual(state);
  });

  it("returns null when no state file exists", async () => {
    const store = new StateStore("/show/state.json", { fs: fakeFs() });
    expect(await store.load()).toBeNull();
  });

  it("returns null for a corrupt state file rather than throwing", async () => {
    const store = new StateStore("/show/state.json", {
      fs: fakeFs({ "/show/state.json": "{ truncated" })
    });
    expect(await store.load()).toBeNull();
  });

  it("returns null for a state file of an unknown version", async () => {
    const store = new StateStore("/show/state.json", {
      fs: fakeFs({ "/show/state.json": JSON.stringify({ ...state, version: 99 }) })
    });
    expect(await store.load()).toBeNull();
  });

  /**
   * A version-1 file is structurally indistinguishable from a current one —
   * the change was inside the override records, which load() does not
   * inspect. Without the version bump this file loads clean and restores
   * overrides keyed "1383", which buildPanelistDb (looking up "pin:1383")
   * never finds: every operator role assignment vanishes on restart, with
   * no error anywhere. Rejecting it is this file's stated policy.
   */
  it("rejects a PIN-keyed version-1 state file rather than silently dropping its roles", async () => {
    const pinKeyed = JSON.stringify({
      version: 1,
      slots: state.slots,
      overrides: {
        "1383": { pin: "1383", displayName: "J.J.", location: "CA", role: "host" }
      },
      gallery: state.gallery
    });
    const store = new StateStore("/show/state.json", {
      fs: fakeFs({ "/show/state.json": pinKeyed })
    });
    expect(await store.load()).toBeNull();
  });

  it("creates the parent directory before writing", async () => {
    const made: string[] = [];
    const fs = fakeFs();
    const store = new StateStore("/show/nested/state.json", {
      fs: {
        ...fs,
        async mkdir(path) {
          made.push(path);
        }
      }
    });
    await store.save(state);
    expect(made).toEqual(["/show/nested"]);
  });

  it("propagates a write failure loudly", async () => {
    const fs = fakeFs();
    const store = new StateStore("/show/state.json", {
      fs: {
        ...fs,
        async writeFile() {
          throw new Error("ENOSPC: no space left on device");
        }
      }
    });
    await expect(store.save(state)).rejects.toThrow(/ENOSPC/);
  });

  it("extracts parent directory for a bare filename", async () => {
    const made: string[] = [];
    const fs = fakeFs();
    const store = new StateStore("state.json", {
      fs: {
        ...fs,
        async mkdir(path) {
          made.push(path);
        }
      }
    });
    await store.save(state);
    expect(made).toEqual(["."]);
  });

  it("extracts parent directory for a root-level POSIX path", async () => {
    const made: string[] = [];
    const fs = fakeFs();
    const store = new StateStore("/state.json", {
      fs: {
        ...fs,
        async mkdir(path) {
          made.push(path);
        }
      }
    });
    await store.save(state);
    expect(made).toEqual(["/"]);
  });

  it("extracts parent directory correctly for a Windows-style path", async () => {
    const made: string[] = [];
    const fs = fakeFs();
    const store = new StateStore("C:\\show\\state.json", {
      fs: {
        ...fs,
        async mkdir(path) {
          made.push(path);
        }
      }
    });
    await store.save(state);
    expect(made).toEqual(["C:\\show"]);
  });

  it("returns null when slots is explicitly null", async () => {
    const store = new StateStore("/show/state.json", {
      fs: fakeFs({ "/show/state.json": JSON.stringify({ version: 2, slots: null, overrides: {} }) })
    });
    expect(await store.load()).toBeNull();
  });

  it("returns null when overrides is explicitly null", async () => {
    const store = new StateStore("/show/state.json", {
      fs: fakeFs({ "/show/state.json": JSON.stringify({ version: 2, slots: state.slots, overrides: null }) })
    });
    expect(await store.load()).toBeNull();
  });

  it("returns null when slots.seats is not an array", async () => {
    const store = new StateStore("/show/state.json", {
      fs: fakeFs({
        "/show/state.json": JSON.stringify({
          version: 2,
          slots: { version: 1, capacity: 2, seats: "garbage" },
          overrides: {}
        })
      })
    });
    expect(await store.load()).toBeNull();
  });

  it("returns null when slots.capacity is not a number", async () => {
    const store = new StateStore("/show/state.json", {
      fs: fakeFs({
        "/show/state.json": JSON.stringify({
          version: 2,
          slots: { version: 1, capacity: "2", seats: [null, null] },
          overrides: {}
        })
      })
    });
    expect(await store.load()).toBeNull();
  });
});

describe("StateStore gallery node", () => {
  const withGallery: ShowState = {
    version: 2,
    slots: { version: 1, capacity: 2, seats: [null, null] },
    overrides: {},
    gallery: { version: 1, cells: 2, assignments: [{ cell: 1, slot: 0 }, { cell: 2, slot: 0 }] }
  };

  it("round-trips a state carrying a gallery", async () => {
    const store = new StateStore("/show/state.json", { fs: fakeFs() });
    await store.save(withGallery);
    expect(await store.load()).toEqual(withGallery);
  });

  it("returns null for a state file with no gallery node", async () => {
    const legacy = JSON.stringify({
      version: 2,
      slots: withGallery.slots,
      overrides: {}
    });
    const store = new StateStore("/show/state.json", {
      fs: fakeFs({ "/show/state.json": legacy })
    });
    expect(await store.load()).toBeNull();
  });

  it("returns null when the gallery node is malformed", async () => {
    const store = new StateStore("/show/state.json", {
      fs: fakeFs({
        "/show/state.json": JSON.stringify({ ...withGallery, gallery: { cells: 2 } })
      })
    });
    expect(await store.load()).toBeNull();
  });
});
