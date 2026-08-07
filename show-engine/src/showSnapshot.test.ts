import { describe, expect, it } from "vitest";
import { buildSnapshot } from "./showSnapshot.js";
import type { Panelist, QueueState, Slot } from "./contracts.js";
import type { LookResolution, ManualBoxAssignments, Nameplate } from "./lookDirector.js";

function sampleLook(): LookResolution {
  return {
    lookId: "two-box",
    scenePreset: "two-box",
    plateTone: "neutral",
    tallySource: "boxes",
    hostSlot: null,
    readerSlot: null,
    boxes: [{ box: 1, slot: 1 }],
    nameplates: [{ position: { kind: "host" }, slot: 1, name: "Bo", location: "", tone: "neutral" }],
    page: 0,
    pageCount: 1,
    boxFill: "queue"
  };
}

function panelist(id: string, name: string): Panelist {
  return {
    participantId: id,
    rawName: name,
    online: true,
    videoOn: true,
    audioOn: true,
    handRaised: false,
    zoomRole: 0,
    displayName: name,
    location: "",
    pin: null,
    hasMukana: false,
    role: "panelist",
    personKey: `name:${name.toLowerCase()}`
  };
}

function input() {
  const ann = panelist("p2", "Ann");
  const bo = panelist("p1", "Bo");
  const slots: Slot[] = [
    { slot: 1, panelist: bo },
    { slot: 2, panelist: null }
  ];
  const manualBoxes: ManualBoxAssignments = { 1: 4 };
  const tallyOnAirSlots: number[] = [];
  const overlayNameplates: Nameplate[] = [];
  const queue: QueueState = { previous: [], current: "4242", upcoming: ["5555"] };
  return {
    revision: 7,
    panelists: new Map([
      ["p2", ann],
      ["p1", bo]
    ]),
    slots,
    gallery: [{ cell: 1, slot: 1 }],
    queue,
    program: {
      program: { kind: "black" } as const,
      preview: { kind: "black" } as const,
      activeSpeakerFollow: false,
      activeSpeakerId: null
    },
    look: null as LookResolution | null,
    page: 0,
    manualBoxes,
    tally: { mode: "none" as const, onAirSlots: tallyOnAirSlots, onAirPins: [], onAirParticipantIds: [] },
    overlays: { nameplates: overlayNameplates, question: null },
    capabilities: {
      registry: { state: "disabled" as const, detail: null },
      handsQueue: { state: "disabled" as const, detail: null },
      questionFeed: { state: "disabled" as const, detail: null }
    },
    health: {
      panelists: { state: "ok" as const, consecutiveFailures: 0, detail: null },
      hands: { state: "ok" as const, consecutiveFailures: 0, detail: null },
      question: { state: "ok" as const, consecutiveFailures: 0, detail: null }
    },
    unseated: [panelist("p9", "Cy")]
  };
}

describe("buildSnapshot", () => {
  it("publishes the revision verbatim", () => {
    expect(buildSnapshot(input()).revision).toBe(7);
  });

  it("flattens the panelist map to an array sorted by participant id", () => {
    expect(buildSnapshot(input()).panelists.map((p) => p.participantId)).toEqual(["p1", "p2"]);
  });

  it("is deterministic across Map insertion order", () => {
    const a = input();
    const b = input();
    b.panelists = new Map([...b.panelists.entries()].reverse());
    expect(buildSnapshot(a)).toEqual(buildSnapshot(b));
  });

  /**
   * The invariant these must break on: a shallow copy. The engine keeps
   * mutating its live module state, and a surface may hold a snapshot across
   * ticks — a shared reference lets a later tick rewrite a snapshot that a
   * panel is mid-render on.
   */
  it("does not share the slot array with its caller", () => {
    const src = input();
    const snap = buildSnapshot(src);
    src.slots.push({ slot: 3, panelist: null });
    expect(snap.slots).toHaveLength(2);
  });

  it("does not share a seated panelist object with its caller", () => {
    const src = input();
    const snap = buildSnapshot(src);
    const seated = src.slots[0]?.panelist;
    if (seated) seated.displayName = "MUTATED";
    expect(snap.slots[0]?.panelist?.displayName).toBe("Bo");
  });

  it("does not share the queue arrays with its caller", () => {
    const src = input();
    const snap = buildSnapshot(src);
    src.queue.previous.push("1111");
    src.queue.upcoming.push("9999");
    expect(snap.queue.previous).toEqual([]);
    expect(snap.queue.upcoming).toEqual(["5555"]);
  });

  it("does not share the manual box assignments with its caller", () => {
    const src = input();
    const snap = buildSnapshot(src);
    src.manualBoxes[2] = 8;
    expect(snap.manualBoxes).toEqual({ 1: 4 });
  });

  it("does not share the gallery, tally, or overlay structures with its caller", () => {
    const src = input();
    const snap = buildSnapshot(src);
    src.gallery.push({ cell: 2, slot: 2 });
    src.tally.onAirSlots.push(1);
    src.overlays.nameplates.push({
      position: { kind: "host" },
      slot: 1,
      name: "X",
      location: "",
      tone: "neutral"
    });
    expect(snap.gallery).toHaveLength(1);
    expect(snap.tally.onAirSlots).toEqual([]);
    expect(snap.overlays.nameplates).toEqual([]);
  });

  it("does not share a look's box assignment with its caller", () => {
    const src = input();
    src.look = sampleLook();
    const snap = buildSnapshot(src);
    const box = src.look.boxes[0];
    if (box) box.slot = 99;
    expect(snap.look?.boxes[0]?.slot).toBe(1);
  });

  it("does not share a look's nameplate with its caller", () => {
    const src = input();
    src.look = sampleLook();
    const snap = buildSnapshot(src);
    const plate = src.look.nameplates[0];
    if (plate) plate.name = "MUTATED";
    expect(snap.look?.nameplates[0]?.name).toBe("Bo");
  });

  it("publishes the unseated panelists", () => {
    expect(buildSnapshot(input()).unseated.map((p) => p.participantId)).toEqual(["p9"]);
  });

  it("does not share the unseated array or its panelists with its caller", () => {
    const src = input();
    const snap = buildSnapshot(src);
    src.unseated.push(panelist("p10", "Dee"));
    const first = src.unseated[0];
    if (first) first.displayName = "MUTATED";
    expect(snap.unseated).toHaveLength(1);
    expect(snap.unseated[0]?.displayName).toBe("Cy");
  });
});
