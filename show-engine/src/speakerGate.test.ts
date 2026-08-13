import { describe, expect, it } from "vitest";
import { shouldFollowSpeaker } from "./speakerGate.js";
import { DEFAULT_SKIP_ROLES } from "./contracts.js";

describe("shouldFollowSpeaker", () => {
  it("blocks a role in the skip list", () => {
    expect(shouldFollowSpeaker("aslinterpreter", DEFAULT_SKIP_ROLES)).toBe(false);
  });

  it("allows a role not in the skip list", () => {
    expect(shouldFollowSpeaker("panelist", DEFAULT_SKIP_ROLES)).toBe(true);
    expect(shouldFollowSpeaker("host", DEFAULT_SKIP_ROLES)).toBe(true);
  });

  it("allows an unseated speaker with no role", () => {
    expect(shouldFollowSpeaker(null, DEFAULT_SKIP_ROLES)).toBe(true);
  });

  it("honours a custom skip list", () => {
    expect(shouldFollowSpeaker("reader", ["reader"])).toBe(false);
    expect(shouldFollowSpeaker("aslinterpreter", ["reader"])).toBe(true);
  });

  it("allows everything when the skip list is empty", () => {
    expect(shouldFollowSpeaker("aslinterpreter", [])).toBe(true);
  });
});
