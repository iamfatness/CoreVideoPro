import { describe, expect, it } from "vitest";
import { OverlayDirector } from "./overlayDirector.js";
import type { Headline, MukanaQuestion } from "./contracts.js";
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

const headline: Headline = { name: "Ohio Humanities Gathering", location: "Columbus, OH" };

const hidden = {
  look: null,
  question: null,
  questionVisible: false,
  headline: null,
  headlineVisible: false
};

describe("OverlayDirector", () => {
  it("starts empty", () => {
    expect(new OverlayDirector().state()).toEqual({
      nameplates: [],
      question: null,
      headline: null,
      headlineVisible: false
    });
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
    expect(director.update({ ...hidden, question, questionVisible: true })).toBe(true);
    expect(director.state().question).toEqual({
      askerName: "Douglas",
      text: "Why does it work?",
      tag: "General",
      votes: 4
    });
  });

  it("hides the question when not visible", () => {
    const director = new OverlayDirector();
    director.update({ ...hidden, question, questionVisible: true });
    expect(director.update({ ...hidden, question, questionVisible: false })).toBe(true);
    expect(director.state().question).toBeNull();
  });

  it("shows no question when there is none", () => {
    const director = new OverlayDirector();
    expect(director.update({ ...hidden, question: null, questionVisible: true })).toBe(false);
    expect(director.state().question).toBeNull();
  });

  it("shows no question when its text is empty", () => {
    const director = new OverlayDirector();
    expect(
      director.update({ ...hidden, question: { ...question, text: "" }, questionVisible: true })
    ).toBe(false);
    expect(director.state().question).toBeNull();
  });

  it("detects a changed question", () => {
    const director = new OverlayDirector();
    director.update({ ...hidden, question, questionVisible: true });
    expect(
      director.update({
        ...hidden,
        question: { ...question, text: "A different question?" },
        questionVisible: true
      })
    ).toBe(true);
  });

  it("detects a changed vote count", () => {
    const director = new OverlayDirector();
    director.update({ ...hidden, question, questionVisible: true });
    expect(
      director.update({ ...hidden, question: { ...question, votes: 5 }, questionVisible: true })
    ).toBe(true);
  });

  it("ignores fields it does not draw", () => {
    const director = new OverlayDirector();
    director.update({ ...hidden, question, questionVisible: true });
    expect(
      director.update({
        ...hidden,
        question: { ...question, key: "-different", timestampMs: 999 },
        questionVisible: true
      })
    ).toBe(false);
  });

  it("empties on reset", () => {
    const director = new OverlayDirector();
    director.update({ ...hidden, look, question, questionVisible: true, headline, headlineVisible: true });
    director.reset();
    expect(director.state()).toEqual({
      nameplates: [],
      question: null,
      headline: null,
      headlineVisible: false
    });
  });

  it("returns copies so callers cannot mutate internal state", () => {
    const director = new OverlayDirector();
    director.update({ ...hidden, look });
    const plates = director.state().nameplates;
    const first = plates[0];
    if (first !== undefined) first.name = "hacked";
    expect(director.state().nameplates[0]?.name).toBe("J.J.");
  });

  describe("headline", () => {
    it("publishes an operator headline when visible", () => {
      const director = new OverlayDirector();
      expect(director.update({ ...hidden, headline, headlineVisible: true })).toBe(true);
      expect(director.state()).toEqual({
        nameplates: [],
        question: null,
        headline,
        headlineVisible: true
      });
    });

    it("reports no change when the headline is identical (neither text nor visibility moved)", () => {
      const director = new OverlayDirector();
      director.update({ ...hidden, headline, headlineVisible: true });
      expect(director.update({ ...hidden, headline: { ...headline }, headlineVisible: true })).toBe(
        false
      );
    });

    it("detects a changed headline text with visibility unchanged", () => {
      const director = new OverlayDirector();
      director.update({ ...hidden, headline, headlineVisible: true });
      const renamed: Headline = { ...headline, name: "Special Panel" };
      expect(director.update({ ...hidden, headline: renamed, headlineVisible: true })).toBe(true);
      expect(director.state().headline).toEqual(renamed);
    });

    it("detects a changed headline location with visibility unchanged", () => {
      const director = new OverlayDirector();
      director.update({ ...hidden, headline, headlineVisible: true });
      const relocated: Headline = { ...headline, location: "Cleveland, OH" };
      expect(director.update({ ...hidden, headline: relocated, headlineVisible: true })).toBe(true);
    });

    it("detects a visibility change with identical text", () => {
      const director = new OverlayDirector();
      director.update({ ...hidden, headline, headlineVisible: true });
      expect(director.update({ ...hidden, headline, headlineVisible: false })).toBe(true);
    });

    it("detects both text and visibility changing together", () => {
      const director = new OverlayDirector();
      director.update({ ...hidden, headline, headlineVisible: true });
      const renamed: Headline = { ...headline, name: "Special Panel" };
      expect(director.update({ ...hidden, headline: renamed, headlineVisible: false })).toBe(true);
    });

    it("retains the headline text when visibility toggles off, so it is not lost mid-show", () => {
      const director = new OverlayDirector();
      director.update({ ...hidden, headline, headlineVisible: true });
      director.update({ ...hidden, headline, headlineVisible: false });
      expect(director.state().headline).toEqual(headline);
      expect(director.state().headlineVisible).toBe(false);
    });

    it("restores the same headline content when visibility toggles back on", () => {
      const director = new OverlayDirector();
      director.update({ ...hidden, headline, headlineVisible: true });
      director.update({ ...hidden, headline, headlineVisible: false });
      expect(director.update({ ...hidden, headline, headlineVisible: true })).toBe(true);
      expect(director.state()).toEqual({
        nameplates: [],
        question: null,
        headline,
        headlineVisible: true
      });
    });

    it("clones the headline on ingest so mutating the caller's object afterward does not affect internal state", () => {
      const director = new OverlayDirector();
      const mutable: Headline = { name: "Original", location: "Columbus, OH" };
      director.update({ ...hidden, headline: mutable, headlineVisible: true });
      mutable.name = "hacked";
      mutable.location = "hacked";
      expect(director.state().headline).toEqual({ name: "Original", location: "Columbus, OH" });
    });

    it("returns copies on egress so mutating the returned headline does not affect internal state", () => {
      const director = new OverlayDirector();
      director.update({ ...hidden, headline, headlineVisible: true });
      const returned = director.state().headline;
      if (returned === null) throw new Error("fixture");
      returned.name = "hacked";
      expect(director.state().headline?.name).toBe(headline.name);
    });

    it("still reports a change from nameplates alone when the headline is untouched", () => {
      const director = new OverlayDirector();
      director.update({ ...hidden, headline, headlineVisible: true });
      expect(director.update({ ...hidden, look, headline, headlineVisible: true })).toBe(true);
      expect(director.state().headline).toEqual(headline);
      expect(director.state().nameplates).toEqual(look.nameplates);
    });
  });
});
