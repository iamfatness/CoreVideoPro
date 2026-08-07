// show-engine/src/showEngine.test.ts
import { describe, expect, it } from "vitest";
import { ShowEngine } from "./showEngine.js";
import { MockHost } from "./mockHost.js";
import { StateStore } from "./persistence.js";
import { parseShowEngineConfig } from "./config.js";
import type { StateFs } from "./persistence.js";
import type { Clock } from "./clock.js";

function fixedClock(t = 1000): Clock {
  return { now: () => t };
}

function memoryFs(seed: Record<string, string> = {}): StateFs {
  const files = new Map(Object.entries(seed));
  return {
    readFile: async (p) => {
      const v = files.get(p);
      if (v === undefined) throw new Error(`ENOENT ${p}`);
      return v;
    },
    writeFile: async (p, c) => void files.set(p, c),
    rename: async (from, to) => {
      const v = files.get(from);
      if (v !== undefined) {
        files.set(to, v);
        files.delete(from);
      }
    },
    mkdir: async () => undefined
  };
}

const registryLess = parseShowEngineConfig({
  capacity: 8,
  statePath: "/state/show.json",
  galleryCells: 16,
  looks: [
    {
      id: "teatime",
      label: "Teatime",
      scenePreset: "scene-teatime",
      boxes: 2,
      includesHost: true,
      includesReader: false
    },
    {
      id: "banter",
      label: "Banter",
      scenePreset: "scene-banter",
      boxes: 1,
      includesHost: true,
      includesReader: true
    }
  ]
});

function engine(overrides: { host?: MockHost; fs?: StateFs } = {}) {
  const host = overrides.host ?? new MockHost();
  const fs = overrides.fs ?? memoryFs();
  return new ShowEngine({
    config: registryLess,
    host,
    clock: fixedClock(),
    store: new StateStore(registryLess.statePath, { fs })
  });
}

describe("ShowEngine construction", () => {
  /**
   * The invariant this must break on: requiring a Mukana client. A show with
   * no registry is the case the capability model exists to serve; if it cannot
   * even construct, none of that work is reachable.
   */
  it("constructs for a show with no Mukana at all", () => {
    expect(() => engine()).not.toThrow();
  });

  it("starts at revision zero with an empty, valid snapshot", () => {
    const snap = engine().snapshot();
    expect(snap.revision).toBe(0);
    expect(snap.panelists).toEqual([]);
    expect(snap.look).toBeNull();
    expect(snap.tally.mode).toBe("none");
    expect(snap.overlays).toEqual({ nameplates: [], question: null });
  });

  it("resolves every capability to disabled for a registry-less show", () => {
    const caps = engine().snapshot().capabilities;
    expect(caps.registry.state).toBe("disabled");
    expect(caps.handsQueue.state).toBe("disabled");
    expect(caps.questionFeed.state).toBe("disabled");
  });

  it("seats the roster to config capacity", () => {
    expect(engine().snapshot().slots).toHaveLength(8);
  });

  /**
   * The invariant this must break on: handing the host more gallery cells than
   * it declared it can render.
   */
  it("clamps the gallery to what the host can render", () => {
    const small = engine({ host: new MockHost({ maxGalleryCells: 4 }) });
    expect(small.snapshot().gallery).toHaveLength(4);
  });

  it("does not clamp upward when the host can render more than configured", () => {
    const big = engine({ host: new MockHost({ maxGalleryCells: 64 }) });
    expect(big.snapshot().gallery).toHaveLength(16);
  });

  it("refuses a Mukana client the config has no address for", () => {
    expect(
      () =>
        new ShowEngine({
          config: registryLess,
          host: new MockHost(),
          clock: fixedClock(),
          store: new StateStore(registryLess.statePath, { fs: memoryFs() }),
          // @ts-expect-error deliberately wrong: a client with no configured address
          mukana: {}
        })
    ).toThrow(/mukana/i);
  });

  it("emits no host commands before the first tick", () => {
    const host = new MockHost();
    engine({ host });
    expect(host.calls()).toEqual([]);
  });
});

describe("ShowEngine.restore", () => {
  it("returns false when there is no state file", async () => {
    expect(await engine().restore()).toBe(false);
  });

  it("keeps manual box assignments that belong to the current look", async () => {
    const fs = memoryFs({
      "/state/show.json": JSON.stringify({
        version: 3,
        slots: { version: 1, capacity: 8, seats: new Array(8).fill(null) },
        overrides: {},
        gallery: {
          version: 1,
          cells: 16,
          assignments: Array.from({ length: 16 }, (_, i) => ({ cell: i + 1, slot: 0 }))
        },
        manualBoxes: { 1: 3 },
        lookId: "teatime"
      })
    });
    const e = engine({ fs });
    e.setLook("teatime");
    expect(await e.restore()).toBe(true);
    expect(e.snapshot().manualBoxes).toEqual({ 1: 3 });
  });

  /**
   * The invariant this must break on: restoring manual assignments across a
   * look change. Box 1 of one arrangement is not box 1 of another; inheriting
   * them puts the wrong person in the wrong window on a live show.
   */
  it("discards manual box assignments that belong to a different look", async () => {
    const fs = memoryFs({
      "/state/show.json": JSON.stringify({
        version: 3,
        slots: { version: 1, capacity: 8, seats: new Array(8).fill(null) },
        overrides: {},
        gallery: {
          version: 1,
          cells: 16,
          assignments: Array.from({ length: 16 }, (_, i) => ({ cell: i + 1, slot: 0 }))
        },
        manualBoxes: { 1: 3 },
        lookId: "some-other-look"
      })
    });
    const e = engine({ fs });
    e.setLook("teatime");
    expect(await e.restore()).toBe(true);
    expect(e.snapshot().manualBoxes).toEqual({});
  });

  it("rejects a version-2 file as no state at all", async () => {
    const fs = memoryFs({
      "/state/show.json": JSON.stringify({
        version: 2,
        slots: { version: 1, capacity: 8, seats: new Array(8).fill(null) },
        overrides: {},
        gallery: { version: 1, cells: 16, assignments: [] }
      })
    });
    expect(await engine({ fs }).restore()).toBe(false);
  });
});
