import { describe, expect, it } from "vitest";
import { OverlayDirector } from "./overlayDirector.js";
import type { MukanaQuestion } from "./contracts.js";
import type { LookResolution } from "./lookDirector.js";

const look: LookResolution = {
  lookId: "hr",
  scenePreset: "scene-hr",
  plateTone: "accent",
  tallySource: "boxes",
  hostSlot: 1,
  readerSlot: null,
  boxes: [{ box: 1, slot: 3 }],
  nameplates: [
    { position: { kind: "host" }, slot: 1, name: "J.J.", location: "CA", tone: "accent" },
    { position: { kind: "box", box: 1 }, slot: 3, name: "Ann", location: "TX", tone: "accent" }
  ],
  page: 0,
  pageCount: 1,
  boxFill: "queue"
};

const question: MukanaQuestion = {
  key: "-abc",
  askerName: "Douglas",
  text: "Why does it work?",
  tag: "General",
  votes: 4,
  timestampMs: 1635176445667
};

const hidden = { look: null, question: null, questionVisible: false };

describe("OverlayDirector", () => {
  it("starts empty", () => {
    expect(new OverlayDirector().state()).toEqual({ nameplates: [], question: null });
  });

  it("reports no change when the first update is empty", () => {
    expect(new OverlayDirector().update(hidden)).toBe(false);
  });

  it("publishes the look's nameplates", () => {
    const director = new OverlayDirector();
    expect(director.update({ ...hidden, look })).toBe(true);
    expect(director.state().nameplates).toEqual(look.nameplates);
  });

  it("clears nameplates when the look goes away", () => {
    const director = new OverlayDirector();
    director.update({ ...hidden, look });
    expect(director.update(hidden)).toBe(true);
    expect(director.state().nameplates).toEqual([]);
  });

  it("reports no change when nothing moved", () => {
    const director = new OverlayDirector();
    director.update({ ...hidden, look });
    expect(director.update({ ...hidden, look })).toBe(false);
  });

  it("detects a changed nameplate field", () => {
    const director = new OverlayDirector();
    director.update({ ...hidden, look });
    const [firstPlate, secondPlate] = look.nameplates;
    if (firstPlate === undefined || secondPlate === undefined) throw new Error("fixture");
    const renamed: LookResolution = {
      ...look,
      nameplates: [{ ...firstPlate, name: "J.J. Mc Kenna" }, secondPlate]
    };
    expect(director.update({ ...hidden, look: renamed })).toBe(true);
  });

  it("shows a question when visible", () => {
    const director = new OverlayDirector();
    expect(director.update({ look: null, question, questionVisible: true })).toBe(true);
    expect(director.state().question).toEqual({
      askerName: "Douglas",
      text: "Why does it work?",
      tag: "General",
      votes: 4
    });
  });

  it("hides the question when not visible", () => {
    const director = new OverlayDirector();
    director.update({ look: null, question, questionVisible: true });
    expect(director.update({ look: null, question, questionVisible: false })).toBe(true);
    expect(director.state().question).toBeNull();
  });

  it("shows no question when there is none", () => {
    const director = new OverlayDirector();
    expect(director.update({ look: null, question: null, questionVisible: true })).toBe(false);
    expect(director.state().question).toBeNull();
  });

  it("shows no question when its text is empty", () => {
    const director = new OverlayDirector();
    expect(
      director.update({ look: null, question: { ...question, text: "" }, questionVisible: true })
    ).toBe(false);
    expect(director.state().question).toBeNull();
  });

  it("detects a changed question", () => {
    const director = new OverlayDirector();
    director.update({ look: null, question, questionVisible: true });
    expect(
      director.update({
        look: null,
        question: { ...question, text: "A different question?" },
        questionVisible: true
      })
    ).toBe(true);
  });

  it("detects a changed vote count", () => {
    const director = new OverlayDirector();
    director.update({ look: null, question, questionVisible: true });
    expect(
      director.update({ look: null, question: { ...question, votes: 5 }, questionVisible: true })
    ).toBe(true);
  });

  it("ignores fields it does not draw", () => {
    const director = new OverlayDirector();
    director.update({ look: null, question, questionVisible: true });
    expect(
      director.update({
        look: null,
        question: { ...question, key: "-different", timestampMs: 999 },
        questionVisible: true
      })
    ).toBe(false);
  });

  it("empties on reset", () => {
    const director = new OverlayDirector();
    director.update({ look, question, questionVisible: true });
    director.reset();
    expect(director.state()).toEqual({ nameplates: [], question: null });
  });

  it("returns copies so callers cannot mutate internal state", () => {
    const director = new OverlayDirector();
    director.update({ ...hidden, look });
    const plates = director.state().nameplates;
    const first = plates[0];
    if (first !== undefined) first.name = "hacked";
    expect(director.state().nameplates[0]?.name).toBe("J.J.");
  });
});
