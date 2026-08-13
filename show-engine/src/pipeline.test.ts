import { describe, expect, it } from "vitest";
import {
  buildPanelistDb,
  LiveSlots,
  MukanaClient,
  MukanaRegistry,
  OverrideDb,
  parseShowEngineConfig,
  StateStore,
  ZoomIngest,
  type FetchLike,
  type Participant,
  type ShowState,
  type StateFs
} from "./index.js";

const config = parseShowEngineConfig({
  capacity: 4,
  statePath: "/show/state.json",
  mukana: { baseUrl: "https://hoka.example.com/rest.php", event: "officehours" }
});

/**
 * `config.mukana` is nullable because a show may have no Mukana at all.
 * These tests are all about the Mukana-backed pipeline, so the block is
 * present by construction — this narrows it once, loudly, rather than
 * asserting it away at each use.
 */
const mukanaConfig = config.mukana;
if (mukanaConfig === null) throw new Error("fixture: pipeline tests require a mukana config");

const mukanaBody = JSON.stringify({
  uidHost: {
    displayName: "J.J. Mc Kenna",
    loc: "Santa Venetia, CA",
    pin: 1383,
    role: "host",
    online: true
  },
  uidPanelist: {
    displayName: "Ann Lee",
    loc: "Austin, TX",
    pin: 4242,
    role: "panelist",
    online: true
  }
});

const fetchMukana: FetchLike = async () => ({
  ok: true,
  status: 200,
  text: async () => mukanaBody
});

function participant(participantId: string, rawName: string): Participant {
  return {
    participantId,
    rawName,
    online: true,
    videoOn: true,
    audioOn: false,
    handRaised: false,
    zoomRole: 3
  };
}

function memoryFs(): StateFs {
  const files = new Map<string, string>();
  return {
    async readFile(path) {
      const content = files.get(path);
      if (content === undefined) throw new Error(`ENOENT: ${path}`);
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
      /* no-op */
    }
  };
}

describe("identity and roster pipeline", () => {
  it("seats registered panelists with their Mukana identity and role", async () => {
    const ingest = new ZoomIngest();
    ingest.apply({ kind: "joined", participant: participant("z1", "JJ | 1383 | somewhere") });
    ingest.apply({ kind: "joined", participant: participant("z2", "Ann | 4242 | Austin") });
    ingest.apply({ kind: "joined", participant: participant("z3", "Walk-in Guest") });
    ingest.commit();

    const registry = new MukanaRegistry();
    const client = new MukanaClient(mukanaConfig, { fetch: fetchMukana });
    const outcome = await client.fetchPanelists();
    if (outcome.kind === "data") registry.merge(outcome.db);

    const overrides = new OverrideDb();
    const db = buildPanelistDb(ingest.snapshot(), registry.current(), overrides.entries());

    const slots = new LiveSlots({
      capacity: config.capacity,
      utilityPinBase: config.utilityPinBase
    });
    for (const panelist of db.values()) slots.add(panelist);

    const seated = slots.slots().map((entry) =>
      entry.panelist === null
        ? null
        : { name: entry.panelist.displayName, role: entry.panelist.role }
    );
    expect(seated).toEqual([
      { name: "J.J. Mc Kenna", role: "host" },
      { name: "Ann Lee", role: "panelist" },
      { name: "Walk-in Guest", role: "panelist" },
      null
    ]);
  });

  it("moves the host chair via an override without moving anyone's slot", async () => {
    const ingest = new ZoomIngest();
    ingest.apply({ kind: "joined", participant: participant("z1", "JJ | 1383") });
    ingest.apply({ kind: "joined", participant: participant("z2", "Ann | 4242") });
    ingest.commit();

    const registry = new MukanaRegistry();
    const client = new MukanaClient(mukanaConfig, { fetch: fetchMukana });
    const outcome = await client.fetchPanelists();
    if (outcome.kind === "data") registry.merge(outcome.db);

    const overrides = new OverrideDb();
    const slots = new LiveSlots({
      capacity: config.capacity,
      utilityPinBase: config.utilityPinBase
    });
    for (const panelist of buildPanelistDb(
      ingest.snapshot(),
      registry.current(),
      overrides.entries()
    ).values()) {
      slots.add(panelist);
    }
    expect(slots.slotOf("z1")).toBe(1);

    overrides.assignExclusiveRole("pin:4242", "host", registry.current());
    slots.refresh(buildPanelistDb(ingest.snapshot(), registry.current(), overrides.entries()));

    expect(slots.slotOf("z1")).toBe(1);
    expect(slots.slotOf("z2")).toBe(2);
    expect(slots.slots()[0]?.panelist?.role).toBe("panelist");
    expect(slots.slots()[1]?.panelist?.role).toBe("host");
  });

  it("keeps a reconnecting panelist's slot across a persistence round trip", async () => {
    const ingest = new ZoomIngest();
    ingest.apply({ kind: "joined", participant: participant("z1", "JJ | 1383") });
    ingest.apply({ kind: "joined", participant: participant("z2", "Ann | 4242") });
    ingest.commit();

    const registry = new MukanaRegistry();
    const client = new MukanaClient(mukanaConfig, { fetch: fetchMukana });
    const outcome = await client.fetchPanelists();
    if (outcome.kind === "data") registry.merge(outcome.db);

    const overrides = new OverrideDb();
    const slots = new LiveSlots({
      capacity: config.capacity,
      utilityPinBase: config.utilityPinBase
    });
    slots.rebuild([
      ...buildPanelistDb(ingest.snapshot(), registry.current(), overrides.entries()).values()
    ]);

    const store = new StateStore(config.statePath, { fs: memoryFs() });
    const saved: ShowState = {
      version: 2,
      slots: slots.toJSON(),
      overrides: overrides.entries(),
      gallery: { version: 1, cells: 1, assignments: [{ cell: 1, slot: 0 }] }
    };
    await store.save(saved);

    const loaded = await store.load();
    expect(loaded).not.toBeNull();
    if (loaded === null) return;

    const restored = LiveSlots.fromJSON(loaded.slots, {
      capacity: config.capacity,
      utilityPinBase: config.utilityPinBase
    });
    expect(restored.slotOf("z2")).toBe(2);

    ingest.apply({ kind: "left", participantId: "z2" });
    ingest.commit();
    restored.refresh(
      buildPanelistDb(ingest.snapshot(), registry.current(), overrides.entries())
    );
    expect(restored.slotOf("z2")).toBe(2);
    expect(restored.slots()[1]?.panelist?.online).toBe(false);
  });

  it("survives a dormant window and resumes without losing seats or corrupting backoff", async () => {
    const ingest = new ZoomIngest();
    ingest.apply({ kind: "joined", participant: participant("z1", "JJ | 1383") });
    ingest.apply({ kind: "joined", participant: participant("z2", "Ann | 4242") });
    ingest.commit();

    const registry = new MukanaRegistry();
    const overrides = new OverrideDb();
    const slots = new LiveSlots({
      capacity: config.capacity,
      utilityPinBase: config.utilityPinBase
    });

    let body = mukanaBody;
    const client = new MukanaClient(mukanaConfig, {
      fetch: async () => ({ ok: true, status: 200, text: async () => body })
    });

    function seatedIdentities(): ({ name: string; role: string } | null)[] {
      return slots
        .slots()
        .map((entry) =>
          entry.panelist === null
            ? null
            : { name: entry.panelist.displayName, role: entry.panelist.role }
        );
    }

    // 1. Full registry data. Merge it, build the panelist DB, seat the roster.
    const first = await client.fetchPanelists();
    if (first.kind === "data") registry.merge(first.db);
    slots.rebuild([
      ...buildPanelistDb(ingest.snapshot(), registry.current(), overrides.entries()).values()
    ]);

    expect(seatedIdentities()).toEqual([
      { name: "J.J. Mc Kenna", role: "host" },
      { name: "Ann Lee", role: "panelist" },
      null,
      null
    ]);
    expect(slots.slotOf("z1")).toBe(1);
    expect(slots.slotOf("z2")).toBe(2);

    // 2. The registry goes dormant. Downstream composition must keep serving off the
    // retained registry rather than losing seats — the invariant a dormant window relies on.
    body = JSON.stringify({
      status: 200,
      detail: "This page is only available between 1300 and 2000 UTC"
    });
    const second = await client.fetchPanelists();
    expect(second.kind).toBe("dormant");
    expect(client.healthFor("panelists").state).toBe("dormant");

    slots.refresh(buildPanelistDb(ingest.snapshot(), registry.current(), overrides.entries()));

    expect(seatedIdentities()).toEqual([
      { name: "J.J. Mc Kenna", role: "host" },
      { name: "Ann Lee", role: "panelist" },
      null,
      null
    ]);
    expect(slots.slotOf("z1")).toBe(1);
    expect(slots.slotOf("z2")).toBe(2);

    // 3. Data resumes: one existing panelist's name changes and a new PIN registers.
    body = JSON.stringify({
      uidHost: {
        displayName: "J.J. Mc Kenna",
        loc: "Santa Venetia, CA",
        pin: 1383,
        role: "host",
        online: true
      },
      uidPanelist: {
        displayName: "Ann Lee-Martinez",
        loc: "Austin, TX",
        pin: 4242,
        role: "panelist",
        online: true
      },
      uidNew: {
        displayName: "New Panelist",
        loc: "Denver, CO",
        pin: 5555,
        role: "panelist",
        online: true
      }
    });
    const third = await client.fetchPanelists();
    if (third.kind === "data") registry.merge(third.db);
    expect(Object.keys(registry.current()).sort()).toEqual(["1383", "4242", "5555"]);

    slots.refresh(buildPanelistDb(ingest.snapshot(), registry.current(), overrides.entries()));

    expect(slots.slotOf("z2")).toBe(2);
    expect(slots.slots()[1]?.panelist?.displayName).toBe("Ann Lee-Martinez");
    expect(client.healthFor("panelists").state).toBe("ok");
    expect(client.nextDelayMs("panelists")).toBe(mukanaConfig.panelistsIntervalMs);
  });

  it("seats a utility bot in the tail while people fill from the front", async () => {
    const ingest = new ZoomIngest();
    ingest.apply({ kind: "joined", participant: participant("z1", "JJ | 1383") });
    ingest.apply({ kind: "joined", participant: participant("bot", "Playback | 9000") });
    ingest.commit();

    const registry = new MukanaRegistry();
    const client = new MukanaClient(mukanaConfig, { fetch: fetchMukana });
    const outcome = await client.fetchPanelists();
    if (outcome.kind === "data") registry.merge(outcome.db);

    const slots = new LiveSlots({
      capacity: config.capacity,
      utilityPinBase: config.utilityPinBase
    });
    slots.rebuild([
      ...buildPanelistDb(ingest.snapshot(), registry.current(), new OverrideDb().entries()).values()
    ]);

    expect(slots.slotOf("z1")).toBe(1);
    expect(slots.slotOf("bot")).toBe(4);
  });
});
