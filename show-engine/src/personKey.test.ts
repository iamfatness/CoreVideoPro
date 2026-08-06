import { describe, expect, it } from "vitest";
import { personKeyForPin, resolvePersonKey } from "./personKey.js";

describe("resolvePersonKey", () => {
  it("prefers the PIN when the name carries one", () => {
    expect(resolvePersonKey({ participantId: "z1", rawName: "Ann Lee | 4242" })).toBe("pin:4242");
  });

  it("falls back to the normalized display name", () => {
    expect(resolvePersonKey({ participantId: "z1", rawName: "Ann Lee" })).toBe("name:ann lee");
  });

  it("normalizes case and whitespace so a retyped name still matches", () => {
    const a = resolvePersonKey({ participantId: "z1", rawName: "Ann   Lee" });
    const b = resolvePersonKey({ participantId: "z9", rawName: "  ann lee  " });
    expect(a).toBe(b);
  });

  it("gives the same key to the same PIN regardless of surrounding name", () => {
    const a = resolvePersonKey({ participantId: "z1", rawName: "Ann Lee | 4242" });
    const b = resolvePersonKey({ participantId: "z9", rawName: "A. Lee | 4242 | Austin" });
    expect(a).toBe(b);
  });

  it("falls through to participantId when the name is empty", () => {
    expect(resolvePersonKey({ participantId: "z1", rawName: "" })).toBe("id:z1");
  });

  it("falls through to participantId when the name is only whitespace", () => {
    expect(resolvePersonKey({ participantId: "z1", rawName: "   " })).toBe("id:z1");
  });

  it("keeps tiers distinct so a participantId cannot collide with a name", () => {
    const byName = resolvePersonKey({ participantId: "z1", rawName: "bob" });
    const byId = resolvePersonKey({ participantId: "bob", rawName: "" });
    expect(byName).not.toBe(byId);
  });

  it("keeps tiers distinct so a PIN cannot collide with a name", () => {
    const byPin = resolvePersonKey({ participantId: "z1", rawName: "Ann | 4242" });
    const byName = resolvePersonKey({ participantId: "z2", rawName: "4242x" });
    expect(byPin).not.toBe(byName);
  });

  it("gives two different people with the same name the same key, by design", () => {
    const a = resolvePersonKey({ participantId: "z1", rawName: "John Smith" });
    const b = resolvePersonKey({ participantId: "z2", rawName: "John Smith" });
    expect(a).toBe(b);
  });
});

describe("personKeyForPin", () => {
  /**
   * The registry is PIN-keyed and the override table is person-keyed, so
   * these two must agree exactly or a registry-declared host can never be
   * demoted. Sharing the helper is what keeps them agreeing; this pins it.
   */
  it("produces the same key resolvePersonKey gives a participant with that PIN", () => {
    expect(personKeyForPin("1383")).toBe(
      resolvePersonKey({ participantId: "z1", rawName: "J.J. | 1383" })
    );
  });

  it("stays inside the PIN tier, so it cannot collide with a name key", () => {
    expect(personKeyForPin("4242")).not.toBe(
      resolvePersonKey({ participantId: "z2", rawName: "Ann Lee" })
    );
  });
});
