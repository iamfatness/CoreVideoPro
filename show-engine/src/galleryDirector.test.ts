import { describe, expect, it } from "vitest";
import { GalleryDirector, GalleryError } from "./galleryDirector.js";
import type { Panelist, Slot } from "./contracts.js";
import { resolvePersonKey } from "./personKey.js";

function panelist(participantId: string): Panelist {
  return {
    participantId,
    rawName: participantId,
    personKey: resolvePersonKey({ participantId, rawName: participantId }),
    online: true,
    videoOn: true,
    audioOn: false,
    handRaised: false,
    zoomRole: 3,
    displayName: participantId,
    location: "",
    pin: null,
    hasMukana: false,
    role: "panelist"
  };
}

function slots(occupied: (string | null)[]): Slot[] {
  return occupied.map((id, index) => ({
    slot: index + 1,
    panelist: id === null ? null : panelist(id)
  }));
}

function slotNumbers(gallery: GalleryDirector): number[] {
  return gallery.cells().map((cell) => cell.slot);
}

describe("GalleryDirector", () => {
  it("rejects a cell count below one", () => {
    expect(() => new GalleryDirector({ cells: 0 })).toThrow(/cells/);
  });

  it("starts with every cell blank", () => {
    const gallery = new GalleryDirector({ cells: 4 });
    expect(gallery.cells()).toEqual([
      { cell: 1, slot: 0 },
      { cell: 2, slot: 0 },
      { cell: 3, slot: 0 },
      { cell: 4, slot: 0 }
    ]);
    expect(gallery.occupiedCount()).toBe(0);
  });

  it("packs occupied roster slots in ascending cell order", () => {
    const gallery = new GalleryDirector({ cells: 4 });
    gallery.resetFromSlots(slots(["a", null, "c", "d"]));
    expect(slotNumbers(gallery)).toEqual([1, 3, 4, 0]);
    expect(gallery.occupiedCount()).toBe(3);
  });

  it("stops packing when cells run out", () => {
    const gallery = new GalleryDirector({ cells: 2 });
    gallery.resetFromSlots(slots(["a", "b", "c"]));
    expect(slotNumbers(gallery)).toEqual([1, 2]);
  });

  it("blanks leftover cells on reset", () => {
    const gallery = new GalleryDirector({ cells: 3 });
    gallery.replace(3, 9);
    gallery.resetFromSlots(slots(["a"]));
    expect(slotNumbers(gallery)).toEqual([1, 0, 0]);
  });

  it("replaces a single cell", () => {
    const gallery = new GalleryDirector({ cells: 3 });
    gallery.replace(2, 7);
    expect(slotNumbers(gallery)).toEqual([0, 7, 0]);
  });

  it("allows the same roster slot on two cells", () => {
    const gallery = new GalleryDirector({ cells: 3 });
    gallery.replace(1, 5);
    gallery.replace(2, 5);
    expect(slotNumbers(gallery)).toEqual([5, 5, 0]);
  });

  it("blanks one cell on remove without compacting", () => {
    const gallery = new GalleryDirector({ cells: 4 });
    gallery.resetFromSlots(slots(["a", "b", "c"]));
    gallery.remove(2);
    expect(slotNumbers(gallery)).toEqual([1, 0, 3, 0]);
  });

  it("blanks everything on empty", () => {
    const gallery = new GalleryDirector({ cells: 3 });
    gallery.resetFromSlots(slots(["a", "b"]));
    gallery.empty();
    expect(slotNumbers(gallery)).toEqual([0, 0, 0]);
  });

  it("rearranges to a given slot order", () => {
    const gallery = new GalleryDirector({ cells: 4 });
    gallery.resetFromSlots(slots(["a", "b", "c"]));
    gallery.applyOrder([3, 1, 2]);
    expect(slotNumbers(gallery)).toEqual([3, 1, 2, 0]);
  });

  it("skips blank entries in an applied order", () => {
    const gallery = new GalleryDirector({ cells: 3 });
    gallery.applyOrder([2, 0, 5]);
    expect(slotNumbers(gallery)).toEqual([2, 5, 0]);
  });

  it("blanks cells past the end of an applied order", () => {
    const gallery = new GalleryDirector({ cells: 3 });
    gallery.resetFromSlots(slots(["a", "b", "c"]));
    gallery.applyOrder([2]);
    expect(slotNumbers(gallery)).toEqual([2, 0, 0]);
  });

  it("rejects out-of-range cells and negative slots", () => {
    const gallery = new GalleryDirector({ cells: 2 });
    expect(() => gallery.replace(0, 1)).toThrow(/cell/);
    expect(() => gallery.replace(3, 1)).toThrow(/cell/);
    expect(() => gallery.remove(3)).toThrow(/cell/);
    expect(() => gallery.replace(1, -1)).toThrow(/slot/);
  });

  it("compacts blanks away in the smart view", () => {
    const gallery = new GalleryDirector({ cells: 4 });
    gallery.resetFromSlots(slots(["a", "b", "c"]));
    gallery.remove(2);
    expect(gallery.smartCells()).toEqual([
      { cell: 1, slot: 1 },
      { cell: 2, slot: 3 }
    ]);
    expect(slotNumbers(gallery)).toEqual([1, 0, 3, 0]);
  });

  it("returns an empty smart view when nothing is assigned", () => {
    expect(new GalleryDirector({ cells: 3 }).smartCells()).toEqual([]);
  });

  it("returns copies so callers cannot mutate internal state", () => {
    const gallery = new GalleryDirector({ cells: 2 });
    gallery.replace(1, 4);
    const view = gallery.cells();
    view[0] = { cell: 1, slot: 99 };
    expect(gallery.cells()[0]).toEqual({ cell: 1, slot: 4 });
  });

  it("round-trips through JSON", () => {
    const gallery = new GalleryDirector({ cells: 4 });
    gallery.resetFromSlots(slots(["a", "b", "c"]));
    gallery.remove(2);
    const restored = GalleryDirector.fromJSON(gallery.toJSON(), { cells: 4 });
    expect(restored.cells()).toEqual(gallery.cells());
  });

  it("rejects a persisted state that disagrees with the configuration", () => {
    const gallery = new GalleryDirector({ cells: 4 });
    expect(() => GalleryDirector.fromJSON(gallery.toJSON(), { cells: 8 })).toThrow(GalleryError);
  });

  it("rejects a persisted state with a bad assignment list", () => {
    const gallery = new GalleryDirector({ cells: 4 });
    const state = gallery.toJSON();
    expect(() =>
      GalleryDirector.fromJSON({ ...state, assignments: state.assignments.slice(1) }, { cells: 4 })
    ).toThrow(GalleryError);
    expect(() =>
      GalleryDirector.fromJSON(
        { ...state, assignments: [{ cell: 9, slot: 0 }, ...state.assignments.slice(1)] },
        { cells: 4 }
      )
    ).toThrow(GalleryError);
  });

  it("rejects a persisted state of a foreign version", () => {
    const gallery = new GalleryDirector({ cells: 2 });
    const state = { ...gallery.toJSON(), version: 2 } as unknown as ReturnType<
      GalleryDirector["toJSON"]
    >;
    expect(() => GalleryDirector.fromJSON(state, { cells: 2 })).toThrow(GalleryError);
  });
});
