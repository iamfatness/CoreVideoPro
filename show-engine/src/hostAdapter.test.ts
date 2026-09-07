import { describe, expect, it } from "vitest";
import { MockHost } from "./mockHost.js";

describe("MockHost", () => {
  it("defaults to a host with a preview bus and 16 gallery cells", () => {
    expect(new MockHost().capabilities()).toEqual({
      hasPreviewBus: true,
      maxGalleryCells: 16,
      transitions: ["cut", "fade"]
    });
  });

  it("merges a partial capability override over the defaults", () => {
    const host = new MockHost({ hasPreviewBus: false });
    expect(host.capabilities().hasPreviewBus).toBe(false);
    expect(host.capabilities().maxGalleryCells).toBe(16);
  });

  /**
   * The invariant this must break on: handing out the instance's own
   * capability record (final review, Minor). With no `transitions` override
   * that array WAS the shared module-level default, so one caller casting
   * away `readonly` and pushing would rewrite the defaults for every
   * `MockHost` in the process — including in other test files in the same
   * run. `transitions` is `readonly string[]`, so a cast is what a
   * conformance test would have to write to do it; the cast is the point.
   */
  it("hands out a copy of its capabilities, sharing nothing across instances", () => {
    const host = new MockHost();
    const read = host.capabilities();
    (read.transitions as string[]).push("wipe");
    read.maxGalleryCells = 1;

    expect(host.capabilities().transitions).toEqual(["cut", "fade"]);
    expect(host.capabilities().maxGalleryCells).toBe(16);
    expect(new MockHost().capabilities().transitions).toEqual(["cut", "fade"]);
  });

  it("records calls in arrival order", () => {
    const host = new MockHost();
    host.assignSlot(1, "p1");
    host.cut();
    host.assignSlot(2, null);
    expect(host.calls().map((c) => c.kind)).toEqual(["assignSlot", "cut", "assignSlot"]);
  });

  it("filters by kind with the narrowed element type", () => {
    const host = new MockHost();
    host.assignSlot(3, "p9");
    host.cut();
    const assigns = host.callsOfKind("assignSlot");
    expect(assigns).toHaveLength(1);
    expect(assigns[0]?.slot).toBe(3);
    expect(assigns[0]?.participantId).toBe("p9");
  });

  /**
   * The invariant this must break on: storing the caller's Map by reference.
   * The engine reuses its working maps between ticks, so a reference-storing
   * mock reports every historical call as the final state — turning a real
   * diffing bug into a green test.
   */
  it("snapshots map arguments so later mutation cannot rewrite history", () => {
    const host = new MockHost();
    const boxes = new Map<number, number | null>([[1, 4]]);
    host.applyLook({
      lookId: "teatime",
      scenePreset: "scene-teatime",
      hostSlot: 1,
      readerSlot: null,
      boxes
    });
    boxes.set(1, 7);
    boxes.set(2, 9);
    const recorded = host.callsOfKind("applyLook")[0];
    expect(recorded?.boxes).toEqual([[1, 4]]);
  });

  it("snapshots gallery maps the same way", () => {
    const host = new MockHost();
    const cells = new Map<number, number>([[1, 2]]);
    host.setGallery(cells);
    cells.set(1, 5);
    expect(host.callsOfKind("setGallery")[0]?.cells).toEqual([[1, 2]]);
  });

  it("clears recorded calls without clearing capabilities", () => {
    const host = new MockHost({ maxGalleryCells: 4 });
    host.cut();
    host.clear();
    expect(host.calls()).toEqual([]);
    expect(host.capabilities().maxGalleryCells).toBe(4);
  });

  it("records auto's transition id, including when omitted", () => {
    const host = new MockHost();
    host.auto();
    host.auto("fade");
    expect(host.callsOfKind("auto").map((c) => c.transitionId)).toEqual([undefined, "fade"]);
  });
});
