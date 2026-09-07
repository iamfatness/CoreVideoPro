import { describe, expect, it } from "vitest";
import { HostCommandEmitter } from "./hostCommands.js";
import { MockHost } from "./mockHost.js";
import type { GalleryCell, Panelist, Slot } from "./contracts.js";
import type { LookResolution } from "./lookDirector.js";
import type { OverlayState } from "./overlayDirector.js";

function panelist(participantId: string, online = true): Panelist {
  return {
    participantId,
    rawName: participantId,
    online,
    videoOn: online,
    audioOn: online,
    handRaised: false,
    zoomRole: 0,
    displayName: participantId,
    location: "",
    pin: null,
    hasMukana: false,
    role: "panelist",
    personKey: `zoom:${participantId}`
  };
}

function slots(occupants: Record<number, Panelist | null>, capacity = 3): Slot[] {
  return Array.from({ length: capacity }, (_, index) => {
    const slot = index + 1;
    return { slot, panelist: occupants[slot] ?? null };
  });
}

const look: LookResolution = {
  lookId: "teatime",
  scenePreset: "scene-teatime",
  plateTone: "neutral",
  tallySource: "boxes",
  hostSlot: 1,
  readerSlot: null,
  boxes: [{ box: 1, slot: 2 }],
  nameplates: [],
  page: 0,
  pageCount: 1,
  boxFill: "queue"
};

const gallery: GalleryCell[] = [
  { cell: 1, slot: 1 },
  { cell: 2, slot: 0 }
];

const overlays: OverlayState = {
  nameplates: [],
  question: null,
  headline: null,
  headlineVisible: false
};

describe("HostCommandEmitter.emitSlots", () => {
  it("emits every slot on the first call", () => {
    const host = new MockHost();
    const emitter = new HostCommandEmitter(host);
    emitter.emitSlots(slots({ 1: panelist("p1") }));
    expect(host.callsOfKind("assignSlot")).toEqual([
      { kind: "assignSlot", slot: 1, participantId: "p1" },
      { kind: "assignSlot", slot: 2, participantId: null },
      { kind: "assignSlot", slot: 3, participantId: null }
    ]);
  });

  it("emits nothing on a repeat call with the same occupants", () => {
    const host = new MockHost();
    const emitter = new HostCommandEmitter(host);
    emitter.emitSlots(slots({ 1: panelist("p1") }));
    host.clear();
    emitter.emitSlots(slots({ 1: panelist("p1") }));
    expect(host.calls()).toEqual([]);
  });

  it("emits only the slot whose occupant changed", () => {
    const host = new MockHost();
    const emitter = new HostCommandEmitter(host);
    emitter.emitSlots(slots({ 1: panelist("p1"), 2: panelist("p2") }));
    host.clear();
    emitter.emitSlots(slots({ 1: panelist("p1"), 2: panelist("p2", false) }));
    expect(host.callsOfKind("assignSlot")).toEqual([
      { kind: "assignSlot", slot: 2, participantId: null }
    ]);
  });
});

describe("HostCommandEmitter.emitLook", () => {
  it("does nothing for a null look", () => {
    const host = new MockHost();
    const emitter = new HostCommandEmitter(host);
    emitter.emitLook(null);
    expect(host.calls()).toEqual([]);
  });

  it("emits applyLook on the first resolved look, carrying the scene preset and both chairs", () => {
    const host = new MockHost();
    const emitter = new HostCommandEmitter(host);
    emitter.emitLook(look);
    expect(host.callsOfKind("applyLook")).toEqual([
      {
        kind: "applyLook",
        lookId: "teatime",
        scenePreset: "scene-teatime",
        hostSlot: 1,
        readerSlot: null,
        boxes: [[1, 2]]
      }
    ]);
  });

  it("does not re-emit an unchanged look", () => {
    const host = new MockHost();
    const emitter = new HostCommandEmitter(host);
    emitter.emitLook(look);
    host.clear();
    emitter.emitLook({ ...look });
    expect(host.calls()).toEqual([]);
  });

  it("re-emits when a box assignment changes", () => {
    const host = new MockHost();
    const emitter = new HostCommandEmitter(host);
    emitter.emitLook(look);
    host.clear();
    emitter.emitLook({ ...look, boxes: [{ box: 1, slot: 3 }] });
    expect(host.callsOfKind("applyLook")).toEqual([
      {
        kind: "applyLook",
        lookId: "teatime",
        scenePreset: "scene-teatime",
        hostSlot: 1,
        readerSlot: null,
        boxes: [[1, 3]]
      }
    ]);
  });

  it("re-emits when the look id changes even with the same boxes", () => {
    const host = new MockHost();
    const emitter = new HostCommandEmitter(host);
    emitter.emitLook(look);
    host.clear();
    emitter.emitLook({ ...look, lookId: "banter" });
    expect(host.callsOfKind("applyLook")).toHaveLength(1);
  });

  /**
   * The invariant this must break on: diffing `applyLook` only on `lookId`
   * (and boxes). A host chair move is a real placement change a host adapter
   * must act on — an unchanged `lookId` must not swallow it.
   */
  it("re-emits when the host chair moves, even with an unchanged look id and boxes", () => {
    const host = new MockHost();
    const emitter = new HostCommandEmitter(host);
    emitter.emitLook(look);
    host.clear();
    emitter.emitLook({ ...look, hostSlot: 3 });
    expect(host.callsOfKind("applyLook")).toEqual([
      {
        kind: "applyLook",
        lookId: "teatime",
        scenePreset: "scene-teatime",
        hostSlot: 3,
        readerSlot: null,
        boxes: [[1, 2]]
      }
    ]);
  });

  it("re-emits when the reader chair moves, even with an unchanged look id and boxes", () => {
    const host = new MockHost();
    const emitter = new HostCommandEmitter(host);
    emitter.emitLook(look);
    host.clear();
    emitter.emitLook({ ...look, readerSlot: 3 });
    expect(host.callsOfKind("applyLook")).toHaveLength(1);
  });

  /**
   * The invariant this must break on: diffing `applyLook` only on `lookId`
   * and `boxes`. A `scenePreset` swap with an unchanged look id and
   * unchanged boxes is exactly the case that widening this command exists
   * to carry — it must still emit.
   */
  it("re-emits when scenePreset changes, even with an unchanged look id and boxes", () => {
    const host = new MockHost();
    const emitter = new HostCommandEmitter(host);
    emitter.emitLook(look);
    host.clear();
    emitter.emitLook({ ...look, scenePreset: "scene-other" });
    expect(host.callsOfKind("applyLook")).toEqual([
      {
        kind: "applyLook",
        lookId: "teatime",
        scenePreset: "scene-other",
        hostSlot: 1,
        readerSlot: null,
        boxes: [[1, 2]]
      }
    ]);
  });
});

describe("HostCommandEmitter.emitGallery", () => {
  it("emits the whole map on the first call", () => {
    const host = new MockHost();
    const emitter = new HostCommandEmitter(host);
    emitter.emitGallery(gallery);
    expect(host.callsOfKind("setGallery")).toEqual([
      { kind: "setGallery", cells: [[1, 1], [2, 0]] }
    ]);
  });

  it("does not re-emit an unchanged gallery", () => {
    const host = new MockHost();
    const emitter = new HostCommandEmitter(host);
    emitter.emitGallery(gallery);
    host.clear();
    emitter.emitGallery([...gallery]);
    expect(host.calls()).toEqual([]);
  });

  it("re-emits when any cell changes", () => {
    const host = new MockHost();
    const emitter = new HostCommandEmitter(host);
    emitter.emitGallery(gallery);
    host.clear();
    emitter.emitGallery([{ cell: 1, slot: 1 }, { cell: 2, slot: 3 }]);
    expect(host.callsOfKind("setGallery")).toHaveLength(1);
  });
});

describe("HostCommandEmitter.emitOverlays", () => {
  it("emits both nameplates and the question when told the overlay changed", () => {
    const host = new MockHost();
    const emitter = new HostCommandEmitter(host);
    emitter.emitOverlays(overlays, true);
    expect(host.callsOfKind("setNameplates")).toHaveLength(1);
    expect(host.callsOfKind("setQuestion")).toHaveLength(1);
  });

  it("emits nothing when told the overlay is unchanged", () => {
    const host = new MockHost();
    const emitter = new HostCommandEmitter(host);
    emitter.emitOverlays(overlays, false);
    expect(host.calls()).toEqual([]);
  });
});
