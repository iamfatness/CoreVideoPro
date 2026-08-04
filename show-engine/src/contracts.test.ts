import { describe, expect, it } from "vitest";
import { coerceRole, isRole, ROLES } from "./contracts.js";

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
