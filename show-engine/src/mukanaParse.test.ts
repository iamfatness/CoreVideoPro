import { describe, expect, it } from "vitest";
import { MukanaRegistry, parseMukanaPanelists, parseMukanaQuestion } from "./mukanaParse.js";

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

const questionBody = JSON.stringify({
  q: {
    key: "-Mms66PcbK_9cAj550wX",
    n: "Douglas Carmichael",
    q: "Do you think that adding\nslickness to an event\r\naffects community?",
    tag: "Zoom ISO",
    ts: 1635176445667,
    v: -1
  },
  hands: { prev: [], curr: [], next: [] }
});

describe("parseMukanaQuestion", () => {
  it("maps the question record", () => {
    const outcome = parseMukanaQuestion(questionBody);
    expect(outcome.kind).toBe("data");
    if (outcome.kind !== "data") return;
    expect(outcome.question).toEqual({
      key: "-Mms66PcbK_9cAj550wX",
      askerName: "Douglas Carmichael",
      text: "Do you think that adding slickness to an event affects community?",
      tag: "Zoom ISO",
      votes: -1,
      timestampMs: 1635176445667
    });
  });

  it("collapses newlines in the question text", () => {
    const outcome = parseMukanaQuestion(
      JSON.stringify({ q: { q: "one\n\ntwo\r\nthree  " } })
    );
    expect(outcome.kind).toBe("data");
    if (outcome.kind !== "data") return;
    expect(outcome.question?.text).toBe("one two three");
  });

  it("returns a null question when no q node is present", () => {
    const outcome = parseMukanaQuestion(JSON.stringify({ hands: { prev: [] } }));
    expect(outcome).toEqual({ kind: "data", question: null });
  });

  it("returns a null question when q is not an object", () => {
    expect(parseMukanaQuestion(JSON.stringify({ q: "nope" }))).toEqual({
      kind: "data",
      question: null
    });
  });

  it("defaults missing fields", () => {
    const outcome = parseMukanaQuestion(JSON.stringify({ q: { n: "Ann" } }));
    expect(outcome.kind).toBe("data");
    if (outcome.kind !== "data") return;
    expect(outcome.question).toEqual({
      key: "",
      askerName: "Ann",
      text: "",
      tag: "",
      votes: 0,
      timestampMs: 0
    });
  });

  it("treats the off-hours envelope as dormant", () => {
    const outcome = parseMukanaQuestion(
      JSON.stringify({ status: 200, detail: "outside show hours" })
    );
    expect(outcome).toEqual({ kind: "dormant", detail: "outside show hours" });
  });

  it("reports an unparseable body as invalid", () => {
    expect(parseMukanaQuestion("<html>502</html>").kind).toBe("invalid");
  });

  it("reports a non-object body as invalid", () => {
    expect(parseMukanaQuestion("[1,2,3]").kind).toBe("invalid");
  });
});
