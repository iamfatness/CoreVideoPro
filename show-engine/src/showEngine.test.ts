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
import type { MukanaClient, MukanaEndpoint, MukanaHealth } from "./mukanaClient.js";

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

  it("derives tally from the program source and the resolved look", async () => {
    const e = engine();
    e.onZoomEvent(joined("p1", "Ann"));
    e.setLook("teatime");
    await e.tick();
    e.directCut({ kind: "look", lookId: "teatime" });
    const snap = await e.tick();
    expect(snap.tally.mode).toBe("look");
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
  return { health } as unknown as MukanaClient;
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
        });
      }
    }
  }
});
