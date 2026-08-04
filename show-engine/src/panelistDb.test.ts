import { describe, expect, it } from "vitest";
import { buildPanelistDb } from "./panelistDb.js";
import type { MukanaDb, Participant } from "./contracts.js";

function participant(participantId: string, rawName: string): Participant {
  return {
    participantId,
    rawName,
    online: true,
    videoOn: true,
    audioOn: false,
    handRaised: false,
    zoomRole: 3
  };
}

const mukana: MukanaDb = {
  "1383": {
    pin: "1383",
    displayName: "J.J. Mc Kenna",
    location: "Santa Venetia, CA, US",
    role: "host",
    online: true
  }
};

describe("buildPanelistDb", () => {
  it("uses Mukana identity and role when the PIN matches", () => {
    const db = buildPanelistDb([participant("p1", "JJ | 1383 | somewhere")], mukana, {});
    const panelist = db.get("p1");
    expect(panelist).toMatchObject({
      displayName: "J.J. Mc Kenna",
      location: "Santa Venetia, CA, US",
      pin: "1383",
      role: "host",
      hasMukana: true
    });
  });

  it("falls back to the parsed display name when there is no Mukana record", () => {
    const db = buildPanelistDb([participant("p2", "Guest User | Austin, TX")], mukana, {});
    expect(db.get("p2")).toMatchObject({
      displayName: "Guest User",
      location: "Austin, TX",
      pin: null,
      role: "panelist",
      hasMukana: false
    });
  });

  it("treats an unregistered PIN as no Mukana record", () => {
    const db = buildPanelistDb([participant("p3", "Ann Lee | 9999 | Austin")], mukana, {});
    expect(db.get("p3")).toMatchObject({ pin: "9999", hasMukana: false, role: "panelist" });
  });

  it("lets an override win over Mukana", () => {
    const db = buildPanelistDb([participant("p1", "JJ | 1383")], mukana, {
      "1383": {
        pin: "1383",
        displayName: "JJ (stand-in)",
        location: "Remote",
        role: "panelist"
      }
    });
    expect(db.get("p1")).toMatchObject({
      displayName: "JJ (stand-in)",
      location: "Remote",
      role: "panelist",
      hasMukana: true
    });
  });

  it("keeps Mukana identity when the override carries only a role", () => {
    const db = buildPanelistDb([participant("p1", "JJ | 1383")], mukana, {
      "1383": { pin: "1383", displayName: "", location: "", role: "reader" }
    });
    expect(db.get("p1")).toMatchObject({
      displayName: "J.J. Mc Kenna",
      location: "Santa Venetia, CA, US",
      role: "reader"
    });
  });

  it("preserves participant liveness fields", () => {
    const db = buildPanelistDb(
      [{ ...participant("p1", "JJ | 1383"), videoOn: false, handRaised: true, online: false }],
      mukana,
      {}
    );
    expect(db.get("p1")).toMatchObject({ videoOn: false, handRaised: true, online: false });
  });

  it("keys the database by participant id", () => {
    const db = buildPanelistDb(
      [participant("p1", "JJ | 1383"), participant("p2", "Ann Lee")],
      mukana,
      {}
    );
    expect([...db.keys()].sort()).toEqual(["p1", "p2"]);
  });

  it("returns an empty map for an empty roster", () => {
    expect(buildPanelistDb([], mukana, {}).size).toBe(0);
  });
});
