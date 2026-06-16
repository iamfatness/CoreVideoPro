import { describe, expect, it } from "vitest";
import { initialProduction, initialParticipants } from "./production";

describe("initialProduction", () => {
  it("starts with program and preview aligned for a safe first take", () => {
    expect(initialProduction.activeSceneId).toBe("speaker-slides");
    expect(initialProduction.previewSceneId).toBe("speaker-slides");
    expect(initialProduction.transition.style).toBe("fade");
    expect(initialProduction.transition.durationMs).toBe(420);
    expect(initialProduction.transition.statusText).toBe("Program ready");
  });

  it("seeds seven Zoom sources with unique ids covering every production role", () => {
    expect(initialParticipants).toHaveLength(7);
    expect(new Set(initialParticipants.map((participant) => participant.id)).size).toBe(7);
    expect(new Set(initialParticipants.map((participant) => participant.role))).toEqual(
      new Set(["Host", "Presenter", "Panelist", "Guest"])
    );
  });
});
