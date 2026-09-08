import { describe, expect, it } from "vitest";
import { StdioHostAdapter, WINDOWS_SHELL_CAPABILITIES } from "./stdioHostAdapter.js";

function rig() {
  const lines: string[] = [];
  const host = new StdioHostAdapter({ sink: (l) => lines.push(l), generation: 3, capabilities: WINDOWS_SHELL_CAPABILITIES });
  return { host, parsed: () => lines.map((l) => JSON.parse(l) as { event: string; generation: number; seq: number; name: string; args: unknown[] }) };
}

describe("StdioHostAdapter", () => {
  it("emits one hostCommand per call, in order, with the generation and a 1-based seq", () => {
    const { host, parsed } = rig();
    host.assignSlot(2, "p-9");
    host.cut();
    host.auto();
    host.setQuestion(null);
    expect(parsed()).toEqual([
      { event: "hostCommand", generation: 3, seq: 1, name: "assignSlot", args: [2, "p-9"] },
      { event: "hostCommand", generation: 3, seq: 2, name: "cut", args: [] },
      { event: "hostCommand", generation: 3, seq: 3, name: "auto", args: [null] },
      { event: "hostCommand", generation: 3, seq: 4, name: "setQuestion", args: [null] }
    ]);
  });

  it("serializes applyLook boxes and setGallery cells as pair arrays", () => {
    const { host, parsed } = rig();
    host.applyLook({ lookId: "l", scenePreset: "s", hostSlot: null, readerSlot: 4, boxes: new Map([[1, 7]]) });
    host.setGallery(new Map([[1, 2], [2, 0]]));
    expect(parsed()[0].args[0]).toEqual({ lookId: "l", scenePreset: "s", hostSlot: null, readerSlot: 4, boxes: [[1, 7]] });
    expect(parsed()[1].args[0]).toEqual([[1, 2], [2, 0]]);
  });

  it("reports the capabilities it was constructed with, unchanged", () => {
    const { host } = rig();
    expect(host.capabilities()).toEqual({ hasPreviewBus: true, maxGalleryCells: 16, transitions: ["cut", "fade", "dip", "wipe"] });
  });
});
