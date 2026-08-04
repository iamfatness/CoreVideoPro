import { describe, expect, it } from "vitest";
import { StateStore, type ShowState, type StateFs } from "./persistence.js";

const state: ShowState = {
  version: 1,
  slots: { version: 1, capacity: 2, seats: [null, null] },
  overrides: {
    "1383": { pin: "1383", displayName: "J.J.", location: "CA", role: "host" }
  }
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
});
