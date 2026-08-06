import { describe, expect, it } from "vitest";
import {
  buildPanelistDb,
  deriveTally,
  LiveSlots,
  OverlayDirector,
  OverrideDb,
  parseHandsPayload,
  resolveLook,
  resolvePersonKey,
  ZoomIngest,
  type Capability,
  type LookDefinition,
  type ManualBoxAssignments,
  type MukanaDb,
  type Participant
} from "./index.js";

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

const registry: MukanaDb = {
  "1383": { pin: "1383", displayName: "J.J. Mc Kenna", location: "CA", role: "host", online: true },
  "4242": { pin: "4242", displayName: "Ann Lee", location: "TX", role: "panelist", online: true },
  "5555": { pin: "5555", displayName: "Bo Diaz", location: "PE", role: "panelist", online: true }
};

const teatime: LookDefinition = {
  id: "teatime",
  label: "Teatime",
  scenePreset: "scene-teatime",
  boxes: 2,
  includesHost: true,
  includesReader: false,
  plateTone: "accent",
  tallySource: "boxes",
  boxFill: "queue"
};

const unavailable: Capability = { state: "unavailable", detail: "HTTP 503" };
const disabled: Capability = { state: "disabled", detail: null };
const available: Capability = { state: "available", detail: null };

function roster(names: readonly (readonly [string, string])[]): LiveSlots {
  const ingest = new ZoomIngest();
  for (const [id, name] of names) {
    ingest.apply({ kind: "joined", participant: participant(id, name) });
  }
  ingest.commit();
  const slots = new LiveSlots({ capacity: 8, utilityPinBase: 9000 });
  slots.rebuild([
    ...buildPanelistDb(ingest.snapshot(), registry, new OverrideDb().entries()).values()
  ]);
  return slots;
}

const manualBoxes: ManualBoxAssignments = { 1: 1, 2: 2 };

describe("degradation equivalence", () => {
  const slots = roster([
    ["z-ann", "Ann | 4242"],
    ["z-bo", "Bo | 5555"],
    ["z-host", "JJ | 1383"]
  ]);
  const parsed = parseHandsPayload("5555\n4242\nNONE");

  it("resolves a look identically whether hands are unavailable or disabled", () => {
    if (parsed.kind !== "data") throw new Error("fixture");
    const context = { queue: parsed.queue, slots: slots.slots(), page: 0, manualBoxes };
    expect(resolveLook(teatime, { ...context, handsQueue: unavailable })).toEqual(
      resolveLook(teatime, { ...context, handsQueue: disabled })
    );
  });

  /**
   * The two tests below cannot fail while the one above passes, and are not
   * independent coverage. `deriveTally` and `OverlayDirector.update` receive
   * only the resolved `LookResolution` — never a `Capability` — and both are
   * pure, so once the two resolutions are `toEqual` their outputs are too.
   * They are kept as executable documentation that the equivalence is meant
   * to reach all the way to what the operator sees.
   *
   * The guarantee itself is structural, not tested: a `Capability` is read
   * at exactly one site in the package (`canUse` inside `effectiveBoxFill`),
   * so nothing downstream of `LookResolution` is even able to branch on the
   * difference between `unavailable` and `disabled`. Widening that read past
   * one site is what would put these two at risk — a new consumer taking a
   * `Capability` directly needs its own equivalence test, because this one
   * would not notice.
   */
  it("derives tally identically whether hands are unavailable or disabled", () => {
    if (parsed.kind !== "data") throw new Error("fixture");
    const context = { queue: parsed.queue, slots: slots.slots(), page: 0, manualBoxes };
    const tallyFor = (handsQueue: Capability) =>
      deriveTally({
        source: { kind: "look", lookId: "teatime" },
        slots: slots.slots(),
        gallery: [],
        look: resolveLook(teatime, { ...context, handsQueue }),
        activeSpeakerSlot: null
      });
    expect(tallyFor(unavailable)).toEqual(tallyFor(disabled));
  });

  it("produces identical overlays whether hands are unavailable or disabled", () => {
    if (parsed.kind !== "data") throw new Error("fixture");
    const context = { queue: parsed.queue, slots: slots.slots(), page: 0, manualBoxes };
    const stateFor = (handsQueue: Capability) => {
      const director = new OverlayDirector();
      director.update({
        look: resolveLook(teatime, { ...context, handsQueue }),
        question: null,
        questionVisible: false
      });
      return director.state();
    };
    expect(stateFor(unavailable)).toEqual(stateFor(disabled));
  });
});

describe("surviving a hands outage mid-show", () => {
  const slots = roster([
    ["z-ann", "Ann | 4242"],
    ["z-bo", "Bo | 5555"],
    ["z-host", "JJ | 1383"]
  ]);

  it("keeps guests on screen when the hands feed dies", () => {
    const parsed = parseHandsPayload("5555\n4242\nNONE");
    if (parsed.kind !== "data") throw new Error("fixture");
    const context = { queue: parsed.queue, slots: slots.slots(), page: 0 };

    const before = resolveLook(teatime, { ...context, handsQueue: available });
    expect(before.boxFill).toBe("queue");
    expect(before.boxes.every((box) => box.slot !== null)).toBe(true);

    const after = resolveLook(teatime, {
      ...context,
      handsQueue: unavailable,
      manualBoxes
    });
    expect(after.boxFill).toBe("manual");
    expect(after.boxes.every((box) => box.slot !== null)).toBe(true);
  });

  it("keeps the host chair and its nameplate through the outage", () => {
    const parsed = parseHandsPayload("5555\n4242\nNONE");
    if (parsed.kind !== "data") throw new Error("fixture");
    const after = resolveLook(teatime, {
      queue: parsed.queue,
      slots: slots.slots(),
      page: 0,
      handsQueue: unavailable,
      manualBoxes
    });
    expect(after.hostSlot).toBe(slots.slotOf("z-host"));
    expect(after.nameplates.some((plate) => plate.position.kind === "host")).toBe(true);
  });
});

describe("a show with no registry at all", () => {
  const slots = roster([
    ["z-ann", "Ann Lee"],
    ["z-bo", "Bo Diaz"]
  ]);

  it("still seats everyone with names parsed from Zoom", () => {
    expect(slots.occupiedCount()).toBe(2);
    expect(slots.slots()[0]?.panelist?.displayName).toBe("Ann Lee");
  });

  it("lets the operator assign a host with no PIN present", () => {
    const key = resolvePersonKey({ participantId: "z-ann", rawName: "Ann Lee" });
    const overrides = new OverrideDb();
    overrides.assignExclusiveRole(key, "host", {});

    const ingest = new ZoomIngest();
    ingest.apply({ kind: "joined", participant: participant("z-ann", "Ann Lee") });
    ingest.apply({ kind: "joined", participant: participant("z-bo", "Bo Diaz") });
    ingest.commit();

    const db = buildPanelistDb(ingest.snapshot(), {}, overrides.entries());
    expect(db.get("z-ann")?.role).toBe("host");
    expect(db.get("z-bo")?.role).toBe("panelist");
  });

  it("resolves the host chair from that assignment", () => {
    const key = resolvePersonKey({ participantId: "z-ann", rawName: "Ann Lee" });
    const overrides = new OverrideDb();
    overrides.assignExclusiveRole(key, "host", {});

    const ingest = new ZoomIngest();
    ingest.apply({ kind: "joined", participant: participant("z-ann", "Ann Lee") });
    ingest.apply({ kind: "joined", participant: participant("z-bo", "Bo Diaz") });
    ingest.commit();

    const seated = new LiveSlots({ capacity: 8, utilityPinBase: 9000 });
    seated.rebuild([...buildPanelistDb(ingest.snapshot(), {}, overrides.entries()).values()]);

    const resolution = resolveLook(teatime, {
      queue: { previous: [], current: null, upcoming: [] },
      slots: seated.slots(),
      page: 0,
      manualBoxes: { 1: seated.slotOf("z-bo") ?? 0 }
    });
    expect(resolution.hostSlot).toBe(seated.slotOf("z-ann"));
    expect(resolution.boxFill).toBe("manual");
  });
});
