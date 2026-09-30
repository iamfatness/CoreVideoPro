import assert from "node:assert/strict";
import test from "node:test";
import { parseFfprobeFramePts } from "../qa/ffprobe-frame-pts.mjs";

test("SEI side-data labels do not turn continuous video into a false PTS gap", () => {
  const pts = parseFfprobeFramePts([
    "0.000000,H.26[45] User Data Unregistered SEI message",
    "0.016667,H.26[45] User Data Unregistered SEI message",
    "0.033333",
    "0.050000,H.26[45] User Data Unregistered SEI message",
  ].join("\r\n"));
  assert.equal(pts.length, 4);
  assert.ok(Math.max(...pts.slice(1).map((time, index) => (time - pts[index]) * 1000)) < 17);
});

test("missing or malformed FFprobe timestamps fail measurement", () => {
  assert.deepEqual(parseFfprobeFramePts("  "), []);
  assert.throws(() => parseFfprobeFramePts("0.000000\nN/A,side-data"), /invalid FFprobe frame timestamp/);
});
