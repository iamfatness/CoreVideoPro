import { describe, expect, it } from "vitest";
import { extractPin, identityFromName, splitDisplayName } from "./identity.js";

describe("extractPin", () => {
  it("finds a standalone 4-digit PIN", () => {
    expect(extractPin("Roy Meyers | 1383 | Forest Hill, MD")).toBe("1383");
  });

  it("returns the first PIN when several are present", () => {
    expect(extractPin("Ann 1383 Bee 4242")).toBe("1383");
  });

  it("returns null when there is no 4-digit token", () => {
    expect(extractPin("Roy Meyers | Forest Hill, MD")).toBeNull();
    expect(extractPin("Room 12345")).toBeNull();
    expect(extractPin("Desk 123")).toBeNull();
  });
});

describe("splitDisplayName", () => {
  it("takes the first segment as the name and the next non-PIN segment as location", () => {
    expect(splitDisplayName("Roy Meyers | dd02 | Forest Hill, MD | US", null)).toEqual({
      displayName: "Roy Meyers",
      location: "dd02"
    });
  });

  it("skips the PIN segment when choosing a location", () => {
    expect(splitDisplayName("J.J. Mc Kenna | 1383 | Santa Venetia, CA", "1383")).toEqual({
      displayName: "J.J. Mc Kenna",
      location: "Santa Venetia, CA"
    });
  });

  it("yields an empty location when the PIN is the only other segment", () => {
    expect(splitDisplayName("J.J. Mc Kenna | 1383", "1383")).toEqual({
      displayName: "J.J. Mc Kenna",
      location: ""
    });
  });

  it("accepts a slash separator", () => {
    expect(splitDisplayName("Ann Lee / Austin, TX", null)).toEqual({
      displayName: "Ann Lee",
      location: "Austin, TX"
    });
  });

  it("yields an empty location for a bare name", () => {
    expect(splitDisplayName("Ann Lee", null)).toEqual({ displayName: "Ann Lee", location: "" });
  });

  it("ignores empty segments from doubled separators", () => {
    expect(splitDisplayName("Ann Lee ||  Austin, TX", null)).toEqual({
      displayName: "Ann Lee",
      location: "Austin, TX"
    });
  });
});

describe("identityFromName", () => {
  it("combines PIN extraction and name splitting", () => {
    expect(identityFromName("J.J. Mc Kenna | 1383 | Santa Venetia, CA")).toEqual({
      displayName: "J.J. Mc Kenna",
      location: "Santa Venetia, CA",
      pin: "1383"
    });
  });

  it("handles an unregistered guest", () => {
    expect(identityFromName("Guest User")).toEqual({
      displayName: "Guest User",
      location: "",
      pin: null
    });
  });

  it("strips newlines from pasted names", () => {
    expect(identityFromName("Ann\nLee | Austin")).toEqual({
      displayName: "AnnLee",
      location: "Austin",
      pin: null
    });
  });
});
