import { describe, expect, it } from "vitest";
import { OverrideDb } from "./overrideDb.js";
import type { MukanaDb } from "./contracts.js";

const mukana: MukanaDb = {
  "1383": {
    pin: "1383",
    displayName: "J.J. Mc Kenna",
    location: "Santa Venetia, CA",
    role: "host",
    online: true
  },
  "4242": {
    pin: "4242",
    displayName: "Ann Lee",
    location: "Austin, TX",
    role: "panelist",
    online: true
  },
  "5555": {
    pin: "5555",
    displayName: "Bo Diaz",
    location: "Lima, PE",
    role: "panelist",
    online: true
  }
};

describe("OverrideDb", () => {
  it("starts empty", () => {
    expect(new OverrideDb().entries()).toEqual({});
  });

  it("stores and reads back an override", () => {
    const db = new OverrideDb();
    db.set({
      personKey: "pin:4242",
      displayName: "Ann Lee",
      location: "Austin, TX",
      role: "aslinterpreter"
    });
    expect(db.roleFor("pin:4242")).toBe("aslinterpreter");
  });

  it("deletes an override", () => {
    const db = new OverrideDb();
    db.set({ personKey: "pin:4242", displayName: "Ann Lee", location: "Austin, TX", role: "reader" });
    db.delete("pin:4242");
    expect(db.roleFor("pin:4242")).toBeUndefined();
  });

  it("demotes a Mukana-declared host when another PIN is promoted", () => {
    const db = new OverrideDb();
    db.assignExclusiveRole("pin:4242", "host", mukana);
    expect(db.roleFor("pin:4242")).toBe("host");
    expect(db.roleFor("pin:1383")).toBe("panelist");
  });

  it("carries the demoted person's identity into the override row", () => {
    const db = new OverrideDb();
    db.assignExclusiveRole("pin:4242", "host", mukana);
    expect(db.entries()["pin:1383"]).toEqual({
      personKey: "pin:1383",
      displayName: "J.J. Mc Kenna",
      location: "Santa Venetia, CA",
      role: "panelist"
    });
  });

  it("deletes a previous override-only host rather than leaving a demotion row", () => {
    const db = new OverrideDb();
    db.assignExclusiveRole("pin:5555", "host", mukana);
    expect(db.roleFor("pin:5555")).toBe("host");

    db.assignExclusiveRole("pin:4242", "host", mukana);
    expect(db.roleFor("pin:4242")).toBe("host");
    expect(db.entries()["pin:5555"]).toBeUndefined();
  });

  it("leaves the reader alone when assigning a host", () => {
    const db = new OverrideDb();
    db.assignExclusiveRole("pin:5555", "reader", mukana);
    db.assignExclusiveRole("pin:4242", "host", mukana);
    expect(db.roleFor("pin:5555")).toBe("reader");
    expect(db.roleFor("pin:4242")).toBe("host");
  });

  it("yields exactly one holder of an exclusive role after repeated assignment", () => {
    const db = new OverrideDb();
    db.assignExclusiveRole("pin:4242", "host", mukana);
    db.assignExclusiveRole("pin:5555", "host", mukana);
    db.assignExclusiveRole("pin:1383", "host", mukana);
    const hosts = Object.values(db.entries()).filter((entry) => entry.role === "host");
    expect(hosts.map((entry) => entry.personKey)).toEqual(["pin:1383"]);
  });

  it("uses the Mukana identity when promoting an unknown-to-overrides PIN", () => {
    const db = new OverrideDb();
    db.assignExclusiveRole("pin:4242", "reader", mukana);
    expect(db.entries()["pin:4242"]).toEqual({
      personKey: "pin:4242",
      displayName: "Ann Lee",
      location: "Austin, TX",
      role: "reader"
    });
  });

  it("promotes a PIN absent from Mukana with empty identity fields", () => {
    const db = new OverrideDb();
    db.assignExclusiveRole("pin:7777", "host", mukana);
    expect(db.entries()["pin:7777"]).toEqual({
      personKey: "pin:7777",
      displayName: "",
      location: "",
      role: "host"
    });
  });

  it("restores a persisted table and clears on demand", () => {
    const db = new OverrideDb();
    db.restore({
      "pin:4242": { personKey: "pin:4242", displayName: "Ann Lee", location: "Austin, TX", role: "host" }
    });
    expect(db.roleFor("pin:4242")).toBe("host");
    db.clear();
    expect(db.entries()).toEqual({});
  });

  it("preserves an operator-entered identity for a non-Mukana PIN through exclusive assignment", () => {
    const db = new OverrideDb();
    db.set({ personKey: "pin:7777", displayName: "Guest Speaker", location: "Remote", role: "panelist" });
    db.assignExclusiveRole("pin:7777", "host", mukana);
    expect(db.entries()["pin:7777"]).toEqual({
      personKey: "pin:7777",
      displayName: "Guest Speaker",
      location: "Remote",
      role: "host"
    });
  });

  it("still prefers the Mukana identity over a stale prior override row", () => {
    const db = new OverrideDb();
    db.set({ personKey: "pin:5555", displayName: "Old Name", location: "Old Location", role: "panelist" });
    db.assignExclusiveRole("pin:5555", "host", mukana);
    expect(db.entries()["pin:5555"]).toEqual({
      personKey: "pin:5555",
      displayName: "Bo Diaz",
      location: "Lima, PE",
      role: "host"
    });
  });
});

describe("OverrideDb without a registry", () => {
  it("assigns a role to a name-keyed person", () => {
    const db = new OverrideDb();
    db.assignExclusiveRole("name:guest user", "host", {});
    expect(db.roleFor("name:guest user")).toBe("host");
  });

  it("still enforces one holder with no registry present", () => {
    const db = new OverrideDb();
    db.assignExclusiveRole("name:ann lee", "host", {});
    db.assignExclusiveRole("name:bo diaz", "host", {});
    const hosts = Object.values(db.entries()).filter((entry) => entry.role === "host");
    expect(hosts.map((entry) => entry.personKey)).toEqual(["name:bo diaz"]);
  });

  it("writes a demoted registry holder under its pin-prefixed key", () => {
    const registry: MukanaDb = {
      "1383": {
        pin: "1383",
        displayName: "J.J. Mc Kenna",
        location: "CA",
        role: "host",
        online: true
      }
    };
    const db = new OverrideDb();
    db.assignExclusiveRole("name:ann lee", "host", registry);
    expect(db.entries()["pin:1383"]).toEqual({
      personKey: "pin:1383",
      displayName: "J.J. Mc Kenna",
      location: "CA",
      role: "panelist"
    });
  });
});
