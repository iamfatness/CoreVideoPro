import { test } from "node:test";
import assert from "node:assert/strict";
import { judge, anyFailed, formatTable } from "./validate-show-engine-judge.mjs";

function ohgActionIds() {
  // 28 real ohg.* action ids (show-engine/src/actions.ts), used so the "exactly 28" check
  // exercises the real contract rather than a synthetic fixture count.
  return [
    "ohg.panelist.add",
    "ohg.panelist.remove",
    "ohg.panelist.replace",
    "ohg.panelist.role.set",
    "ohg.panelist.syncAll",
    "ohg.program.preview",
    "ohg.program.cut",
    "ohg.program.auto",
    "ohg.program.directCut",
    "ohg.program.asFollow.set",
    "ohg.look.set",
    "ohg.look.nextGuest",
    "ohg.look.prevGuest",
    "ohg.look.box.assign",
    "ohg.look.box.clear",
    "ohg.gallery.resetFromSlots",
    "ohg.gallery.replace",
    "ohg.gallery.remove",
    "ohg.gallery.empty",
    "ohg.gallery.smart.set",
    "ohg.gfx.headline.in",
    "ohg.gfx.headline.out",
    "ohg.gfx.headline.change",
    "ohg.gfx.question.in",
    "ohg.gfx.question.out",
    "ohg.mukana.sync",
    "ohg.mukana.override.set",
    "ohg.mukana.override.delete",
  ];
}

function goodManifest(overrides = {}) {
  return {
    version: 1,
    root: "/cvp",
    actions: [
      { id: "recording.toggle" },
      ...ohgActionIds().map((id) => ({ id })),
    ],
    feedbackFields: ["recording", "ohg/health/engine", "ohg/shadow/lastCommand"],
    ...overrides,
  };
}

function goodStateBefore(overrides = {}) {
  return {
    ohg: { revision: 1, program: { program: { kind: "black" }, preview: { kind: "black" } } },
    ohgEngineHealth: "running",
    ...overrides,
  };
}

function goodInvokeResults(overrides = {}) {
  return {
    programPreviewBlack: { status: 200, body: { ok: true } },
    panelistRemove: { status: 422, body: { ok: false, error: "slot 42 is out of range 1..10" } },
    lookSet: { skipped: true, reason: "no configured-look list is exposed" },
    ...overrides,
  };
}

function goodStateAfter(overrides = {}) {
  return {
    ohg: { program: { program: { kind: "black" }, preview: { kind: "black" } } },
    ...overrides,
  };
}

function rowFor(rows, needle) {
  const row = rows.find((r) => r.name.includes(needle));
  assert.ok(row, `expected a row matching "${needle}"`);
  return row;
}

test("a fully healthy run: every asserted check PASSes, both look.set rows SKIP", () => {
  const rows = judge({
    manifest: goodManifest(),
    stateBefore: goodStateBefore(),
    invokeResults: goodInvokeResults(),
    stateAfter: goodStateAfter(),
  });

  assert.equal(rowFor(rows, "exactly 28 ohg.* actions").status, "PASS");
  assert.equal(rowFor(rows, "feedbackFields includes").status, "PASS");
  assert.equal(rowFor(rows, "ohg node is present").status, "PASS");
  assert.equal(rowFor(rows, 'ohgEngineHealth === "running"').status, "PASS");
  assert.equal(rowFor(rows, "invoke ohg.program.preview").status, "PASS");
  assert.equal(rowFor(rows, 'state: ohg.program.preview === {kind:"black"}').status, "PASS");
  assert.equal(rowFor(rows, "422 refusal").status, "PASS");
  assert.equal(rowFor(rows, "ohg.look.set (first configured look)").status, "SKIP");
  assert.equal(rowFor(rows, "cleared-box blankness").status, "SKIP");
  assert.equal(
    rowFor(rows, "cleared-box blankness").detail,
    "NOT JUDGEABLE from /state (needs a screenshot); see ledger",
  );
  assert.equal(anyFailed(rows), false);
});

test("wrong action count FAILs with the actual count in the detail", () => {
  const rows = judge({
    manifest: goodManifest({ actions: [{ id: "ohg.panelist.add" }, { id: "ohg.panelist.remove" }] }),
    stateBefore: goodStateBefore(),
    invokeResults: goodInvokeResults(),
    stateAfter: goodStateAfter(),
  });
  const row = rowFor(rows, "exactly 28 ohg.* actions");
  assert.equal(row.status, "FAIL");
  assert.match(row.detail, /found 2/);
  assert.equal(anyFailed(rows), true);
});

test("missing ohg/health/engine in feedbackFields FAILs", () => {
  const rows = judge({
    manifest: goodManifest({ feedbackFields: ["recording"] }),
    stateBefore: goodStateBefore(),
    invokeResults: goodInvokeResults(),
    stateAfter: goodStateAfter(),
  });
  assert.equal(rowFor(rows, "feedbackFields includes").status, "FAIL");
});

test("a null ohg node or non-running engine health FAILs both state checks", () => {
  const rows = judge({
    manifest: goodManifest(),
    stateBefore: { ohg: null, ohgEngineHealth: "stopped" },
    invokeResults: goodInvokeResults(),
    stateAfter: goodStateAfter(),
  });
  assert.equal(rowFor(rows, "ohg node is present").status, "FAIL");
  assert.equal(rowFor(rows, "ohgEngineHealth").status, "FAIL");
});

test("a non-200 or ok:false program.preview invoke FAILs", () => {
  const rows = judge({
    manifest: goodManifest(),
    stateBefore: goodStateBefore(),
    invokeResults: goodInvokeResults({ programPreviewBlack: { status: 400, body: { ok: false } } }),
    stateAfter: goodStateAfter(),
  });
  assert.equal(rowFor(rows, "invoke ohg.program.preview").status, "FAIL");
});

test("state not reflecting the black ProgramSource after the invoke FAILs", () => {
  const rows = judge({
    manifest: goodManifest(),
    stateBefore: goodStateBefore(),
    invokeResults: goodInvokeResults(),
    stateAfter: { ohg: { program: { program: { kind: "black" }, preview: { kind: "gallery" } } } },
  });
  const row = rowFor(rows, 'ohg.program.preview === {kind:"black"}');
  assert.equal(row.status, "FAIL");
  assert.match(row.detail, /"kind":"gallery"/);
});

test("a panelist.remove that unexpectedly succeeds (not a 422 refusal) FAILs", () => {
  const rows = judge({
    manifest: goodManifest(),
    stateBefore: goodStateBefore(),
    invokeResults: goodInvokeResults({ panelistRemove: { status: 200, body: { ok: true } } }),
    stateAfter: goodStateAfter(),
  });
  assert.equal(rowFor(rows, "422 refusal").status, "FAIL");
});

test("an attempted (non-skipped) look.set is judged as a real PASS/FAIL, not auto-skipped", () => {
  const passRows = judge({
    manifest: goodManifest(),
    stateBefore: goodStateBefore(),
    invokeResults: goodInvokeResults({
      lookSet: { skipped: false, status: 200, body: { ok: true } },
    }),
    stateAfter: goodStateAfter(),
  });
  assert.equal(rowFor(passRows, "ohg.look.set (first configured look)").status, "PASS");

  const failRows = judge({
    manifest: goodManifest(),
    stateBefore: goodStateBefore(),
    invokeResults: goodInvokeResults({
      lookSet: { skipped: false, status: 422, body: { ok: false, error: "unknown look" } },
    }),
    stateAfter: goodStateAfter(),
  });
  assert.equal(rowFor(failRows, "ohg.look.set (first configured look)").status, "FAIL");
});

test("formatTable renders one line per row including the header", () => {
  const rows = judge({
    manifest: goodManifest(),
    stateBefore: goodStateBefore(),
    invokeResults: goodInvokeResults(),
    stateAfter: goodStateAfter(),
  });
  const table = formatTable(rows);
  const lines = table.split("\n");
  assert.equal(lines.length, rows.length + 1);
  assert.match(lines[0], /STATUS/);
  assert.match(lines[0], /CHECK/);
});
