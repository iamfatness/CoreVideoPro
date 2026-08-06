import { describe, expect, it } from "vitest";
import { MukanaRegistry, parseMukanaPanelists } from "./mukanaParse.js";

const panelistsBody = JSON.stringify({
  "0B6FTPaUEF": {
    displayName: "J.J. Mc Kenna",
    loc: "Santa Venetia, CA, US",
    pin: 1383,
    role: "host",
    online: true,
    uid: "0B6FTPaUEF"
  },
  ZZ9PluralZAlpha: {
    displayName: "Ann Lee",
    loc: "Austin, TX, US",
    pin: 4242,
    role: "panelist",
    online: false,
    uid: "ZZ9PluralZAlpha"
  }
});

describe("parseMukanaPanelists", () => {
  it("re-keys records by PIN", () => {
    const outcome = parseMukanaPanelists(panelistsBody);
    expect(outcome.kind).toBe("data");
    if (outcome.kind !== "data") return;
    expect(Object.keys(outcome.db).sort()).toEqual(["1383", "4242"]);
    expect(outcome.db["1383"]).toEqual({
      pin: "1383",
      displayName: "J.J. Mc Kenna",
      location: "Santa Venetia, CA, US",
      role: "host",
      online: true
    });
  });

  it("treats the off-hours status envelope as dormant", () => {
    const outcome = parseMukanaPanelists(
      JSON.stringify({
        status: 200,
        source: "/var/www/html/phpsdk/php-panel-rest.php",
        detail: "This page is only available between 1300 and 2000 UTC"
      })
    );
    expect(outcome.kind).toBe("dormant");
    if (outcome.kind !== "dormant") return;
    expect(outcome.detail).toMatch(/1300 and 2000 UTC/);
  });

  it("reports unparseable bodies as invalid", () => {
    const outcome = parseMukanaPanelists("<html>502 Bad Gateway</html>");
    expect(outcome.kind).toBe("invalid");
  });

  it("skips records without a PIN", () => {
    const outcome = parseMukanaPanelists(
      JSON.stringify({ uidA: { displayName: "No Pin", loc: "Nowhere", role: "panelist" } })
    );
    expect(outcome.kind).toBe("data");
    if (outcome.kind !== "data") return;
    expect(Object.keys(outcome.db)).toEqual([]);
  });

  it("coerces an unknown role to panelist", () => {
    const outcome = parseMukanaPanelists(
      JSON.stringify({ uidA: { displayName: "Odd", loc: "X", pin: 1111, role: "moderator" } })
    );
    expect(outcome.kind).toBe("data");
    if (outcome.kind !== "data") return;
    expect(outcome.db["1111"]?.role).toBe("panelist");
  });
});

describe("MukanaRegistry", () => {
  it("starts empty", () => {
    expect(new MukanaRegistry().current()).toEqual({});
  });

  it("merges successive fetches, with later data winning", () => {
    const registry = new MukanaRegistry();
    registry.merge({
      "1383": { pin: "1383", displayName: "J.J.", location: "CA", role: "host", online: true }
    });
    registry.merge({
      "1383": { pin: "1383", displayName: "J.J. Mc Kenna", location: "CA", role: "host", online: false },
      "4242": { pin: "4242", displayName: "Ann Lee", location: "TX", role: "panelist", online: true }
    });
    const db = registry.current();
    expect(db["1383"]?.displayName).toBe("J.J. Mc Kenna");
    expect(db["1383"]?.online).toBe(false);
    expect(Object.keys(db).sort()).toEqual(["1383", "4242"]);
  });

  it("retains records absent from a later fetch", () => {
    const registry = new MukanaRegistry();
    registry.merge({
      "1383": { pin: "1383", displayName: "J.J.", location: "CA", role: "host", online: true }
    });
    registry.merge({
      "4242": { pin: "4242", displayName: "Ann", location: "TX", role: "panelist", online: true }
    });
    expect(Object.keys(registry.current()).sort()).toEqual(["1383", "4242"]);
  });

  it("drops everything on purge", () => {
    const registry = new MukanaRegistry();
    registry.merge({
      "1383": { pin: "1383", displayName: "J.J.", location: "CA", role: "host", online: true }
    });
    registry.purge();
    expect(registry.current()).toEqual({});
  });

  it("returns a copy so callers cannot mutate internal state", () => {
    const registry = new MukanaRegistry();
    registry.merge({
      "1383": { pin: "1383", displayName: "J.J.", location: "CA", role: "host", online: true }
    });
    const db = registry.current();
    delete db["1383"];
    expect(Object.keys(registry.current())).toEqual(["1383"]);
  });

  it("protects against mutation of record fields from current()", () => {
    const registry = new MukanaRegistry();
    registry.merge({
      "1383": { pin: "1383", displayName: "J.J.", location: "CA", role: "host", online: true }
    });
    const db = registry.current();
    db["1383"]!.displayName = "MUTATED";
    expect(registry.current()["1383"]?.displayName).toBe("J.J.");
  });

  it("protects against mutation of records passed to merge()", () => {
    const registry = new MukanaRegistry();
    const incoming = {
      "1383": { pin: "1383", displayName: "J.J.", location: "CA", role: "host", online: true }
    };
    registry.merge(incoming);
    incoming["1383"]!.displayName = "MUTATED";
    expect(registry.current()["1383"]?.displayName).toBe("J.J.");
  });
});
