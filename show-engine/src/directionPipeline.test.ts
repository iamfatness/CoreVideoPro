import { describe, expect, it } from "vitest";
import {
  buildPanelistDb,
  FiloAssigner,
  GalleryDirector,
  LiveSlots,
  OverrideDb,
  ProgramBus,
  RecencyScores,
  parseHandsPayload,
  resolveLook,
  stripChairs,
  findChairSlots,
  ZoomIngest,
  type LookDefinition,
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

const mukana: MukanaDb = {
  "1383": {
    pin: "1383",
    displayName: "J.J. Mc Kenna",
    location: "CA",
    role: "host",
    online: true
  },
  "2001": { pin: "2001", displayName: "Reader Rose", location: "NY", role: "reader", online: true },
  "4242": { pin: "4242", displayName: "Ann Lee", location: "TX", role: "panelist", online: true },
  "5555": { pin: "5555", displayName: "Bo Diaz", location: "PE", role: "panelist", online: true },
  "7777": {
    pin: "7777",
    displayName: "Sam Signer",
    location: "WA",
    role: "aslinterpreter",
    online: true
  }
};

function seatedRoster(): LiveSlots {
  const ingest = new ZoomIngest();
  for (const [id, pin] of [
    ["z-host", "1383"],
    ["z-reader", "2001"],
    ["z-ann", "4242"],
    ["z-bo", "5555"],
    ["z-asl", "7777"]
  ] as const) {
    ingest.apply({ kind: "joined", participant: participant(id, `Name | ${pin}`) });
  }
  ingest.commit();

  // ZoomIngest publishes sorted by participantId, and buildPanelistDb preserves that
  // order, so rebuild seats: z-ann, z-asl, z-bo, z-host, z-reader. Assertions below
  // derive slot numbers via slotOf rather than hardcoding that order.
  const slots = new LiveSlots({ capacity: 8, utilityPinBase: 9000 });
  slots.rebuild([...buildPanelistDb(ingest.snapshot(), mukana, new OverrideDb().entries()).values()]);
  return slots;
}

const teatime: LookDefinition = {
  id: "teatime",
  label: "Teatime",
  scenePreset: "scene-teatime",
  boxes: 2,
  includesHost: true,
  includesReader: true,
  plateTone: "accent"
};

describe("direction pipeline", () => {
  it("resolves a look against a roster built from the registry", () => {
    const slots = seatedRoster();
    const parsed = parseHandsPayload("5555\n4242\nNONE");
    expect(parsed.kind).toBe("data");
    if (parsed.kind !== "data") return;

    const chairs = findChairSlots(slots.slots());
    const queue = stripChairs(parsed.queue, {
      hostPin: "1383",
      readerPin: "2001"
    });

    const resolution = resolveLook(teatime, { queue, slots: slots.slots(), page: 0 });
    expect(resolution.hostSlot).toBe(slots.slotOf("z-host"));
    expect(resolution.readerSlot).toBe(slots.slotOf("z-reader"));
    expect(chairs).toEqual({
      hostSlot: slots.slotOf("z-host"),
      readerSlot: slots.slotOf("z-reader")
    });
    expect(resolution.boxes).toEqual([
      { box: 1, slot: slots.slotOf("z-ann") },
      { box: 2, slot: slots.slotOf("z-bo") }
    ]);
    expect(resolution.nameplates.map((plate) => plate.name)).toEqual([
      "J.J. Mc Kenna",
      "Reader Rose",
      "Ann Lee",
      "Bo Diaz"
    ]);
  });

  it("keeps a chair out of the guest boxes even when they raise a hand", () => {
    const slots = seatedRoster();
    const parsed = parseHandsPayload("1383,4242\n2001\nNONE");
    expect(parsed.kind).toBe("data");
    if (parsed.kind !== "data") return;

    const queue = stripChairs(parsed.queue, { hostPin: "1383", readerPin: "2001" });
    const resolution = resolveLook(teatime, { queue, slots: slots.slots(), page: 0 });

    const boxedSlots = resolution.boxes.map((box) => box.slot);
    expect(boxedSlots).not.toContain(resolution.hostSlot);
    expect(boxedSlots).not.toContain(resolution.readerSlot);
    expect(boxedSlots).toEqual([slots.slotOf("z-ann"), null]);
  });

  it("never lets an ASL interpreter take program, even as the newest speaker", () => {
    const slots = seatedRoster();
    const bus = new ProgramBus();
    bus.setActiveSpeakerFollow(true);

    const aslSlot = slots.slotOf("z-asl");
    expect(aslSlot).not.toBeNull();
    const asl = slots.slots()[(aslSlot ?? 1) - 1]?.panelist;
    expect(asl?.role).toBe("aslinterpreter");

    expect(bus.onActiveSpeaker("z-ann", "panelist")).toBe(true);
    expect(bus.state().activeSpeakerId).toBe("z-ann");

    expect(bus.onActiveSpeaker("z-asl", asl?.role ?? "panelist")).toBe(false);
    expect(bus.state().activeSpeakerId).toBe("z-ann");
    expect(bus.state().program).toEqual({ kind: "activeSpeaker" });
  });

  it("reorders the gallery by recency without disturbing the roster", () => {
    const slots = seatedRoster();
    const gallery = new GalleryDirector({ cells: 4 });
    gallery.resetFromSlots(slots.slots());
    const before = gallery.cells().map((cell) => cell.slot);

    const scores = new RecencyScores();
    scores.onActiveSpeaker("z-bo");
    const order = scores
      .order(["z-host", "z-reader", "z-ann", "z-bo"])
      .map((id) => slots.slotOf(id))
      .filter((slot): slot is number => slot !== null);

    gallery.applyOrder(order);
    expect(gallery.cells()[0]?.slot).toBe(slots.slotOf("z-bo"));
    expect(gallery.cells().map((cell) => cell.slot)).not.toEqual(before);
    expect(order[0]).toBe(slots.slotOf("z-bo"));
  });

  it("keeps the most recent speakers in a limited position pool", () => {
    const slots = seatedRoster();
    const filo = new FiloAssigner({ capacity: 2 });
    for (const id of ["z-host", "z-ann", "z-bo"]) filo.onActiveSpeaker(id);

    const occupants = [...filo.positions().values()];
    expect(occupants).toContain("z-bo");
    expect(occupants).toContain("z-ann");
    expect(occupants).not.toContain("z-host");
    expect(slots.slotOf("z-host")).not.toBeNull();
  });
});
