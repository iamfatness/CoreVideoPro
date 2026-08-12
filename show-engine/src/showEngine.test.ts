// show-engine/src/showEngine.test.ts
import { describe, expect, it } from "vitest";
import { ShowEngine } from "./showEngine.js";
import { MockHost } from "./mockHost.js";
import { StateStore } from "./persistence.js";
import { parseShowEngineConfig } from "./config.js";
import { LiveSlotsRestoreError } from "./liveSlots.js";
import { GalleryError } from "./galleryDirector.js";
import { resolvePersonKey } from "./personKey.js";
import type { StateFs, PersistedShowState } from "./persistence.js";
import type { Clock } from "./clock.js";
import type { ZoomEvent } from "./zoomIngest.js";
import type { QueueState } from "./contracts.js";
import { MukanaClient } from "./mukanaClient.js";
import type { FetchResponse, MukanaEndpoint, MukanaHealth } from "./mukanaClient.js";

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

  /**
   * The invariant this must break on: what the published `health` node
   * actually says for a show with no Mukana client. `resolveCapabilities`
   * collapses any unconfigured integration to "disabled" regardless of
   * health content, so the capabilities test above cannot tell a correct
   * "no registry configured" health record from a wrong one (e.g. a
   * fabricated "ok"). `health` is host-facing — an operator surface renders
   * it — so its content is a contract in its own right.
   */
  it("reports every endpoint as failing with 'no registry configured' when there is no Mukana client", () => {
    const health = engine().snapshot().health;
    expect(health).toEqual({
      panelists: { state: "failing", consecutiveFailures: 0, detail: "no registry configured" },
      hands: { state: "failing", consecutiveFailures: 0, detail: "no registry configured" },
      question: { state: "failing", consecutiveFailures: 0, detail: "no registry configured" }
    });
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

/**
 * A coherent, current-version state document for the `registryLess` config:
 * capacity 8 with one seated panelist, a full 16-cell gallery, no look. The
 * restore tests below spread it and change ONE thing, so each test's subject
 * is the difference rather than a wall of re-typed JSON — and so "this
 * document is otherwise fine" is true by construction.
 */
function persistedFixture(): PersistedShowState {
  const seats: PersistedShowState["slots"]["seats"] = new Array(8).fill(null);
  seats[0] = {
    slot: 1,
    panelist: {
      participantId: "p1",
      rawName: "Ann",
      online: true,
      videoOn: true,
      audioOn: true,
      handRaised: false,
      zoomRole: 0,
      displayName: "Ann",
      location: "",
      pin: null,
      hasMukana: false,
      role: "panelist",
      personKey: resolvePersonKey({ participantId: "p1", rawName: "Ann" })
    }
  };
  return {
    version: 3,
    slots: { version: 1, capacity: 8, seats },
    overrides: {},
    gallery: {
      version: 1,
      cells: 16,
      assignments: Array.from({ length: 16 }, (_, index) => ({ cell: index + 1, slot: 0 }))
    },
    manualBoxes: {},
    lookId: null
  };
}

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

  /**
   * The invariant this must break on: a file that passes `StateStore`'s
   * shallow shape check (numeric capacity, seats is an array of the right
   * length) but is not a coherent roster underneath — here, a seat whose
   * `slot` disagrees with its array index. `LiveSlots.fromJSON` catches that
   * deeper corruption and throws `LiveSlotsRestoreError`; `restore()` must
   * let it propagate rather than swallow it into a silent empty roster,
   * which the brief calls out as strictly worse than refusing to start.
   */
  it("propagates a LiveSlotsRestoreError from a structurally-corrupt roster", async () => {
    const seats = new Array(8).fill(null);
    // index 0 must claim slot 1; claiming slot 2 is the deeper corruption
    // the shallow StateStore check cannot see.
    seats[0] = { slot: 2, panelist: { participantId: "p1", rawName: "Test Person" } };
    const fs = memoryFs({
      "/state/show.json": JSON.stringify({
        version: 3,
        slots: { version: 1, capacity: 8, seats },
        overrides: {},
        gallery: {
          version: 1,
          cells: 16,
          assignments: Array.from({ length: 16 }, (_, i) => ({ cell: i + 1, slot: 0 }))
        },
        manualBoxes: {},
        lookId: null
      })
    });
    await expect(engine({ fs }).restore()).rejects.toThrow(LiveSlotsRestoreError);
  });

  /** The baseline the four "it warned" assertions below are only meaningful against: a clean restore says nothing. */
  it("reports no restore warnings for a document that matches this configuration", async () => {
    const fs = memoryFs({ "/state/show.json": JSON.stringify(persistedFixture()) });
    const e = engine({ fs });
    expect(await e.restore()).toBe(true);
    expect(e.snapshot().restoreWarnings).toEqual([]);
  });

  /**
   * Final review, I3. A look removed from the configuration used to be
   * adopted verbatim: `lookById` then resolved nothing, so NO look resolved
   * for the rest of the show, nothing warned, `pagingRefused` stayed null
   * until an explicit `nextGuest`, the orphaned manual boxes were kept, and
   * the phantom id was written straight back on the next save — so it never
   * self-healed and every later restart repeated it. `setLook` throws for
   * the same input; this path may not be quieter than that.
   */
  it("discards a persisted look this configuration no longer defines, loudly", async () => {
    const store = recordingFs({
      "/state/show.json": JSON.stringify({
        ...persistedFixture(),
        manualBoxes: { 1: 3 },
        lookId: "retired-look"
      })
    });
    const e = engine({ fs: store.fs });

    expect(await e.restore()).toBe(true);
    const warnings = e.snapshot().restoreWarnings;
    expect(warnings).toHaveLength(1);
    expect(warnings[0]).toMatch(/retired-look/);

    const snap = await e.tick();
    expect(snap.look).toBeNull();
    // The manual boxes belonged to a look that no longer exists — carrying
    // them to whatever look is selected next would seat the wrong person.
    expect(snap.manualBoxes).toEqual({});
    expect(snap.restoreWarnings).toEqual(warnings);

    // …and it self-heals: the corrected document, not the phantom, is what
    // the next restart will read.
    const written = JSON.parse(store.files.get("/state/show.json") ?? "") as PersistedShowState;
    expect(written.lookId).toBeNull();
    expect(written.manualBoxes).toEqual({});
  });

  /**
   * Final review, I4. `galleryCellCount` is
   * `min(config.galleryCells, host.maxGalleryCells)`, so the SAME good state
   * file restored on a host that renders fewer gallery cells — the spec's
   * own degradation story — threw `GalleryError` out of `restore()` and
   * reported a legitimate configuration change as a corrupt file. The stale
   * geometry is discarded and reported instead; everything that does not
   * depend on the cell count survives.
   */
  it("starts a fresh gallery when the persisted cell count no longer matches this host", async () => {
    const fs = memoryFs({
      "/state/show.json": JSON.stringify({
        ...persistedFixture(),
        manualBoxes: { 1: 3 },
        lookId: "teatime"
      })
    });
    const e = engine({ host: new MockHost({ maxGalleryCells: 4 }), fs });

    expect(await e.restore()).toBe(true);
    const snap = e.snapshot();
    expect(snap.restoreWarnings).toHaveLength(1);
    expect(snap.restoreWarnings[0]).toMatch(/gallery/i);
    expect(snap.gallery).toHaveLength(4);
    // The rest of the document was not collateral damage.
    expect(snap.manualBoxes).toEqual({ 1: 3 });
    expect(snap.slots[0]?.panelist?.participantId).toBe("p1");
  });

  /** The roster twin of the gallery case: an operator edit to `capacity` is a config change, not a corrupt file. */
  it("starts a fresh roster when the persisted capacity no longer matches this configuration", async () => {
    const fs = memoryFs({
      "/state/show.json": JSON.stringify({
        ...persistedFixture(),
        slots: { version: 1, capacity: 4, seats: new Array(4).fill(null) }
      })
    });
    const e = engine({ fs });

    expect(await e.restore()).toBe(true);
    const snap = e.snapshot();
    expect(snap.restoreWarnings).toHaveLength(1);
    expect(snap.restoreWarnings[0]).toMatch(/capacity/i);
    expect(snap.slots).toHaveLength(8);
    expect(snap.slots.every((slot) => slot.panelist === null)).toBe(true);
    // The gallery had nothing to do with the capacity change.
    expect(snap.gallery).toHaveLength(16);
  });

  /**
   * The gallery-side twin of the test above: `cells` matches the configured
   * count and `assignments` has the right length (both pass the shallow
   * check), but an entry's `cell` field disagrees with its position —
   * deeper corruption only `GalleryDirector.fromJSON` catches. Must
   * propagate, not resolve to a silent empty gallery.
   */
  it("propagates a GalleryError from a structurally-corrupt gallery", async () => {
    const assignments = Array.from({ length: 16 }, (_, i) => ({ cell: i + 1, slot: 0 }));
    // index 0 must claim cell 1; claiming cell 5 is the deeper corruption.
    assignments[0] = { cell: 5, slot: 0 };
    const fs = memoryFs({
      "/state/show.json": JSON.stringify({
        version: 3,
        slots: { version: 1, capacity: 8, seats: new Array(8).fill(null) },
        overrides: {},
        gallery: { version: 1, cells: 16, assignments },
        manualBoxes: {},
        lookId: null
      })
    });
    await expect(engine({ fs }).restore()).rejects.toThrow(GalleryError);
  });
});

function joined(id: string, name: string): ZoomEvent {
  return {
    kind: "joined",
    participant: {
      participantId: id,
      rawName: name,
      online: true,
      videoOn: true,
      audioOn: true,
      handRaised: false,
      zoomRole: 0
    }
  };
}

describe("ShowEngine roster tick", () => {
  it("does not seat anyone until a tick runs", () => {
    const e = engine();
    e.onZoomEvent(joined("p1", "Ann"));
    expect(e.snapshot().slots.every((s) => s.panelist === null)).toBe(true);
  });

  it("seats the roster on tick and bumps the revision", async () => {
    const e = engine();
    e.onZoomEvent(joined("p1", "Ann"));
    const snap = await e.tick();
    expect(snap.revision).toBe(1);
    expect(snap.slots[0]?.panelist?.displayName).toBe("Ann");
  });

  it("seats in participant-id order regardless of arrival order", async () => {
    const e = engine();
    e.onZoomEvent(joined("p3", "Cy"));
    e.onZoomEvent(joined("p1", "Ann"));
    e.onZoomEvent(joined("p2", "Bo"));
    const snap = await e.tick();
    expect(snap.slots.slice(0, 3).map((s) => s.panelist?.participantId)).toEqual([
      "p1",
      "p2",
      "p3"
    ]);
  });

  /**
   * The invariant this must break on: calling rebuild when only a property
   * changed. A guest toggling their camera must not reseat the room.
   */
  it("holds seats still when a participant toggles video", async () => {
    const e = engine();
    e.onZoomEvent(joined("p1", "Ann"));
    e.onZoomEvent(joined("p2", "Bo"));
    await e.tick();
    e.onZoomEvent({ kind: "video", participantId: "p1", on: false });
    const snap = await e.tick();
    expect(snap.slots[0]?.panelist?.participantId).toBe("p1");
    expect(snap.slots[0]?.panelist?.videoOn).toBe(false);
    expect(snap.slots[1]?.panelist?.participantId).toBe("p2");
  });

  /**
   * A Zoom departure does NOT vacate a seat. Owner ruling, 2026-08-06: the seat
   * is held and flagged offline, so a connection blip does not drop a panelist
   * off air and a reconnect returns them to the SAME slot. Clearing a seat is an
   * explicit operator action, exactly as it was in the Isadora patch this ports.
   *
   * This matches the two modules underneath: `ZoomIngest` keeps a departed
   * participant marked offline "so they can be restored on reconnect"
   * (zoomIngest.ts:52-55), and `LiveSlots.refresh` documents that a vanished
   * participant "keeps their seat but is marked offline — visibly gone rather
   * than silently dropped" (liveSlots.ts:114-118).
   *
   * The invariant this must break on: any code that vacates a seat on an
   * offline flag. That makes a reconnecting guest permanently unseatable,
   * because a departure never changes the id set and `refresh` only re-pulls
   * ALREADY-seated panelists.
   */
  it("holds the seat and marks it offline when someone leaves", async () => {
    const e = engine();
    e.onZoomEvent(joined("p1", "Ann"));
    e.onZoomEvent(joined("p2", "Bo"));
    await e.tick();
    e.onZoomEvent({ kind: "left", participantId: "p1" });
    const snap = await e.tick();
    expect(snap.slots[0]?.panelist?.participantId).toBe("p1");
    expect(snap.slots[0]?.panelist?.online).toBe(false);
    expect(snap.slots[1]?.panelist?.participantId).toBe("p2");
  });

  it("returns a reconnecting panelist to the same slot", async () => {
    const e = engine();
    e.onZoomEvent(joined("p1", "Ann"));
    e.onZoomEvent(joined("p2", "Bo"));
    await e.tick();
    e.onZoomEvent({ kind: "left", participantId: "p1" });
    await e.tick();
    e.onZoomEvent(joined("p1", "Ann"));
    const snap = await e.tick();
    expect(snap.slots[0]?.panelist?.participantId).toBe("p1");
    expect(snap.slots[0]?.panelist?.online).toBe(true);
  });

  /**
   * The refresh-vs-rebuild branch, pinned in BOTH directions. Mutation testing
   * showed the committed suite passed under "always rebuild" AND under "always
   * refresh" — the second means a mid-show join is never seated and nothing
   * fails. Every roster test staged its joins before the first tick.
   */
  it("seats a guest who joins after the first tick", async () => {
    const e = engine();
    e.onZoomEvent(joined("p1", "Ann"));
    await e.tick();
    e.onZoomEvent(joined("p2", "Bo"));
    const snap = await e.tick();
    expect(snap.slots[1]?.panelist?.participantId).toBe("p2");
  });

  /**
   * The `sameIdSet` comparison must be a real set-membership check, not a
   * size check. A full roster resync (a "roster" event, unlike "left") can
   * swap membership while keeping the same COUNT — one person omitted, a
   * different one added. `a.size === b.size` alone would misclassify this
   * as "unchanged" and take the `refresh` path, which never seats the new
   * arrival because `refresh` only re-pulls already-seated panelists.
   */
  it("rebuilds on a same-size membership swap, not just a size change", async () => {
    const e = engine();
    e.onZoomEvent(joined("p1", "Ann"));
    e.onZoomEvent(joined("p2", "Bo"));
    await e.tick();
    e.onZoomEvent({
      kind: "roster",
      participants: [
        {
          participantId: "p2",
          rawName: "Bo",
          online: true,
          videoOn: true,
          audioOn: true,
          handRaised: false,
          zoomRole: 0
        },
        {
          participantId: "p3",
          rawName: "Cy",
          online: true,
          videoOn: true,
          audioOn: true,
          handRaised: false,
          zoomRole: 0
        }
      ]
    });
    const snap = await e.tick();
    expect(snap.slots.some((s) => s.panelist?.participantId === "p3")).toBe(true);
  });

  /**
   * `unseatedPanelists` is written only by `rebuild`'s return value — a
   * `refresh`-path tick never touches it. Without reconciling it against
   * this tick's fresh `panelists` map too, an overflow panelist's own
   * property changes (video/audio/role) are invisible on the published
   * snapshot until the next roster-changing rebuild, even though the
   * engine already has fresher data for them in hand.
   */
  it("reconciles unseated panelists on a refresh-path tick, not just at the rebuild that dropped them", async () => {
    const e = engine();
    for (let i = 1; i <= 9; i += 1) {
      e.onZoomEvent(joined(`p${i}`, `Guest ${i}`));
    }
    const first = await e.tick();
    const overflowId = first.unseated[0]?.participantId;
    expect(overflowId).toBeDefined();

    // A property-only change on the overflow participant: the id set does
    // not change, so this tick takes the `refresh` path.
    e.onZoomEvent({ kind: "video", participantId: overflowId as string, on: false });
    const snap = await e.tick();
    expect(snap.unseated[0]?.participantId).toBe(overflowId);
    expect(snap.unseated[0]?.videoOn).toBe(false);
  });

  /**
   * The invariant this must break on: discarding rebuild's return value.
   * Overflow that is dropped silently means people vanish from a live show
   * with nothing anywhere reporting it.
   */
  it("reports panelists that did not fit rather than dropping them", async () => {
    const e = engine();
    const ids: string[] = [];
    for (let i = 1; i <= 10; i += 1) {
      ids.push(`p${i}`);
      e.onZoomEvent(joined(`p${i}`, `Guest ${i}`));
    }
    const snap = await e.tick();
    const seated = snap.slots.flatMap((s) => (s.panelist ? [s.panelist.participantId] : []));
    expect(seated).toHaveLength(8);
    expect(snap.unseated).toHaveLength(2);
    // Nobody vanishes: seated ∪ unseated is the whole roster, with no overlap.
    expect([...seated, ...snap.unseated.map((p) => p.participantId)].sort()).toEqual(
      [...ids].sort()
    );
  });

  it("debounces persistence against the injected clock", async () => {
    let t = 1000;
    const fs = memoryFs();
    const writes: string[] = [];
    const counting: StateFs = {
      ...fs,
      writeFile: async (p, c) => {
        writes.push(p);
        return fs.writeFile(p, c);
      }
    };
    const e = new ShowEngine({
      config: registryLess,
      host: new MockHost(),
      clock: { now: () => t },
      store: new StateStore(registryLess.statePath, { fs: counting })
    });
    e.onZoomEvent(joined("p1", "Ann"));
    await e.tick();
    const afterFirst = writes.length;
    e.onZoomEvent(joined("p2", "Bo"));
    await e.tick();
    expect(writes.length).toBe(afterFirst);
    t += 1001;
    e.onZoomEvent(joined("p3", "Cy"));
    await e.tick();
    expect(writes.length).toBeGreaterThan(afterFirst);
  });

  /**
   * Task 4's review found `EMPTY_QUEUE` was a module-level constant shared
   * across every `ShowEngine` instance — an in-place `.push` on one
   * instance's queue would have leaked into every other instance, including
   * torn-down ones. It was fixed with an `emptyQueue()` factory
   * (mirroring `overlayDirector.ts`'s `emptyState()`), but that fix was
   * verified with a throwaway test that was since deleted — nothing
   * committed pinned it. `tick()` is where the engine first touches
   * `queue` in earnest, so this is that permanent regression test: mutate
   * one instance's internal queue state in place and confirm a second
   * instance is unaffected. A future "simplification" that hoists
   * `emptyQueue()`'s return back to a shared module constant would
   * type-check and leave every other test in this file green — only this
   * one would catch it.
   */
  it("never shares queue state between ShowEngine instances", () => {
    const a = engine();
    const b = engine();
    (a as unknown as { queue: QueueState }).queue.previous.push("mutated-on-a");
    expect((b as unknown as { queue: QueueState }).queue.previous).toEqual([]);
    expect(a.snapshot().queue.previous).toEqual(["mutated-on-a"]);
    expect(b.snapshot().queue.previous).toEqual([]);
  });

  /**
   * Fix round 1, Minor: the flag-conflation bug caught pre-commit (reusing
   * the lingering "unsaved state" flag to also decide whether the seat step
   * runs) was fixed but unpinned — nothing in the committed suite failed
   * when the two roles were collapsed back into one field, because refresh
   * and rebuild are idempotent for unchanged inputs, so a spurious extra
   * seat-step run produces no different `ShowSnapshot` content. This test
   * reaches into the two private flags directly (the same technique as the
   * queue-isolation test above) to pin that `otherInputsChanged` is
   * consumed by every `tick()` regardless of whether that tick manages to
   * save, while `pendingPersist` correctly lingers until an actual save.
   */
  it("keeps the debounce-lingering persist flag separate from the per-tick reseat flag", async () => {
    const e = engine();
    e.onZoomEvent(joined("p1", "Ann"));
    await e.tick(); // unthrottled first save: pendingPersist -> false
    e.setLook("teatime"); // sets BOTH flags true
    await e.tick(); // debounce not elapsed (fixed clock): save skipped, pendingPersist lingers
    const internals = e as unknown as { otherInputsChanged: boolean; pendingPersist: boolean };
    expect(internals.pendingPersist).toBe(true);
    expect(internals.otherInputsChanged).toBe(false);
  });

  /**
   * Fix round 1, Minor: `setOverride`'s exclusive-role demotion
   * (`assignExclusiveRole`) had zero coverage anywhere in the package — it
   * is the only thing preventing two simultaneous hosts.
   */
  it("demotes the prior host when a new host override is set", async () => {
    const e = engine();
    e.onZoomEvent(joined("p1", "Ann"));
    e.onZoomEvent(joined("p2", "Bo"));
    await e.tick();

    e.setOverride({
      personKey: resolvePersonKey({ participantId: "p1", rawName: "Ann" }),
      displayName: "Ann",
      location: "",
      role: "host"
    });
    await e.tick();

    e.setOverride({
      personKey: resolvePersonKey({ participantId: "p2", rawName: "Bo" }),
      displayName: "Bo",
      location: "",
      role: "host"
    });
    const snap = await e.tick();

    const p1 = snap.slots.find((s) => s.panelist?.participantId === "p1")?.panelist;
    const p2 = snap.slots.find((s) => s.panelist?.participantId === "p2")?.panelist;
    expect(p1?.role).toBe("panelist");
    expect(p2?.role).toBe("host");
  });
});

describe("ShowEngine restore + non-roster input (fix round 1, Important 3)", () => {
  /**
   * `restore()` seeds `LiveSlots` from disk, but `ZoomIngest` starts empty
   * regardless — the real Zoom roster has not reconnected yet. Before this
   * guard, a non-roster input (`setLook` here) alone would run the seat
   * step against that empty `ZoomIngest` snapshot: `rebuild([])` cleared
   * every restored seat, and the next debounced save persisted the wipe
   * over the good state file. Reproduced without the guard: two restored,
   * seated panelists, then `setLook`, then one `tick()` — every seat went
   * null and the persisted file held 8 nulls. An operator changing looks
   * after a restart, before Zoom reconnects, must not destroy the show.
   */
  it("does not wipe a restored roster when a non-roster input runs the seat step before Zoom reconnects", async () => {
    const seats = new Array(8).fill(null);
    seats[0] = { slot: 1, panelist: { participantId: "p1", rawName: "Ann" } };
    seats[1] = { slot: 2, panelist: { participantId: "p2", rawName: "Bo" } };
    const fs = memoryFs({
      "/state/show.json": JSON.stringify({
        version: 3,
        slots: { version: 1, capacity: 8, seats },
        overrides: {},
        gallery: {
          version: 1,
          cells: 16,
          assignments: Array.from({ length: 16 }, (_, i) => ({ cell: i + 1, slot: 0 }))
        },
        manualBoxes: {},
        lookId: null
      })
    });
    const e = engine({ fs });
    expect(await e.restore()).toBe(true);

    e.setLook("teatime"); // a non-roster input, before Zoom has reconnected
    const snap = await e.tick();

    expect(snap.slots[0]?.panelist?.participantId).toBe("p1");
    expect(snap.slots[1]?.panelist?.participantId).toBe("p2");

    const persisted = JSON.parse(await fs.readFile("/state/show.json")) as PersistedShowState;
    expect(persisted.slots.seats[0]?.panelist.participantId).toBe("p1");
    expect(persisted.slots.seats[1]?.panelist.participantId).toBe("p2");
  });

  /**
   * Fix round 2. `restore()` seeding `lastSeatedParticipantIds`
   * (showEngine.ts:290) is itself what makes `refresh` vs `rebuild`
   * observable, closing a second surviving mutant from round 1's report.
   *
   * The invariant this must break on, in two different ways:
   *
   * 1. `rebuild` sorts by `participantId` (p1 before p2) — if the seat step
   *    ever took `rebuild` here instead of `refresh`, it would silently
   *    re-sort this restored arrangement (p2 at slot 1, p1 at slot 2) back
   *    into id order (p1 at slot 1, p2 at slot 2), discarding whatever the
   *    operator arranged in the prior session.
   * 2. Deleting the `:290` seeding block reintroduces exactly that: with no
   *    prior id set to compare against, Zoom's first post-restart commit
   *    looks like "no prior arrangement to hold" (the very-first-tick
   *    case), so the seat step takes `rebuild` even though the reconnecting
   *    roster is byte-for-byte the one that was just restored — the same
   *    wipe-guard blind spot round 1 closed for an EMPTY roster, silently
   *    reopened here for a non-empty one.
   */
  it("holds a restored seating order when Zoom reconnects with the same roster", async () => {
    const seats = new Array(8).fill(null);
    seats[0] = { slot: 1, panelist: { participantId: "p2", rawName: "Bo" } };
    seats[1] = { slot: 2, panelist: { participantId: "p1", rawName: "Ann" } };
    const fs = memoryFs({
      "/state/show.json": JSON.stringify({
        version: 3,
        slots: { version: 1, capacity: 8, seats },
        overrides: {},
        gallery: {
          version: 1,
          cells: 16,
          assignments: Array.from({ length: 16 }, (_, i) => ({ cell: i + 1, slot: 0 }))
        },
        manualBoxes: {},
        lookId: null
      })
    });
    const e = engine({ fs });
    expect(await e.restore()).toBe(true);

    e.onZoomEvent(joined("p1", "Ann"));
    e.onZoomEvent(joined("p2", "Bo"));
    const snap = await e.tick();

    expect(snap.slots[0]?.panelist?.participantId).toBe("p2");
    expect(snap.slots[1]?.panelist?.participantId).toBe("p1");
  });
});

describe("ShowEngine derived layers", () => {
  it("resolves the selected look and reports it on the snapshot", async () => {
    const e = engine();
    e.onZoomEvent(joined("p1", "Ann"));
    e.setLook("teatime");
    const snap = await e.tick();
    expect(snap.look?.lookId).toBe("teatime");
    expect(snap.look?.scenePreset).toBe("scene-teatime");
  });

  it("rejects an unknown look id", () => {
    expect(() => engine().setLook("nope")).toThrow(/nope/);
  });

  /**
   * Fix round 1, Minor: the original version of this test asserted only
   * `mode === "look"` with nobody seated, so it passed whether or not the
   * on-air slot derivation was correct at all. `assignBox` puts Ann (seated
   * at slot 1, the only participant) into the look's box 1 — under manual
   * fill (this engine's default: `teatime` is `boxFill: "queue"`, but the
   * registry-less config's `handsQueue` capability is `disabled`, which
   * `effectiveBoxFill` collapses to manual regardless) — so the derivation
   * has a real, checkable answer: slot 1 on air, nobody else.
   */
  it("derives tally from the program source and the resolved look", async () => {
    const e = engine();
    e.onZoomEvent(joined("p1", "Ann"));
    e.setLook("teatime");
    e.assignBox(1, 1);
    await e.tick();
    e.directCut({ kind: "look", lookId: "teatime" });
    const snap = await e.tick();
    expect(snap.tally.mode).toBe("look");
    expect(snap.tally.onAirSlots).toEqual([1]);
    expect(snap.tally.onAirParticipantIds).toEqual(["p1"]);
  });

  it("translates the active speaker to a roster slot for tally", async () => {
    const e = engine();
    e.onZoomEvent(joined("p1", "Ann"));
    e.onZoomEvent(joined("p2", "Bo"));
    await e.tick();
    e.directCut({ kind: "activeSpeaker" });
    e.setActiveSpeakerFollow(true);
    e.onActiveSpeaker("p2");
    const snap = await e.tick();
    expect(snap.tally.mode).toBe("activeSpeaker");
    expect(snap.tally.onAirSlots).toEqual([2]);
  });

  it("clears manual box assignments when the look changes", async () => {
    const e = engine();
    e.setLook("teatime");
    e.assignBox(1, 3);
    await e.tick();
    expect(e.snapshot().manualBoxes).toEqual({ 1: 3 });
    e.setLook("banter");
    expect(e.snapshot().manualBoxes).toEqual({});
  });

  it("keeps manual box assignments when the same look is re-selected", async () => {
    const e = engine();
    e.setLook("teatime");
    e.assignBox(1, 3);
    e.setLook("teatime");
    expect(e.snapshot().manualBoxes).toEqual({ 1: 3 });
  });

  it("refuses paging under manual fill instead of throwing", async () => {
    const e = engine();
    e.setLook("teatime");
    await e.tick();
    expect(() => e.nextGuest()).not.toThrow();
    const snap = await e.tick();
    expect(snap.page).toBe(0);
    expect(snap.pagingRefused).toMatch(/manual/i);
  });

  /**
   * Transport degradation. The invariant this must break on: emitting a
   * preview or cut to a host that declared it has no preview bus.
   */
  it("routes cut to a direct cut on a host with no preview bus", async () => {
    const host = new MockHost({ hasPreviewBus: false });
    const e = engine({ host });
    e.setLook("teatime");
    await e.tick();
    host.clear();
    e.setPreview({ kind: "look", lookId: "teatime" });
    e.cut();
    await e.tick();
    expect(host.callsOfKind("setPreview")).toEqual([]);
    expect(host.callsOfKind("cut")).toEqual([]);
    expect(e.snapshot().program.program).toEqual({ kind: "look", lookId: "teatime" });
  });
});

/**
 * Builds a `ShowEngine` for the property below: `capacity: 8`, one look
 * ("teatime", 2 boxes) whose `boxFill` is the given fill strategy, and a
 * Mukana setup whose `hands` endpoint resolves to the given capability
 * state. `state: "disabled"` means the show was never configured for the
 * hands integration at all (no `mukana` client is supplied — its health
 * would be irrelevant, since `resolveCapability` collapses an unconfigured
 * integration to `disabled` regardless of health content). `"available"`/
 * `"unavailable"` configure the integration on and supply a fake
 * `MukanaClient`-shaped `health` getter reporting `ok`/`failing` for
 * `hands` (the other two endpoints report `ok` so only the capability under
 * test varies). The queue is seeded directly (there is no public setter for
 * the raw hands-queue state yet — that lands with the polling wiring) with
 * 5 PINs, which spans exactly 3 pages against a 2-box look
 * (`ceil(5 / 2) = 3`).
 */
function fakeMukanaWithHandsState(state: "ok" | "failing"): MukanaClient {
  const ok: MukanaHealth = { state: "ok", consecutiveFailures: 0, detail: null };
  const failing: MukanaHealth = { state: "failing", consecutiveFailures: 3, detail: "hands endpoint down" };
  const health: Record<MukanaEndpoint, MukanaHealth> = {
    panelists: ok,
    hands: state === "ok" ? ok : failing,
    question: ok
  };
  // Task 9: `tick()` now polls, and the very first tick is always due (there
  // is no prior poll to measure a delay against), so this fake needs real
  // fetch methods even though it exists only to drive capability
  // resolution. Every one returns `invalid` unconditionally — never
  // `data` — so `tick()`'s "leave last-good data in place" rule means none
  // of them can ever overwrite `engineWithFill`'s hand-seeded `this.queue`;
  // only the separately-stubbed `health` getter (not these return values)
  // drives the capability these tests actually assert on. Fix round 1,
  // Minor 8: `fetchPanelists`/`fetchQuestion` are stubbed too (not just
  // `fetchHands`) so a future test flipping `integrations.registry`/
  // `questionFeed` on this fake gets an inert response instead of a
  // runtime `TypeError` from `tick()` with no type-level warning.
  return {
    health,
    nextDelayMs: () => Number.MAX_SAFE_INTEGER,
    fetchPanelists: async () => ({ kind: "invalid", reason: "fakeMukanaWithHandsState: no real endpoint" }),
    fetchHands: async () => ({ kind: "invalid", reason: "fakeMukanaWithHandsState: no real endpoint" }),
    fetchQuestion: async () => ({ kind: "invalid", reason: "fakeMukanaWithHandsState: no real endpoint" })
  } as unknown as MukanaClient;
}

function engineWithFill(fill: "queue" | "manual", state: "available" | "unavailable" | "disabled") {
  const handsEnabled = state !== "disabled";
  const config = parseShowEngineConfig({
    capacity: 8,
    statePath: "/state/show.json",
    galleryCells: 16,
    integrations: { handsQueue: handsEnabled },
    ...(handsEnabled
      ? { mukana: { baseUrl: "https://example.com/rest.php", event: "officehours" } }
      : {}),
    looks: [
      {
        id: "teatime",
        label: "Teatime",
        scenePreset: "scene-teatime",
        boxes: 2,
        includesHost: true,
        includesReader: false,
        boxFill: fill
      }
    ]
  });

  const e = new ShowEngine({
    config,
    host: new MockHost(),
    clock: fixedClock(),
    store: new StateStore(config.statePath, { fs: memoryFs() }),
    mukana: handsEnabled ? fakeMukanaWithHandsState(state === "available" ? "ok" : "failing") : undefined
  });

  (e as unknown as { queue: QueueState }).queue = {
    previous: [],
    current: "1001",
    upcoming: ["1002", "1003", "1004", "1005"]
  };

  return e;
}

/**
 * THE PLAN 4 OBLIGATION, discharged as a property.
 *
 * Quantified over — and the quantification is the point, because Plan 4's own
 * property held page constant at 0 and that is exactly where its bug hid:
 *   - page over -2..pageCount+2, including both out-of-range directions;
 *   - all three capability states for handsQueue;
 *   - both boxFill strategies.
 *
 * The invariant it must break on: clampPage and resolveLook receiving
 * different capability values, or either being called without one. Any of
 * those makes some (page, capability) pair throw out of tick().
 *
 * Fix round 1, Finding 1: the original two assertions here
 * (`snap.page >= 0`, `snap.page < snap.look.pageCount`) are self-referential
 * — under a mutation that drops the shared capability from BOTH calls (or
 * from `clampPage` alone), `snap.page` and `snap.look.pageCount` are both
 * products of the SAME wrong computation and agree with each other anyway,
 * so neither assertion ever reds. `engineWithFill` seeds exactly 5 PINs
 * against a 2-box look, so the TRUE page count is externally computable —
 * 3 when queue fill is actually engaged (`fill === "queue" &&
 * state === "available"`; anything else collapses to manual, one page) —
 * and comparing against that independently-known value is what makes a
 * wrong-but-internally-consistent computation visible.
 */
describe("clampPage and resolveLook always agree", () => {
  const states = ["available", "unavailable", "disabled"] as const;

  for (const fill of ["queue", "manual"] as const) {
    for (const state of states) {
      for (const page of [-2, -1, 0, 1, 2, 3, 4]) {
        it(`survives page ${page} with ${fill} fill and a ${state} hands queue`, async () => {
          const e = engineWithFill(fill, state);
          e.setLook("teatime");
          e.setPage(page);
          await expect(e.tick()).resolves.toBeDefined();
          const snap = e.snapshot();
          expect(snap.page).toBeGreaterThanOrEqual(0);
          expect(snap.page).toBeLessThan(snap.look?.pageCount ?? 1);

          const expectedPageCount = fill === "queue" && state === "available" ? 3 : 1;
          expect(snap.look?.pageCount).toBe(expectedPageCount);
          expect(snap.page).toBe(Math.min(Math.max(page, 0), expectedPageCount - 1));
        });
      }
    }
  }
});

/**
 * A `MukanaClient`-shaped fake whose `health.hands` is backed by a mutable
 * closure variable rather than a fixed object — unlike
 * `fakeMukanaWithHandsState` above (a frozen snapshot, right for the
 * property test's per-case engines), this one can change ITS ANSWER
 * mid-test, the same way a real client's health getter changes after an
 * async fetch resolves. Starts `"failing"` (unavailable); `setHandsOk`
 * flips it to `"ok"` (available) for every subsequent read.
 */
function fakeMukanaWithMutableHands(): { client: MukanaClient; setHandsOk: () => void } {
  let handsHealth: MukanaHealth = { state: "failing", consecutiveFailures: 1, detail: "hands endpoint down" };
  const ok: MukanaHealth = { state: "ok", consecutiveFailures: 0, detail: null };
  const client = {
    get health(): Record<MukanaEndpoint, MukanaHealth> {
      return { panelists: ok, hands: handsHealth, question: ok };
    },
    // Task 9: same reasoning as `fakeMukanaWithHandsState` above — the
    // first tick is always due, so this needs real (but inert) fetches too.
    // Fix round 1, Minor 8: all three, not just `fetchHands`.
    nextDelayMs: () => Number.MAX_SAFE_INTEGER,
    fetchPanelists: async () => ({ kind: "invalid", reason: "fakeMukanaWithMutableHands: no real endpoint" }),
    fetchHands: async () => ({ kind: "invalid", reason: "fakeMukanaWithMutableHands: no real endpoint" }),
    fetchQuestion: async () => ({ kind: "invalid", reason: "fakeMukanaWithMutableHands: no real endpoint" })
  } as unknown as MukanaClient;
  return {
    client,
    setHandsOk: () => {
      handsHealth = { state: "ok", consecutiveFailures: 0, detail: null };
    }
  };
}

describe("ShowEngine fix round 1 (2026-08-07)", () => {
  /**
   * Finding 2: `tick()` must publish the snapshot from the SAME
   * `caps`/`health` it resolved capabilities with at the top of that tick,
   * never re-resolving from the live health getter after `await
   * this.store.save(...)`. This engine's `writeFile` flips the hands
   * endpoint from failing to ok as a side effect of the FIRST save this
   * engine performs — simulating an async Mukana poll landing during that
   * await — so a broken implementation would publish `capabilities.handsQueue
   * = available` (the POST-save value) alongside `look.boxFill = "manual"`,
   * `look.pageCount = 1` (computed PRE-save, before the flip): a snapshot
   * that contradicts itself. The fix makes both come from the one
   * pre-save resolution.
   */
  it("keeps capabilities and the resolved look in agreement even when health changes during the save", async () => {
    const { client, setHandsOk } = fakeMukanaWithMutableHands();
    const config = parseShowEngineConfig({
      capacity: 8,
      statePath: "/state/show.json",
      galleryCells: 16,
      integrations: { handsQueue: true },
      mukana: { baseUrl: "https://example.com/rest.php", event: "officehours" },
      looks: [
        {
          id: "teatime",
          label: "Teatime",
          scenePreset: "scene-teatime",
          boxes: 2,
          includesHost: true,
          includesReader: false
        }
      ]
    });

    const backingFs = memoryFs();
    let flipped = false;
    const flippingFs: StateFs = {
      ...backingFs,
      writeFile: async (p, c) => {
        if (!flipped) {
          flipped = true;
          setHandsOk();
        }
        return backingFs.writeFile(p, c);
      }
    };

    const e = new ShowEngine({
      config,
      host: new MockHost(),
      clock: fixedClock(),
      store: new StateStore(config.statePath, { fs: flippingFs }),
      mukana: client
    });
    (e as unknown as { queue: QueueState }).queue = {
      previous: [],
      current: "1001",
      upcoming: ["1002", "1003", "1004", "1005"]
    };

    e.setLook("teatime"); // pendingPersist true; first save is always unthrottled, so it runs THIS tick
    const snap = await e.tick();

    // The tick's OWN pre-save resolution: hands was failing when `caps` was
    // resolved, so boxFill fell back to manual and pageCount to 1.
    expect(snap.capabilities.handsQueue.state).toBe("unavailable");
    expect(snap.look?.boxFill).toBe("manual");
    expect(snap.look?.pageCount).toBe(1);

    // The live getter now reads "ok" (the save-time flip already happened),
    // proving this isn't passing merely because the flip never fired.
    expect(client.health.hands.state).toBe("ok");
  });

  /**
   * Finding 3: an out-of-range `nextGuest`/`prevGuest` must refuse and
   * leave `this.page` untouched, not silently advance past the end and
   * rely on `tick()`'s `clampPage` to pull it back with no signal to the
   * operator that their move did nothing new.
   */
  it("refuses to page past the last page instead of relying on tick() to clamp it back silently", async () => {
    const e = engineWithFill("queue", "available"); // pageCount 3: valid pages 0,1,2
    e.setLook("teatime");
    await e.tick();
    e.nextGuest(); // 0 -> 1
    e.nextGuest(); // 1 -> 2 (last valid page)
    e.nextGuest(); // would be 3: refuse
    const snap = await e.tick();
    expect(snap.page).toBe(2);
    expect(snap.pagingRefused).toMatch(/out of range/i);
  });

  it("refuses to page before the first page instead of silently doing nothing", async () => {
    const e = engineWithFill("queue", "available");
    e.setLook("teatime");
    await e.tick(); // page 0
    e.prevGuest(); // would be -1: refuse
    const snap = await e.tick();
    expect(snap.page).toBe(0);
    expect(snap.pagingRefused).toMatch(/out of range/i);
  });

  /**
   * Finding 4: `pagingRefused` must not outlive the condition that caused
   * it. Once the hands feed recovers and the active look's boxes are
   * actually filling from the queue again, a STALE refusal recorded while
   * that wasn't true must clear itself on the very next tick — nothing
   * else would ever clear it, since only `nextGuest`/`prevGuest` write it
   * and neither runs every tick.
   */
  it("clears a stale paging refusal once the hands feed recovers", async () => {
    const { client, setHandsOk } = fakeMukanaWithMutableHands(); // starts failing
    const config = parseShowEngineConfig({
      capacity: 8,
      statePath: "/state/show.json",
      galleryCells: 16,
      integrations: { handsQueue: true },
      mukana: { baseUrl: "https://example.com/rest.php", event: "officehours" },
      looks: [
        {
          id: "teatime",
          label: "Teatime",
          scenePreset: "scene-teatime",
          boxes: 2,
          includesHost: true,
          includesReader: false
        }
      ]
    });
    const e = new ShowEngine({
      config,
      host: new MockHost(),
      clock: fixedClock(),
      store: new StateStore(config.statePath, { fs: memoryFs() }),
      mukana: client
    });
    (e as unknown as { queue: QueueState }).queue = {
      previous: [],
      current: "1001",
      upcoming: ["1002", "1003", "1004", "1005"]
    };

    e.setLook("teatime");
    await e.tick(); // hands failing -> manual fill
    e.nextGuest(); // refused: manual fill
    let snap = await e.tick();
    expect(snap.pagingRefused).toMatch(/manual/i);

    setHandsOk(); // the feed recovers between ticks, as a real poll would
    snap = await e.tick(); // this tick's own resolution is now queue fill
    expect(snap.look?.boxFill).toBe("queue");
    expect(snap.pagingRefused).toBeNull();
  });

  /** Finding 4's other writer: a look change must not leave a stale refusal attributed to the look it just left. */
  it("clears a stale paging refusal on setLook, even to a look that is also manual fill", async () => {
    const e = engine(); // registry-less: handsQueue disabled, every look is effectively manual
    e.setLook("teatime");
    await e.tick();
    e.nextGuest(); // refused: manual fill
    expect(e.snapshot().pagingRefused).toMatch(/manual/i);
    e.setLook("banter");
    expect(e.snapshot().pagingRefused).toBeNull();
  });

  /** "Also fix": a cold restart (no `setLook()` before `restore()`) must resume the persisted look, not resolve none until an operator re-selects one. */
  it("resumes the persisted look on a cold restart with no setLook() first", async () => {
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
    expect(await e.restore()).toBe(true); // NO setLook() call before this, unlike the pinned manualBoxes tests
    const snap = await e.tick();
    expect(snap.look?.lookId).toBe("teatime");
    expect(snap.manualBoxes).toEqual({ 1: 3 });
  });

  /** Minor: `assignBox` rejects a box number outside the active look's range instead of silently persisting garbage. */
  it("rejects a manual box assignment outside the active look's box range", async () => {
    const e = engine();
    e.setLook("teatime"); // 2 boxes
    expect(() => e.assignBox(3, 1)).toThrow(/out of range/);
    expect(() => e.assignBox(0, 1)).toThrow();
  });

  /** Minor: `setPage` rejects a non-integer at the call site rather than letting it surface as a thrown error in a later tick. */
  it("rejects a non-integer page from setPage", () => {
    expect(() => engine().setPage(1.5)).toThrow(/integer/);
  });
});

/**
 * A `StateFs` that keeps its files where the test can read them, counts
 * writes, and can hold ONE write open until the test releases it. The
 * held-open write is what makes the "a change landed during the save" case
 * (final review, I2) reachable at all: with a synchronous in-memory fs the
 * window does not exist, which is why the whole persistence-writer half of
 * this engine went unpinned.
 */
function recordingFs(seed: Record<string, string> = {}): {
  fs: StateFs;
  files: Map<string, string>;
  writeCount: () => number;
  holdNextWrite: () => () => void;
} {
  const files = new Map(Object.entries(seed));
  let writes = 0;
  let gate: Promise<void> | null = null;

  const fs: StateFs = {
    readFile: async (p) => {
      const v = files.get(p);
      if (v === undefined) throw new Error(`ENOENT ${p}`);
      return v;
    },
    writeFile: async (p, c) => {
      writes += 1;
      if (gate !== null) {
        const held = gate;
        gate = null;
        await held;
      }
      files.set(p, c);
    },
    rename: async (from, to) => {
      const v = files.get(from);
      if (v !== undefined) {
        files.set(to, v);
        files.delete(from);
      }
    },
    mkdir: async () => undefined
  };

  return {
    fs,
    files,
    writeCount: () => writes,
    holdNextWrite: () => {
      let open: () => void = () => undefined;
      gate = new Promise<void>((resolve) => {
        open = resolve;
      });
      return open;
    }
  };
}

/** Mirrors `showEngine.ts`'s module-private `SAVE_DEBOUNCE_MS`; advancing by it is what lets the next tick write. */
const SAVE_DEBOUNCE_MS_FOR_TEST = 1000;

/** Drain enough microtask turns for an in-flight `tick()` to reach its `await store.save(...)`. */
async function drainTurns(): Promise<void> {
  for (let i = 0; i < 8; i += 1) {
    // eslint-disable-next-line no-await-in-loop
    await Promise.resolve();
  }
}

describe("ShowEngine persistence", () => {
  /**
   * Final review, I5: every restore test in this file seeds the state file
   * with a JSON literal, so the READ half of persistence was well pinned
   * while the WRITE half was not — `buildPersistedState` could write
   * `manualBoxes: {}`, `lookId: null` or `overrides: {}` and the entire
   * suite stayed green. `ManualBoxAssignments` surviving in state is one of
   * the three obligations this branch discharges, and it was proven in one
   * direction only.
   *
   * So: drive real operator inputs through the engine, assert on the bytes
   * that actually reached disk, and then prove a SECOND engine restores
   * them — the round trip, not either half alone.
   */
  it("writes the manual boxes, look, overrides and roster it will later restore", async () => {
    const store = recordingFs();
    const host = new MockHost();
    const first = engine({ host, fs: store.fs });
    const annKey = resolvePersonKey({ participantId: "p1", rawName: "Ann" });

    first.onZoomEvent(joined("p1", "Ann"));
    first.onZoomEvent(joined("p2", "Bo"));
    first.setLook("teatime");
    first.assignBox(1, 2);
    first.setOverride({ personKey: annKey, displayName: "", location: "", role: "host" });
    await first.tick();

    const written = store.files.get("/state/show.json");
    expect(written).toBeDefined();
    const document = JSON.parse(written ?? "") as PersistedShowState;
    expect(document.version).toBe(3);
    expect(document.manualBoxes).toEqual({ 1: 2 });
    expect(document.lookId).toBe("teatime");
    expect(document.overrides[annKey]?.role).toBe("host");
    expect(document.slots.seats[0]?.panelist.participantId).toBe("p1");
    expect(document.gallery.cells).toBe(16);

    // The round trip: a fresh engine over the same files, with no setLook()
    // of its own, comes back to the same show.
    const second = engine({ fs: store.fs });
    expect(await second.restore()).toBe(true);
    const restored = await second.tick();
    expect(restored.manualBoxes).toEqual({ 1: 2 });
    expect(restored.look?.lookId).toBe("teatime");
    expect(restored.slots[0]?.panelist?.participantId).toBe("p1");
    expect(restored.slots[0]?.panelist?.role).toBe("host");
  });

  /**
   * Final review, I2. `pendingPersist` used to be cleared AFTER the await on
   * the save, so an operator input that arrived while the write was in
   * flight had its dirty mark wiped: acknowledged in memory, never written,
   * lost on restart. Manual boxes are the exposed case — `assignBox` sets
   * only that flag, while `setLook`/`setOverride` are accidentally rescued
   * by `otherInputsChanged` re-dirtying it at the seat step.
   *
   * The rig holds the first write open, issues the assignment inside that
   * window, then releases it. Without the fix the second write never
   * happens no matter how many ticks follow.
   */
  it("does not drop a change that lands while the previous save is in flight", async () => {
    const store = recordingFs();
    let nowMs = 0;
    const engineUnderTest = new ShowEngine({
      config: registryLess,
      host: new MockHost(),
      clock: { now: () => nowMs },
      store: new StateStore(registryLess.statePath, { fs: store.fs })
    });

    engineUnderTest.onZoomEvent(joined("p1", "Ann"));
    engineUnderTest.setLook("teatime");

    const release = store.holdNextWrite();
    const firstTick = engineUnderTest.tick();
    await drainTurns(); // the tick is now parked inside store.save()

    engineUnderTest.assignBox(1, 2); // lands DURING the write

    release();
    await firstTick;
    expect(store.writeCount()).toBe(1);
    const afterFirst = JSON.parse(store.files.get("/state/show.json") ?? "") as PersistedShowState;
    expect(afterFirst.manualBoxes).toEqual({}); // correctly not in the document that was already being written

    nowMs += SAVE_DEBOUNCE_MS_FOR_TEST;
    await engineUnderTest.tick();
    expect(store.writeCount()).toBe(2);
    const afterSecond = JSON.parse(store.files.get("/state/show.json") ?? "") as PersistedShowState;
    expect(afterSecond.manualBoxes).toEqual({ 1: 2 });
  });

  /**
   * The other half of the same ordering decision: a write that FAILS must
   * leave the change marked dirty, so the next tick past the debounce
   * retries it rather than the fix for I2 turning a failed write into the
   * same silent loss. The rejection itself propagates out of `tick()` by
   * design — documented on `tick()`, and the host's to handle.
   */
  it("retries a change whose save failed, and propagates the failure", async () => {
    const store = recordingFs();
    let nowMs = 0;
    let failNextWrite = true;
    const failing: StateFs = {
      ...store.fs,
      writeFile: async (p, c) => {
        if (failNextWrite) {
          failNextWrite = false;
          throw new Error("ENOSPC: no space left on device");
        }
        await store.fs.writeFile(p, c);
      }
    };
    const engineUnderTest = new ShowEngine({
      config: registryLess,
      host: new MockHost(),
      clock: { now: () => nowMs },
      store: new StateStore(registryLess.statePath, { fs: failing })
    });

    engineUnderTest.onZoomEvent(joined("p1", "Ann"));
    engineUnderTest.setLook("teatime");
    engineUnderTest.assignBox(1, 2);
    await expect(engineUnderTest.tick()).rejects.toThrow(/ENOSPC/);
    expect(store.files.has("/state/show.json")).toBe(false);

    nowMs += SAVE_DEBOUNCE_MS_FOR_TEST;
    await engineUnderTest.tick();
    const document = JSON.parse(store.files.get("/state/show.json") ?? "") as PersistedShowState;
    expect(document.manualBoxes).toEqual({ 1: 2 });
    expect(document.lookId).toBe("teatime");
  });
});

describe("ShowEngine host emission", () => {
  it("emits the full picture on the first tick", async () => {
    const host = new MockHost();
    const e = engine({ host });
    e.onZoomEvent(joined("p1", "Ann"));
    e.setLook("teatime");
    await e.tick();
    expect(host.callsOfKind("assignSlot").length).toBeGreaterThan(0);
    expect(host.callsOfKind("applyLook")).toHaveLength(1);
  });

  /**
   * The invariant this must break on: emitting unchanged values every tick.
   * A host adapter turns each command into real work — a source rebind, a
   * scene swap, an overlay re-raster — and re-emitting at tick rate is the
   * churn class that trips native fail-fasts on a long show.
   */
  it("emits nothing when a tick changes nothing", async () => {
    const host = new MockHost();
    const e = engine({ host });
    e.onZoomEvent(joined("p1", "Ann"));
    e.setLook("teatime");
    await e.tick();
    host.clear();
    await e.tick();
    await e.tick();
    expect(host.calls()).toEqual([]);
  });

  it("emits only the slot that actually changed", async () => {
    const host = new MockHost();
    const e = engine({ host });
    e.onZoomEvent(joined("p1", "Ann"));
    e.onZoomEvent(joined("p2", "Bo"));
    await e.tick();
    host.clear();
    e.onZoomEvent({ kind: "left", participantId: "p2" });
    await e.tick();
    expect(host.callsOfKind("assignSlot")).toEqual([
      { kind: "assignSlot", slot: 2, participantId: null }
    ]);
  });

  it("does not re-emit nameplates when the overlay is unchanged", async () => {
    const host = new MockHost();
    const e = engine({ host });
    e.onZoomEvent(joined("p1", "Ann"));
    e.setLook("teatime");
    await e.tick();
    host.clear();
    e.onZoomEvent({ kind: "audio", participantId: "p1", on: false });
    await e.tick();
    expect(host.callsOfKind("setNameplates")).toEqual([]);
  });

  /**
   * Fixture note: "teatime" (`includesHost: true`) only puts anyone in a
   * nameplate at all if a role is actually resolved for them — a lone
   * joined participant defaults to role "panelist" (no Mukana, no
   * override), which fills neither the host chair nor a manual guest box
   * (this look's `boxFill` resolves to "manual" with no hands-queue
   * capability, and no box was ever assigned). Without the host-role
   * override below, `resolution.nameplates` is `[]` before AND after the
   * rename, so the rename could never be observed to change anything — a
   * scenario, not an implementation bug. The override's `displayName`/
   * `location` are left blank on purpose (`OverrideDb`'s "" = "no
   * override for this field" convention, `panelistDb.ts`'s `pick`) so the
   * plate's name keeps coming from the identity parse of `rawName`, which
   * is the thing this test actually renames.
   */
  it("re-emits nameplates when a display name changes", async () => {
    const host = new MockHost();
    const e = engine({ host });
    e.onZoomEvent(joined("p1", "Ann"));
    e.setOverride({
      personKey: resolvePersonKey({ participantId: "p1", rawName: "Ann" }),
      displayName: "",
      location: "",
      role: "host"
    });
    e.setLook("teatime");
    await e.tick();
    host.clear();
    e.onZoomEvent({ kind: "renamed", participantId: "p1", rawName: "Annette | Oslo" });
    await e.tick();
    expect(host.callsOfKind("setNameplates")).toHaveLength(1);
  });
});

/**
 * Test rig for Task 9: an engine wired to a REAL `MukanaClient` over a
 * controllable `FetchLike`, a mutable injected clock, and a recorded list of
 * every URL fetched (recorded at fetch-START time, not settle time). All
 * three integrations are enabled, matching Task 9's brief.
 *
 * `hangHands: true` makes every `hands` fetch return a promise that never
 * settles on its own; each one queues its resolver, and `resolveHands()`
 * resolves the oldest still-pending one with the current `handsBody`.
 *
 * `flush()` (fix round 1, Finding 3/7): drains 8 microtask turns via a tight
 * `await Promise.resolve()` loop. `MukanaClient.request()`'s own two
 * internal `await`s (`await this.fetch(url)`, `await response.text()`) mean
 * a fetch's outcome does not become observable to a `tick()`-driven poller
 * within just 1-2 real `await e.tick()` calls — the exact number of ticks
 * needed is an accident of how `await`-continuation microtasks interleave
 * with the fetch's own continuation microtasks, NOT a stable property of
 * this engine's design. Before `flush()` existed, the polling tests were
 * accidentally coupled to that interleaving (and, with a busy gate added,
 * some of them broke outright — see `MukanaPoller.poll`'s doc comment).
 * `flush()` decouples every test below from `MukanaClient`'s internal shape:
 * call it after any `await e.tick()` whose test cares whether an in-flight
 * fetch has settled.
 *
 * Pure polling-MECHANISM tests (the due-check, the busy gate, the
 * non-blocking/rejection-safety guarantees) live in `mukanaPoller.test.ts`
 * now, exercising `MukanaPoller` directly (Task 1's carve-out) — what stays
 * here is the integration-level guarantee that `ShowEngine.tick()` wires
 * `MukanaPoller`'s outcomes into engine state exactly as before.
 */
/**
 * A two-guest-box look whose boxes fill from the hands queue (the `boxFill`
 * default), so losing the hands feed is observable as a fill-strategy flip
 * rather than as nothing at all. Declared as a plain object because
 * `parseShowEngineConfig` takes `unknown` and validates it itself.
 */
const QUEUE_LOOK = {
  id: "panel",
  label: "Panel",
  scenePreset: "scene-panel",
  boxes: 2,
  includesHost: false,
  includesReader: false
};

function mukanaEngine(options: { hangHands?: boolean; looks?: readonly unknown[] } = {}) {
  let nowMs = 0;
  const clock: Clock = { now: () => nowMs };
  const fetches: string[] = [];

  let handsHangs = options.hangHands === true;
  let handsBody = "NONE\nNONE\nNONE";
  let handsOk = true;
  let handsStatus = 200;
  let handsBodyBroken = false;
  let panelistsBody = "{}";
  let questionBody = "{}";
  const pendingHandsResolvers: Array<(response: FetchResponse) => void> = [];

  const fetch = async (url: string): Promise<FetchResponse> => {
    fetches.push(url);
    if (url.includes("req=hands")) {
      if (handsHangs) {
        return new Promise<FetchResponse>((resolve) => {
          pendingHandsResolvers.push(resolve);
        });
      }
      if (handsBodyBroken) {
        // A contract-violating FetchLike: `text()` must resolve to a
        // `string` per its own type, but a real fetch layer bug (or a
        // captive-portal / proxy oddity) could hand back something else.
        // `MukanaClient.request()` calls `parse(body)` OUTSIDE its own
        // try/catch, so a non-string body makes `parseHandsPayload`'s
        // `body.split(...)` throw, which makes `client.fetchHands()`'s
        // returned promise REJECT — this is deliberately how Finding 6's
        // rejection-safety test below produces a genuine rejection.
        return { ok: true, status: 200, text: async () => null as unknown as string };
      }
      return { ok: handsOk, status: handsStatus, text: async () => handsBody };
    }
    if (url.includes("req=panelists")) {
      return { ok: true, status: 200, text: async () => panelistsBody };
    }
    return { ok: true, status: 200, text: async () => questionBody };
  };

  const config = parseShowEngineConfig({
    capacity: 8,
    statePath: "/state/show.json",
    galleryCells: 16,
    integrations: { registry: true, handsQueue: true, questionFeed: true },
    mukana: { baseUrl: "https://example.com/rest.php", event: "officehours" },
    looks: options.looks ?? []
  });
  if (config.mukana === null) {
    throw new Error("unreachable: mukanaEngine always configures a mukana block");
  }

  const client = new MukanaClient(config.mukana, { fetch });
  const e = new ShowEngine({
    config,
    host: new MockHost(),
    clock,
    store: new StateStore(config.statePath, { fs: memoryFs() }),
    mukana: client
  });

  return {
    e,
    fetches,
    advance: (ms: number) => {
      nowMs += ms;
    },
    flush: async (): Promise<void> => {
      for (let i = 0; i < 8; i += 1) {
        // eslint-disable-next-line no-await-in-loop
        await Promise.resolve();
      }
    },
    setHandsBody: (body: string) => {
      handsBody = body;
    },
    setPanelistsBody: (body: string) => {
      panelistsBody = body;
    },
    setQuestionBody: (body: string) => {
      questionBody = body;
    },
    failHands: () => {
      handsOk = false;
      handsStatus = 503;
    },
    dormantHands: () => {
      handsBody = JSON.stringify({ status: 200, detail: "outside show hours" });
    },
    breakHandsBody: () => {
      handsBodyBroken = true;
    },
    fixHandsBody: () => {
      handsBodyBroken = false;
    },
    /** Every hands fetch from here on returns a promise that never settles on its own. */
    hangHands: () => {
      handsHangs = true;
    },
    resolveHands: () => {
      const resolver = pendingHandsResolvers.shift();
      if (resolver !== undefined) {
        resolver({ ok: true, status: 200, text: async () => handsBody });
      }
    }
  };
}

describe("ShowEngine Mukana polling", () => {
  /**
   * Final review, I6: `mukanaPoller.test.ts` proves a rejection handler
   * EXISTS on `MukanaPoller.poll` (it observes `unhandledRejection`
   * directly against the poller), but not what its body does — replacing
   * that body with `() => {}` left the whole suite green there. The body's
   * real job is clearing the busy gate, and losing that clear strands the
   * endpoint for the lifetime of the process: the one-in-flight gate never
   * re-opens, no later fetch ever starts, and the feed is dead for the rest
   * of the show with the client's health frozen at whatever it last said.
   * That is the known deadlock this branch's ledger carries as its top open
   * item, and it had no test signal at all.
   *
   * So: reject a fetch, then prove the endpoint is genuinely POLLABLE again
   * — a new fetch starts on the next due tick, and real data from it lands
   * in engine state.
   *
   * Honest scope note: the handler also records an `invalid` outcome, and
   * that half is deliberately NOT asserted here, because it is not
   * observable through the public surface — `applyHandsOutcome` ignores
   * every non-`data` outcome by design, so recording `invalid` and
   * recording nothing are indistinguishable from outside. What a wrong
   * outcome WOULD do (a fabricated `data` outcome overwriting the queue) is
   * covered by the last-good-queue test below.
   */
  it("leaves an endpoint pollable after its fetch rejects", async () => {
    const { e, fetches, advance, flush, breakHandsBody, fixHandsBody, setHandsBody } =
      mukanaEngine();
    const handsFetches = (): number => fetches.filter((url) => url.includes("req=hands")).length;

    breakHandsBody(); // the next hands fetch REJECTS rather than resolving
    advance(10_000);
    await e.tick();
    await flush();
    expect(handsFetches()).toBe(1);

    fixHandsBody();
    setHandsBody("4242,5555\n1383\nNONE");
    advance(10_000);
    await e.tick();
    await flush();
    // The busy flag was cleared by the rejection handler: this endpoint is
    // being polled again rather than stranded.
    expect(handsFetches()).toBe(2);

    await e.tick();
    await flush();
    expect(e.snapshot().queue).toEqual({
      previous: [],
      current: "1383",
      upcoming: ["4242", "5555"]
    });
  });

  it("keeps the last good queue when a poll comes back invalid", async () => {
    const { e, advance, flush, setHandsBody } = mukanaEngine();
    setHandsBody("4242\n5555\nNONE");
    advance(10_000);
    await e.tick();
    await flush();
    await e.tick();
    await flush();
    const good = e.snapshot().queue;
    setHandsBody("<html>gateway timeout</html>");
    advance(10_000);
    await e.tick();
    await flush();
    await e.tick();
    await flush();
    expect(e.snapshot().queue).toEqual(good);
  });

  /**
   * Fix round 1, Finding 1/2: the retention test above only proves "the
   * value didn't change" — it never proves a poll's data reaches engine
   * state AT ALL, because (before `flush()`) neither fetch in that test
   * ever settled within its tick budget. This test observes REAL data
   * landing, so a mutation that deletes `applyHandsOutcome`'s data branch
   * has something to red against.
   */
  it("populates the hands queue from a poll", async () => {
    const { e, advance, flush, setHandsBody } = mukanaEngine();
    setHandsBody("4242,5555\n1383\nNONE");
    advance(10_000);
    await e.tick();
    await flush();
    await e.tick();
    await flush();
    expect(e.snapshot().queue).toEqual({
      previous: [],
      current: "1383",
      upcoming: ["4242", "5555"]
    });
  });

  /** Fix round 1, Finding 1/2: same shape as the hands test, for the question feed. */
  it("populates the audience question from a poll", async () => {
    const { e, advance, flush, setQuestionBody } = mukanaEngine();
    setQuestionBody(
      JSON.stringify({ q: { key: "k1", n: "Ann Lee", q: "Why?", tag: "topic", v: 3, ts: 12 } })
    );
    e.setQuestionVisible(true);
    advance(10_000);
    await e.tick();
    await flush();
    await e.tick();
    await flush();
    expect(e.snapshot().overlays.question).toEqual({
      askerName: "Ann Lee",
      text: "Why?",
      tag: "topic",
      votes: 3
    });
  });

  /**
   * Fix round 1, Finding 1/2: the registry twin — a Mukana panelists poll
   * must actually reach a seated `Panelist`'s identity, not just merge into
   * a registry nothing reads. "Ann Lee | 4242" is the OHG identity
   * convention (`identity.ts`): the PIN `4242` is what `buildPanelistDb`
   * looks up against the merged registry.
   */
  it("merges a panelists poll into a seated panelist's identity", async () => {
    const { e, advance, flush, setPanelistsBody } = mukanaEngine();
    setPanelistsBody(
      JSON.stringify({
        p1: { displayName: "Ann From Mukana", loc: "Austin, TX", pin: 4242, role: "panelist", online: true }
      })
    );
    e.onZoomEvent(joined("p1", "Ann Lee | 4242"));
    advance(10_000);
    await e.tick();
    await flush();
    await e.tick();
    await flush();
    const panelist = e.snapshot().panelists.find((candidate) => candidate.participantId === "p1");
    expect(panelist?.displayName).toBe("Ann From Mukana");
    expect(panelist?.location).toBe("Austin, TX");
    expect(panelist?.hasMukana).toBe(true);
  });

  it("moves the hands capability to unavailable when the feed fails", async () => {
    const { e, advance, flush, failHands } = mukanaEngine();
    advance(10_000);
    await e.tick();
    await flush();
    await e.tick();
    await flush();
    expect(e.snapshot().capabilities.handsQueue.state).toBe("available");
    failHands();
    advance(10_000);
    await e.tick();
    await flush();
    await e.tick();
    await flush();
    const cap = e.snapshot().capabilities.handsQueue;
    expect(cap.state).toBe("unavailable");
    expect(cap.detail).not.toBeNull();
  });

  /**
   * Final review, I1 — the probe that found it, run as a test. A `FetchLike`
   * whose hands promise NEVER settles (the contract violation `FetchLike`'s
   * doc now names: no `AbortSignal.timeout`) used to leave the hands
   * capability `available` forever, because health is only ever written when
   * a request settles and the one-in-flight gate meant no later fetch could
   * contradict it. The consequence measured over 30 simulated minutes was
   * not stale data: `effectiveBoxFill` stayed `"queue"`, the queue was empty
   * because nothing ever arrived, and BOTH guest boxes resolved to `null`
   * for the whole show while the operator's manual assignments sat ignored —
   * the degradation path this package exists to guarantee never engaged at
   * all, and the operator's fallback was unreachable on air.
   *
   * The assertion that matters is the last one: the boxes are FILLED, from
   * `manualBoxes`. Everything above it is the state that has to hold for
   * that fallback to be reachable.
   */
  it("falls back to the operator's manual boxes when an endpoint never answers at all", async () => {
    const { e, advance, flush } = mukanaEngine({ hangHands: true, looks: [QUEUE_LOOK] });
    e.onZoomEvent(joined("p1", "Ann | 1001"));
    e.onZoomEvent(joined("p2", "Bo | 1002"));
    e.setLook("panel");
    e.assignBox(1, 1);
    e.assignBox(2, 2);

    // The very first tick starts the hands fetch that will never settle. It
    // is also the tick that pins the PESSIMISTIC start specifically: no time
    // has passed, so the engine's own hung-poll degradation cannot have
    // fired yet, and the only thing that can be reporting this endpoint as
    // unusable is that it has never answered.
    const first = await e.tick();
    await flush();
    expect(first.capabilities.handsQueue.state).toBe("unavailable");
    expect(first.look?.boxFill).toBe("manual");
    expect(first.look?.boxes).toEqual([
      { box: 1, slot: 1 },
      { box: 2, slot: 2 }
    ]);

    // Thirty simulated minutes of a show whose hands feed never answers.
    let snap = await e.tick();
    for (let minute = 0; minute < 30; minute += 1) {
      advance(60_000);
      // eslint-disable-next-line no-await-in-loop
      snap = await e.tick();
      // eslint-disable-next-line no-await-in-loop
      await flush();
    }

    expect(snap.health.hands.state).toBe("failing");
    expect(snap.capabilities.handsQueue.state).toBe("unavailable");
    expect(snap.capabilities.handsQueue.detail).not.toBeNull();
    expect(snap.look?.boxFill).toBe("manual");
    expect(snap.look?.boxes).toEqual([
      { box: 1, slot: 1 },
      { box: 2, slot: 2 }
    ]);
  });

  /**
   * Final review, I1, the mid-show half: an endpoint that ANSWERED and then
   * hung. A pessimistic `initialHealth` alone cannot catch this one — the
   * client has a genuine `ok` on record and, with the fetch still in flight,
   * will never be handed anything that overwrites it. The engine holds no
   * timer and cannot cancel the fetch, so what it does instead is stop
   * asserting usability it has no current evidence for, off the injected
   * clock and the endpoint's own `nextDelayMs`.
   *
   * The middle assertion is the one that keeps this honest: one interval of
   * ordinary slowness must NOT be reported as an outage.
   */
  it("stops reporting an endpoint as available once its poll has hung for several intervals", async () => {
    const { e, advance, flush, setHandsBody, hangHands } = mukanaEngine();
    setHandsBody("4242,5555\n1383\nNONE");
    advance(10_000);
    await e.tick();
    await flush();
    const healthy = await e.tick();
    await flush();
    expect(healthy.capabilities.handsQueue.state).toBe("available");

    hangHands();
    advance(10_000);
    const hungButFresh = await e.tick(); // starts the fetch that will never settle
    await flush();
    expect(hungButFresh.capabilities.handsQueue.state).toBe("available");

    // The hands interval is 2000ms; three of them is the grace window.
    advance(6_001);
    const degraded = await e.tick();
    await flush();
    expect(degraded.capabilities.handsQueue.state).toBe("unavailable");
    expect(degraded.health.hands.state).toBe("failing");
    expect(degraded.health.hands.detail).toMatch(/no response/);
    // Degrading is not discarding: the last good queue is still there for
    // the moment the feed comes back.
    expect(degraded.queue).toEqual(healthy.queue);
    expect(degraded.queue.current).toBe("1383");
  });

  it("reports dormant as unavailable with the operator-facing detail", async () => {
    const { e, advance, flush, dormantHands } = mukanaEngine();
    dormantHands();
    advance(10_000);
    await e.tick();
    await flush();
    await e.tick();
    await flush();
    expect(e.snapshot().capabilities.handsQueue.state).toBe("unavailable");
  });
});
