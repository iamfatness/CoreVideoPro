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
    const client = new MukanaClient(config.mukana, { fetch: fetchMukana });
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
    const client = new MukanaClient(config.mukana, { fetch: fetchMukana });
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

    overrides.assignExclusiveRole("4242", "host", registry.current());
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
    const client = new MukanaClient(config.mukana, { fetch: fetchMukana });
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
      version: 1,
      slots: slots.toJSON(),
      overrides: overrides.entries()
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

  it("keeps the last-good registry when Mukana goes dormant", async () => {
    const registry = new MukanaRegistry();
    let body = mukanaBody;
    const client = new MukanaClient(config.mukana, {
      fetch: async () => ({ ok: true, status: 200, text: async () => body })
    });

    const first = await client.fetchPanelists();
    if (first.kind === "data") registry.merge(first.db);
    expect(Object.keys(registry.current()).sort()).toEqual(["1383", "4242"]);

    body = JSON.stringify({ status: 200, detail: "This page is only available between 1300 and 2000 UTC" });
    const second = await client.fetchPanelists();
    expect(second.kind).toBe("dormant");
    if (second.kind === "data") registry.merge(second.db);

    expect(Object.keys(registry.current()).sort()).toEqual(["1383", "4242"]);
    expect(client.health.state).toBe("dormant");
  });

  it("seats a utility bot in the tail while people fill from the front", async () => {
    const ingest = new ZoomIngest();
    ingest.apply({ kind: "joined", participant: participant("z1", "JJ | 1383") });
    ingest.apply({ kind: "joined", participant: participant("bot", "Playback | 9000") });
    ingest.commit();

    const registry = new MukanaRegistry();
    const client = new MukanaClient(config.mukana, { fetch: fetchMukana });
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
