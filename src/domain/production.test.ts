import { describe, expect, it } from "vitest";
import { initialProduction } from "./production";

describe("initialProduction", () => {
  it("starts with program and preview aligned for a safe first take", () => {
    expect(initialProduction.activeSceneId).toBe("speaker-slides");
    expect(initialProduction.previewSceneId).toBe("speaker-slides");
    expect(initialProduction.transition.style).toBe("fade");
    expect(initialProduction.transition.durationMs).toBe(420);
    expect(initialProduction.transition.statusText).toBe("Program ready");
  });
});
