import { describe, expect, it } from "vitest";
import { deriveTally, tallyEquals } from "./tallyPublisher.js";
import type { GalleryCell, Panelist, Slot } from "./contracts.js";
import type { LookResolution } from "./lookDirector.js";

function panelist(id: string, pin: string | null): Panelist {
  return {
    participantId: id,
    rawName: id,
    online: true,
    videoOn: true,
    audioOn: false,
    handRaised: false,
    zoomRole: 3,
    displayName: id,
    location: "",
    pin,
    hasMukana: pin !== null,
    role: "panelist"
  };
}

const slots: Slot[] = [
  { slot: 1, panelist: panelist("host", "1383") },
  { slot: 2, panelist: panelist("reader", "2001") },
  { slot: 3, panelist: panelist("ann", "4242") },
  { slot: 4, panelist: null },
  { slot: 5, panelist: panelist("walkin", null) }
];

const gallery: GalleryCell[] = [
  { cell: 1, slot: 3 },
  { cell: 2, slot: 0 },
  { cell: 3, slot: 4 },
  { cell: 4, slot: 1 }
];

const look: LookResolution = {
  lookId: "hr",
  scenePreset: "scene-hr",
  plateTone: "accent",
  hostSlot: 1,
  readerSlot: 2,
  boxes: [
    { box: 1, slot: 3 },
    { box: 2, slot: null }
  ],
  nameplates: [],
  page: 0,
  pageCount: 1
};

function base(overrides: Partial<Parameters<typeof deriveTally>[0]>) {
  return deriveTally({
    source: { kind: "black" },
    slots,
    gallery,
    look: null,
    activeSpeakerSlot: null,
    ...overrides
  });
}

describe("deriveTally", () => {
  it("reports nobody on black", () => {
    expect(base({})).toEqual({
      mode: "none",
      onAirSlots: [],
      onAirPins: [],
      onAirParticipantIds: []
    });
  });

  it("reports the single slot on a slot source", () => {
    expect(base({ source: { kind: "slot", slot: 3 } })).toEqual({
      mode: "slot",
      onAirSlots: [3],
      onAirPins: ["4242"],
      onAirParticipantIds: ["ann"]
    });
  });

  it("reports nobody when the selected slot is empty", () => {
    const tally = base({ source: { kind: "slot", slot: 4 } });
    expect(tally.mode).toBe("slot");
    expect(tally.onAirSlots).toEqual([]);
  });

  it("ignores an out-of-range slot rather than throwing", () => {
    expect(() => base({ source: { kind: "slot", slot: 99 } })).not.toThrow();
    expect(base({ source: { kind: "slot", slot: 99 } }).onAirSlots).toEqual([]);
  });

  it("follows the active speaker", () => {
    expect(base({ source: { kind: "activeSpeaker" }, activeSpeakerSlot: 3 })).toEqual({
      mode: "activeSpeaker",
      onAirSlots: [3],
      onAirPins: ["4242"],
      onAirParticipantIds: ["ann"]
    });
  });

  it("reports nobody when there is no active speaker", () => {
    expect(base({ source: { kind: "activeSpeaker" } }).onAirSlots).toEqual([]);
  });

  it("reports every occupied gallery cell, ascending and deduped", () => {
    const tally = base({ source: { kind: "gallery" } });
    expect(tally.mode).toBe("gallery");
    expect(tally.onAirSlots).toEqual([1, 3]);
    expect(tally.onAirPins).toEqual(["1383", "4242"]);
  });

  it("reports chairs and filled boxes for a boxes look", () => {
    const tally = base({ source: { kind: "look", lookId: "hr" }, look });
    expect(tally.mode).toBe("look");
    expect(tally.onAirSlots).toEqual([1, 2, 3]);
    expect(tally.onAirPins).toEqual(["1383", "2001", "4242"]);
  });

  it("reports nobody for an unresolved look", () => {
    const tally = base({ source: { kind: "look", lookId: "hr" }, look: null });
    expect(tally.mode).toBe("look");
    expect(tally.onAirSlots).toEqual([]);
  });

  it("includes a seated panelist with no PIN in slots but not in pins", () => {
    const tally = base({ source: { kind: "slot", slot: 5 } });
    expect(tally.onAirSlots).toEqual([5]);
    expect(tally.onAirParticipantIds).toEqual(["walkin"]);
    expect(tally.onAirPins).toEqual([]);
  });

  it("does not mutate its inputs", () => {
    const cells = [...gallery];
    base({ source: { kind: "gallery" }, gallery: cells });
    expect(cells).toEqual(gallery);
  });
});

describe("tallyEquals", () => {
  it("matches identical states", () => {
    const a = base({ source: { kind: "slot", slot: 3 } });
    const b = base({ source: { kind: "slot", slot: 3 } });
    expect(tallyEquals(a, b)).toBe(true);
  });

  it("distinguishes different modes", () => {
    expect(
      tallyEquals(base({}), base({ source: { kind: "slot", slot: 3 } }))
    ).toBe(false);
  });

  it("distinguishes different slot sets", () => {
    expect(
      tallyEquals(
        base({ source: { kind: "slot", slot: 3 } }),
        base({ source: { kind: "slot", slot: 1 } })
      )
    ).toBe(false);
  });
});
