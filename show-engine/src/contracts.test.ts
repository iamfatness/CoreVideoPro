import { describe, expect, it } from "vitest";
import {
  BOX_FILLS,
  coerceRole,
  isBoxFill,
  isPlateTone,
  isRole,
  PLATE_TONES,
  programSourcesEqual,
  ROLES
} from "./contracts.js";

describe("roles", () => {
  it("lists the five editorial roles", () => {
    expect(ROLES).toEqual([
      "panelist",
      "host",
      "reader",
      "aslpanelist",
      "aslinterpreter"
    ]);
  });

  it("recognises valid roles", () => {
    expect(isRole("host")).toBe(true);
    expect(isRole("aslinterpreter")).toBe(true);
  });

  it("rejects unknown values", () => {
    expect(isRole("moderator")).toBe(false);
    expect(isRole(3)).toBe(false);
    expect(isRole(undefined)).toBe(false);
  });

  it("coerces unknown values to panelist", () => {
    expect(coerceRole("host")).toBe("host");
    expect(coerceRole("moderator")).toBe("panelist");
    expect(coerceRole(undefined)).toBe("panelist");
  });
});

describe("plate tones", () => {
  it("lists the four tones", () => {
    expect(PLATE_TONES).toEqual(["neutral", "accent", "guest", "breaking"]);
  });

  it("recognises valid tones and rejects others", () => {
    expect(isPlateTone("guest")).toBe(true);
    expect(isPlateTone("chartreuse")).toBe(false);
    expect(isPlateTone(2)).toBe(false);
    expect(isPlateTone(undefined)).toBe(false);
  });
});

describe("programSourcesEqual", () => {
  it("matches identical simple sources", () => {
    expect(programSourcesEqual({ kind: "gallery" }, { kind: "gallery" })).toBe(true);
    expect(programSourcesEqual({ kind: "black" }, { kind: "black" })).toBe(true);
  });

  it("distinguishes different kinds", () => {
    expect(programSourcesEqual({ kind: "gallery" }, { kind: "black" })).toBe(false);
  });

  it("compares slot numbers", () => {
    expect(programSourcesEqual({ kind: "slot", slot: 3 }, { kind: "slot", slot: 3 })).toBe(true);
    expect(programSourcesEqual({ kind: "slot", slot: 3 }, { kind: "slot", slot: 4 })).toBe(false);
  });

  it("compares look ids", () => {
    expect(
      programSourcesEqual({ kind: "look", lookId: "banter" }, { kind: "look", lookId: "banter" })
    ).toBe(true);
    expect(
      programSourcesEqual({ kind: "look", lookId: "banter" }, { kind: "look", lookId: "teatime" })
    ).toBe(false);
  });
});

describe("box fill strategies", () => {
  it("lists the two strategies", () => {
    expect(BOX_FILLS).toEqual(["queue", "manual"]);
  });

  it("recognises valid strategies and rejects others", () => {
    expect(isBoxFill("queue")).toBe(true);
    expect(isBoxFill("manual")).toBe(true);
    expect(isBoxFill("auto")).toBe(false);
    expect(isBoxFill(0)).toBe(false);
    expect(isBoxFill(undefined)).toBe(false);
  });
});
